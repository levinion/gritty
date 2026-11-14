#include <doctest/doctest.h>
#include <png.h>
#include <cstdio>
#include <vector>
#include <filesystem>
#include "core/terminal_engine.hpp"
#include "core/vt_parser.hpp"
#include "render/font_engine.hpp"
#include "render/box_drawing.hpp"
#include "render/cpu_renderer.hpp"

using namespace gritty::core;
using namespace gritty::render;

namespace {

bool save_pixman_to_png(const char* filepath, pixman_image_t* img) {
    int width = pixman_image_get_width(img);
    int height = pixman_image_get_height(img);
    uint32_t* data = pixman_image_get_data(img);
    int stride = pixman_image_get_stride(img);

    FILE* fp = std::fopen(filepath, "wb");
    if (!fp) return false;

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr) { std::fclose(fp); return false; }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, nullptr);
        std::fclose(fp);
        return false;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        std::fclose(fp);
        return false;
    }

    png_init_io(png_ptr, fp);
    png_set_IHDR(png_ptr, info_ptr, width, height, 8,
                 PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png_ptr, info_ptr);
    png_set_bgr(png_ptr);

    std::vector<png_bytep> row_pointers(height);
    for (int y = 0; y < height; ++y) {
        row_pointers[y] = reinterpret_cast<png_bytep>(data + y * (stride / 4));
    }
    png_write_image(png_ptr, row_pointers.data());
    png_write_end(png_ptr, nullptr);

    png_destroy_write_struct(&png_ptr, &info_ptr);
    std::fclose(fp);
    return true;
}

std::string find_test_font() {
    const char* candidates[] = {
        "/System/Library/Fonts/Monaco.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "/Users/levin/Library/Fonts/JetBrainsMonoNerdFont-Regular.ttf",
        "/Library/Fonts/Arial Unicode.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
    };
    for (const char* path : candidates) {
        if (std::filesystem::exists(path)) {
            return path;
        }
    }
    return "";
}

} // namespace

TEST_CASE("BoxDrawingRenderer block elements") {
    int w = 10, h = 20;
    gritty::util::PixmanImage img(pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, nullptr, w * 4));
    CHECK(img.valid());

    // Clear to black
    pixman_color_t black = {0, 0, 0, 0xffff};
    pixman_rectangle16_t all = {0, 0, static_cast<uint16_t>(w), static_cast<uint16_t>(h)};
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, img.get(), &black, 1, &all);

    // Full block 0x2588 in white
    pixman_color_t white = {0xffff, 0xffff, 0xffff, 0xffff};
    bool ok = BoxDrawingRenderer::render(img.get(), 0x2588, 0, 0, w, h, white);
    CHECK(ok);

    uint32_t* pixels = img.data();
    // Top-left pixel should be white 0xFFFFFFFF
    CHECK(pixels[0] == 0xFFFFFFFF);
    // Center pixel should be white
    CHECK(pixels[(h / 2) * w + (w / 2)] == 0xFFFFFFFF);
    // Bottom-right pixel should be white
    CHECK(pixels[(h - 1) * w + (w - 1)] == 0xFFFFFFFF);
}

TEST_CASE("BoxDrawingRenderer half blocks") {
    int w = 10, h = 20;
    gritty::util::PixmanImage img(pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, nullptr, w * 4));

    pixman_color_t black = {0, 0, 0, 0xffff};
    pixman_rectangle16_t all = {0, 0, static_cast<uint16_t>(w), static_cast<uint16_t>(h)};
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, img.get(), &black, 1, &all);

    // Upper half block 0x2580 in red
    pixman_color_t red = {0xffff, 0, 0, 0xffff};
    bool ok = BoxDrawingRenderer::render(img.get(), 0x2580, 0, 0, w, h, red);
    CHECK(ok);

    uint32_t* pixels = img.data();
    // Pixel in top half (y=2, x=5) should be red
    CHECK(pixels[2 * w + 5] == 0xFFFF0000);
    // Pixel in bottom half (y=15, x=5) should remain black
    CHECK(pixels[15 * w + 5] == 0xFF000000);
}

TEST_CASE("FontEngine font loading and metrics") {
    std::string font_path = find_test_font();
    if (font_path.empty()) {
        WARN("No system font found for testing, skipping");
        return;
    }

    FontEngine engine(AntialiasMode::Grayscale);
    bool loaded = engine.load_font(font_path, 12.0, 96);
    CHECK(loaded);

    const auto& m = engine.metrics();
    CHECK(m.cell_width > 0);
    CHECK(m.cell_height > 0);
    CHECK(m.ascent > 0);
    CHECK(m.descent >= 0);

    // Rasterize 'A'
    const CachedGlyph* g1 = engine.get_glyph(U'A', false, false);
    REQUIRE(g1 != nullptr);
    CHECK(g1->mask.valid());
    CHECK(g1->width > 0);
    CHECK(g1->height > 0);

    // Second retrieval must hit cache and return identical pointer
    const CachedGlyph* g2 = engine.get_glyph(U'A', false, false);
    CHECK(g1 == g2);

    // Bold retrieval
    const CachedGlyph* g_bold = engine.get_glyph(U'A', true, false);
    REQUIRE(g_bold != nullptr);
    CHECK(g_bold != g1);
}

TEST_CASE("FontEngine A8 masks have aligned strides for every glyph width") {
    const auto path = find_test_font();
    if (path.empty()) {
        WARN("No system font found for testing, skipping");
        return;
    }
    for (auto mode : {AntialiasMode::Grayscale, AntialiasMode::None}) {
        FontEngine fonts(mode);
        REQUIRE(fonts.load_font(path, 13.0));
        for (char32_t cp = U'!'; cp <= U'~'; ++cp) {
            CAPTURE(cp);
            const auto* glyph = fonts.get_glyph(cp, false, false);
            REQUIRE(glyph != nullptr);
            if (glyph->width == 0 || glyph->height == 0) continue;
            REQUIRE(glyph->mask.valid());
            CHECK(glyph->mask.stride() >= glyph->width);
            CHECK(glyph->mask.stride() % 4 == 0);
        }
    }
}

TEST_CASE("CpuRenderer cursor stays opaque and incremental frames match full redraws") {
    FontEngine fonts;
    TerminalEngine engine(4, 2);
    CpuRenderer renderer(fonts);
    const int width = 4 * fonts.metrics().cell_width;
    const int height = 2 * fonts.metrics().cell_height;
    gritty::util::PixmanImage target(pixman_image_create_bits(
        PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    gritty::util::PixmanImage reference(pixman_image_create_bits(
        PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    gritty::util::PixmanRegion damage;

    auto check_frame = [&] {
        renderer.render(engine, target.get(), &damage);
        CpuRenderer fresh(fonts);
        fresh.render(engine, reference.get(), nullptr, true);
        const int count = height * target.stride() / 4;
        for (int i = 0; i < count; ++i) {
            REQUIRE(target.data()[i] == reference.data()[i]);
            REQUIRE((target.data()[i] >> 24) == 255);
        }
    };
    check_frame();
    check_frame(); // unchanged frame must not erase/toggle the cursor
    engine.set_cursor_pos(2, 1);
    check_frame(); // previous cursor must be restored, including its damage
    engine.set_cursor_visible(false);
    check_frame();
    engine.set_cursor_visible(true);
    engine.set_cursor_shape(CursorShape::Beam, false);
    check_frame();
    engine.set_cursor_shape(CursorShape::Underline, false);
    check_frame();
}

TEST_CASE("FontEngine LCD Subpixel antialiasing") {
    std::string font_path = find_test_font();
    if (font_path.empty()) {
        WARN("No system font found for testing, skipping");
        return;
    }

    FontEngine engine(AntialiasMode::SubpixelLcd);
    bool loaded = engine.load_font(font_path, 12.0, 96);
    CHECK(loaded);

    const CachedGlyph* g = engine.get_glyph(U'M', false, false);
    REQUIRE(g != nullptr);
    CHECK(g->is_subpixel);
    CHECK(g->mask.valid());
}

TEST_CASE("CpuRenderer offline frame rendering and PNG verification") {
    std::string font_path = find_test_font();
    if (font_path.empty()) {
        WARN("No system font found for testing, skipping");
        return;
    }

    FontEngine font_engine(AntialiasMode::Grayscale);
    bool loaded = font_engine.load_font(font_path, 13.0, 96);
    REQUIRE(loaded);

    int cols = 40;
    int rows = 12;
    TerminalEngine term_engine(cols, rows);
    VtParser parser(term_engine);

    // Feed a rich VT stream:
    // 1. Box drawing border
    parser.feed("\x1b[38;2;100;200;255m┌──────────────────────────────────────┐\r\n");
    parser.feed("│ \x1b[1;38;2;255;255;100mGritty Terminal Engine\x1b[0;38;2;100;200;255m               │\r\n");
    parser.feed("│ \x1b[3;38;2;255;100;200m100% CPU Rendered (pixman + fcft)\x1b[0;38;2;100;200;255m    │\r\n");
    parser.feed("│ \x1b[4:3;58;2;255;50;50mCurly Underline\x1b[0;38;2;100;200;255m   \x1b[9mStrikethrough\x1b[0;38;2;100;200;255m        │\r\n");
    parser.feed("│ \x1b[48;2;30;60;90;38;2;255;255;255m Colored Background \x1b[0;38;2;100;200;255m                 │\r\n");
    parser.feed("│ Blocks: \x1b[38;2;0;255;128m█ ▀ ▄ ▌ ▐ ░ ▒ ▓\x1b[0;38;2;100;200;255m              │\r\n");
    parser.feed("└──────────────────────────────────────┘\r\n");

    CpuRenderer renderer(font_engine);

    const auto& metrics = font_engine.metrics();
    int img_w = cols * metrics.cell_width;
    int img_h = rows * metrics.cell_height;

    gritty::util::PixmanImage target(pixman_image_create_bits(
        PIXMAN_a8r8g8b8, img_w, img_h, nullptr, img_w * 4));
    REQUIRE(target.valid());

    gritty::util::PixmanRegion damage;
    renderer.render(term_engine, target.get(), &damage, true);

    CHECK(!damage.empty());
    pixman_box32_t ext = damage.extents();
    CHECK(ext.x2 == img_w);
    CHECK(ext.y2 == img_h);

    // Save PNG image to test_render_output.png
    const char* out_png = "test_render_output.png";
    bool saved = save_pixman_to_png(out_png, target.get());
    CHECK(saved);
    CHECK(std::filesystem::exists(out_png));
    CHECK(std::filesystem::file_size(out_png) > 100);
}
