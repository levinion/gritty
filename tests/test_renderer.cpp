#include <doctest/doctest.h>
#include "font_fixture.hpp"
#include <png.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <filesystem>
#include "core/terminal_engine.hpp"
#include "core/kitty_placeholder.hpp"
#include "core/vt_parser.hpp"
#include "render/font_engine.hpp"
#include "render/box_drawing.hpp"
#include "render/cpu_renderer.hpp"

using namespace gritty::core;
using namespace gritty::render;

namespace {

bool grid_row_dirty(const gritty::core::TerminalEngine& engine, int y) {
    return engine.grid().row(y).dirty;
}

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

std::string find_test_font() { return test_font_path(); }
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

    // Thin line fast fills must stay pixel-identical to Pixman rectangles,
    // including non-white premultiplied colors.
    const int fw = 16, fh = 24;
    gritty::util::PixmanImage direct(pixman_image_create_bits(PIXMAN_a8r8g8b8, fw, fh, nullptr, fw * 4));
    gritty::util::PixmanImage reference(pixman_image_create_bits(PIXMAN_a8r8g8b8, fw, fh, nullptr, fw * 4));
    REQUIRE(direct.valid());
    REQUIRE(reference.valid());
    const pixman_rectangle16_t full = {0, 0, static_cast<uint16_t>(fw), static_cast<uint16_t>(fh)};
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, direct.get(), &black, 1, &full);
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, reference.get(), &black, 1, &full);
    const pixman_color_t tint = {0x8000, 0x4000, 0x00ff, 0xffff};
    const int thick = std::max(1, fh / 12);

    REQUIRE(BoxDrawingRenderer::render(direct.get(), 0x2500, 0, 0, fw, fh, tint));
    const pixman_rectangle16_t horizontal = {
        0, static_cast<int16_t>(fh / 2 - thick / 2),
        static_cast<uint16_t>(fw), static_cast<uint16_t>(thick)};
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, reference.get(), &tint, 1, &horizontal);
    CHECK(std::memcmp(direct.data(), reference.data(), static_cast<size_t>(fw * fh) * 4) == 0);

    pixman_image_fill_rectangles(PIXMAN_OP_SRC, direct.get(), &black, 1, &full);
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, reference.get(), &black, 1, &full);
    REQUIRE(BoxDrawingRenderer::render(direct.get(), 0x2502, 0, 0, fw, fh, tint));
    const pixman_rectangle16_t vertical = {
        static_cast<int16_t>(fw / 2 - thick / 2), 0,
        static_cast<uint16_t>(thick), static_cast<uint16_t>(fh)};
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, reference.get(), &tint, 1, &vertical);
    CHECK(std::memcmp(direct.data(), reference.data(), static_cast<size_t>(fw * fh) * 4) == 0);
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

TEST_CASE("Every box drawing, block and braille codepoint renders procedurally") {
    constexpr int w = 16;
    constexpr int h = 28;
    gritty::util::PixmanImage img(pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, nullptr, w * 4));
    REQUIRE(img.valid());
    const pixman_color_t black = {0, 0, 0, 0xffff};
    const pixman_color_t white = {0xffff, 0xffff, 0xffff, 0xffff};
    const pixman_rectangle16_t all = {0, 0, static_cast<uint16_t>(w), static_cast<uint16_t>(h)};

    for (char32_t cp = 0x2500; cp <= 0x28FF; ++cp) {
        if (cp > 0x259F && cp < 0x2800) continue; // Not a geometric block.
        CAPTURE(cp);
        pixman_image_fill_rectangles(PIXMAN_OP_SRC, img.get(), &black, 1, &all);
        REQUIRE(BoxDrawingRenderer::render(img.get(), cp, 0, 0, w, h, white));
        if (cp == 0x2800) continue; // Blank braille cell is intentionally empty.
        bool inked = false;
        for (int y = 0; y < h && !inked; ++y)
            for (int x = 0; x < w; ++x)
                if (img.data()[y * w + x] == 0xffffffffu) { inked = true; break; }
        CHECK(inked);
    }

    // Diagonals must reach both end corners; the rounded corner must reach
    // the right and bottom edges so it joins the neighbouring straight lines.
    const auto inked_at = [&](int x, int y) {
        return img.data()[y * w + x] == 0xffffffffu;
    };
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, img.get(), &black, 1, &all);
    REQUIRE(BoxDrawingRenderer::render(img.get(), 0x256D, 0, 0, w, h, white));
    CHECK(inked_at(w - 1, h - 1 - w / 2));
    CHECK(inked_at(w - 1 - w / 2, h - 1));
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, img.get(), &black, 1, &all);
    REQUIRE(BoxDrawingRenderer::render(img.get(), 0x2571, 0, 0, w, h, white));
    CHECK(inked_at(w - 1, 0));
    CHECK(inked_at(0, h - 1));
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, img.get(), &black, 1, &all);
    REQUIRE(BoxDrawingRenderer::render(img.get(), 0x2572, 0, 0, w, h, white));
    CHECK(inked_at(0, 0));
    CHECK(inked_at(w - 1, h - 1));
}

TEST_CASE("CpuRenderer draws procedural geometry into terminal cells") {
    const auto font_path = find_test_font();
    if (font_path.empty()) { WARN("No test font found"); return; }
    FontEngine fonts;
    REQUIRE(fonts.load_font(font_path, 13.0, 96));
    const auto& metrics = fonts.metrics();
    constexpr int cols = 4;
    constexpr int rows = 1;
    gritty::util::PixmanImage target(pixman_image_create_bits(
        PIXMAN_a8r8g8b8, cols * metrics.cell_width, rows * metrics.cell_height, nullptr,
        cols * metrics.cell_width * 4));
    REQUIRE(target.valid());
    const pixman_color_t black = {0, 0, 0, 0xffff};
    const pixman_rectangle16_t all = {
        0, 0, static_cast<uint16_t>(cols * metrics.cell_width),
        static_cast<uint16_t>(rows * metrics.cell_height)};

    for (const char32_t cp : {char32_t{0x2504}, char32_t{0x256D}, char32_t{0x2573},
                              char32_t{0x259A}, char32_t{0x28FF}}) {
        TerminalEngine engine(cols, rows);
        engine.set_cursor_visible(false);
        Cell cell;
        cell.codepoint = cp;
        engine.grid().set_cell(0, 0, cell);
        CpuRenderer renderer(fonts);
        pixman_image_fill_rectangles(PIXMAN_OP_SRC, target.get(), &black, 1, &all);
        renderer.render(engine, target.get(), nullptr, true);
        bool inked = false;
        for (int y = 0; y < metrics.cell_height && !inked; ++y)
            for (int x = 0; x < metrics.cell_width; ++x)
                if (target.data()[y * (target.stride() / 4) + x] == 0xffddddddu) {
                    inked = true;
                    break;
                }
        CAPTURE(cp);
        CHECK(inked);
    }
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

TEST_CASE("FontEngine opens fallback faces only on coverage misses and never retries bad files") {
    const auto path = find_test_font();
    REQUIRE_FALSE(path.empty());
    FontEngine fonts;
    CHECK_FALSE(fonts.add_fallback_font(path)); // A primary must establish size/DPI first.
    REQUIRE(fonts.load_font(path, 13.0));
    CHECK_FALSE(fonts.add_fallback_font(""));
    REQUIRE(fonts.add_fallback_font("/nonexistent/gritty-memory-test.ttf"));
    REQUIRE(fonts.add_fallback_font(path));
    REQUIRE(fonts.add_fallback_font(path)); // Repeated registration is deduplicated.
    CHECK(fonts.loaded_face_count() == 1);
    CHECK(fonts.face_load_attempt_count() == 1);
    for (char32_t cp = U'!'; cp <= U'~'; ++cp)
        REQUIRE(fonts.get_glyph(cp, false, false) != nullptr);
    CHECK(fonts.loaded_face_count() == 1);
    CHECK(fonts.face_load_attempt_count() == 1);
    REQUIRE(fonts.get_glyph(U'\U0010ffff', false, false) != nullptr);
    CHECK(fonts.loaded_face_count() == 2);
    CHECK(fonts.face_load_attempt_count() == 3);
    fonts.clear_cache();
    for (int i = 0; i < 8; ++i)
        REQUIRE(fonts.get_glyph(U'\U0010ffff', true, true) != nullptr);
    CHECK(fonts.face_load_attempt_count() == 3);
    REQUIRE(fonts.load_font(path, 14.0, 192));
    CHECK(fonts.loaded_face_count() == 1);
    REQUIRE(fonts.get_glyph(U'\U0010ffff', false, false) != nullptr);
    CHECK(fonts.face_load_attempt_count() == 1); // Reload also discards old fallback descriptors.
}

#if defined(PLATFORM_MACOS)
TEST_CASE("A deferred fallback renders the same glyph as opening that font directly") {
    const auto primary_path = find_test_font();
    const std::string fallback_path = "/System/Library/Fonts/Apple Symbols.ttf";
    REQUIRE_FALSE(primary_path.empty());
    gritty::util::FtLibrary library;
    FT_Face primary_raw = nullptr, fallback_raw = nullptr;
    REQUIRE(FT_New_Face(library.get(), primary_path.c_str(), 0, &primary_raw) == 0);
    gritty::util::FtFace primary(primary_raw);
    REQUIRE(FT_New_Face(library.get(), fallback_path.c_str(), 0, &fallback_raw) == 0);
    gritty::util::FtFace fallback(fallback_raw);
    FT_UInt index = 0;
    FT_ULong cp = FT_Get_First_Char(fallback_raw, &index);
    while (index != 0 && (cp < 33 || FT_Get_Char_Index(primary_raw, cp) != 0))
        cp = FT_Get_Next_Char(fallback_raw, cp, &index);
    REQUIRE(index != 0);

    FontEngine fonts, reference;
    REQUIRE(fonts.load_font(primary_path, 13.0, 192));
    REQUIRE(fonts.add_fallback_font(fallback_path));
    CHECK(fonts.loaded_face_count() == 1);
    REQUIRE(reference.load_font(fallback_path, 13.0, 192));
    const auto* actual = fonts.get_glyph(static_cast<char32_t>(cp), false, false);
    const auto* expected = reference.get_glyph(static_cast<char32_t>(cp), false, false);
    REQUIRE(actual != nullptr);
    REQUIRE(expected != nullptr);
    CHECK(fonts.loaded_face_count() == 2);
    REQUIRE(actual->mask.valid());
    REQUIRE(expected->mask.valid());
    CHECK(actual->bearing_x == expected->bearing_x);
    CHECK(actual->bearing_y == expected->bearing_y);
    REQUIRE(actual->width == expected->width);
    REQUIRE(actual->height == expected->height);
    for (int y = 0; y < actual->height; ++y) {
        auto* a = reinterpret_cast<const uint8_t*>(actual->mask.data()) + y * actual->mask.stride();
        auto* b = reinterpret_cast<const uint8_t*>(expected->mask.data()) + y * expected->mask.stride();
        for (int x = 0; x < actual->width; ++x) REQUIRE(a[x] == b[x]);
    }
}
#endif

TEST_CASE("FontEngine fallback face indices cannot overflow their cache keys") {
    const auto path = find_test_font();
    REQUIRE_FALSE(path.empty());
    FontEngine fonts;
    REQUIRE(fonts.load_font(path, 12.0));
    for (int i = 0; i < 255; ++i)
        REQUIRE(fonts.add_fallback_font("/nonexistent/gritty-fallback-" + std::to_string(i)));
    CHECK_FALSE(fonts.add_fallback_font("/nonexistent/gritty-overflow.ttf"));
    CHECK(fonts.loaded_face_count() == 1);
    CHECK(fonts.face_load_attempt_count() == 1);
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

TEST_CASE("Scrolling moves pixels in place and still matches a full redraw") {
    FontEngine fonts;
    TerminalEngine engine(8, 4);
    CpuRenderer renderer(fonts);
    const int width = 8 * fonts.metrics().cell_width;
    const int height = 4 * fonts.metrics().cell_height;
    gritty::util::PixmanImage target(pixman_image_create_bits(
        PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    gritty::util::PixmanImage reference(pixman_image_create_bits(
        PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    gritty::util::PixmanRegion damage;

    auto check_frame = [&] {
        renderer.render(engine, target.get(), &damage);
        CpuRenderer fresh(fonts);
        fresh.render(engine, reference.get(), nullptr, true);
        for (int i = 0; i < height * target.stride() / 4; ++i) REQUIRE(target.data()[i] == reference.data()[i]);
    };

    gritty::core::VtParser parser(engine);
    parser.feed("AAAA\r\nBBBB\r\nCCCC\r\nDDDD\r\n");
    check_frame();

    // Cursor is on the last row; scrolling must move its pixels too.
    auto before = renderer.stats().scroll_moves;
    parser.feed("E\r\n"); // Cursor at the bottom: this scrolls the screen up by one row.
    check_frame();
    CHECK(renderer.stats().scroll_moves == before + 1);
    CHECK(renderer.stats().scrolled_rows >= 1);

    parser.feed("F\r\n");
    check_frame();
    CHECK(renderer.stats().scroll_moves == before + 2);

    // Same pixels through the explicit fallback path.
    renderer.set_scroll_optimization(false);
    parser.feed("G\r\n");
    check_frame();
    CHECK(renderer.stats().scroll_moves == before + 2);
    for (int y = 0; y < 4; ++y) CHECK(grid_row_dirty(engine, y) == false);

    // Sparse rows must take the same path and stay pixel-exact.
    FontEngine sparse_fonts;
    TerminalEngine sparse(8, 4);
    CpuRenderer sparse_renderer(sparse_fonts);
    gritty::core::VtParser sparse_parser(sparse);
    gritty::util::PixmanImage sparse_target(pixman_image_create_bits(
        PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    gritty::util::PixmanImage sparse_reference(pixman_image_create_bits(
        PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    gritty::util::PixmanRegion sparse_damage;
    auto check_sparse = [&] {
        sparse_renderer.render(sparse, sparse_target.get(), &sparse_damage);
        CpuRenderer fresh(sparse_fonts);
        fresh.render(sparse, sparse_reference.get(), nullptr, true);
        for (int i = 0; i < height * sparse_target.stride() / 4; ++i)
            REQUIRE(sparse_target.data()[i] == sparse_reference.data()[i]);
    };
    sparse_parser.feed("1\r\n2\r\n3\r\n4\r\n");
    check_sparse();
    // Sparse rows are cheaper to repaint now that background runs collapse;
    // the adaptive policy must not move their pixels.
    const auto moves = sparse_renderer.stats().scroll_moves;
    const auto repaints = sparse_renderer.stats().scroll_repaints;
    sparse_parser.feed("5\r\n");
    check_sparse();
    CHECK(sparse_renderer.stats().scroll_moves == moves);
    CHECK(sparse_renderer.stats().scroll_repaints == repaints + 1);
    // A move must still be pixel-exact when forced on the same content.
    auto* sparse_renderer_ptr = &sparse_renderer;
    sparse_renderer_ptr->set_scroll_mode(CpuRenderer::ScrollMode::AlwaysMove);
    sparse_parser.feed("6\r\n");
    check_sparse();
    CHECK(sparse_renderer.stats().scroll_moves == moves + 1);
}

TEST_CASE("Batched background runs and inverse video keep exact colors") {
    FontEngine fonts;
    TerminalEngine engine(8, 1);
    CpuRenderer renderer(fonts);
    engine.set_cursor_visible(false);
    auto& grid = engine.grid();
    const auto cell_w = fonts.metrics().cell_width;
    const auto cell_h = fonts.metrics().cell_height;

    Cell normal;
    normal.codepoint = U' ';
    Cell red;
    red.codepoint = U' ';
    red.bg = Color::make_rgb(0xff, 0x00, 0x00);
    Cell blue;
    blue.codepoint = U' ';
    blue.bg = Color::make_rgb(0x00, 0x00, 0xff);
    Cell inverse;
    inverse.codepoint = U' ';
    inverse.attrs.inverse = true;
    Cell inverse_block;
    inverse_block.codepoint = 0x2588; // Full block: the whole cell is foreground.
    inverse_block.attrs.inverse = true;

    grid.set_cell(0, 0, normal);
    grid.set_cell(1, 0, red);
    grid.set_cell(2, 0, red);
    grid.set_cell(3, 0, blue);
    grid.set_cell(4, 0, normal);
    grid.set_cell(5, 0, inverse);
    grid.set_cell(6, 0, inverse_block);
    grid.set_cell(7, 0, normal);

    gritty::util::PixmanImage target(pixman_image_create_bits(
        PIXMAN_a8r8g8b8, 8 * cell_w, cell_h, nullptr, 0));
    gritty::util::PixmanRegion damage;
    renderer.render(engine, target.get(), &damage, true);

    const auto* pixels = target.data();
    const int stride = target.stride() / 4;
    auto center = [&](int col) { return pixels[(cell_h / 2) * stride + col * cell_w + cell_w / 2]; };
    const uint32_t default_bg = 0xff111111;
    const uint32_t default_fg = 0xffdddddd;
    CHECK(center(0) == default_bg);
    CHECK(center(1) == 0xffff0000);
    CHECK(center(2) == 0xffff0000); // Two equal siblings collapse into one run.
    CHECK(center(3) == 0xff0000ff);
    CHECK(center(4) == default_bg);
    CHECK(center(5) == default_fg);        // Inverse background is the default foreground.
    CHECK(center(6) == default_bg);        // Inverse full block foreground is the default background.
    CHECK(center(7) == default_bg);
}

TEST_CASE("Two scroll regions in one frame apply both shifts and match a redraw") {
    FontEngine fonts;
    TerminalEngine engine(8, 6);
    CpuRenderer renderer(fonts);
    engine.set_cursor_visible(false);
    const int width = 8 * fonts.metrics().cell_width;
    const int height = 6 * fonts.metrics().cell_height;
    gritty::util::PixmanImage target(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    gritty::util::PixmanImage reference(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    gritty::util::PixmanRegion damage;

    gritty::core::VtParser parser(engine);
    parser.feed("AAAAAAAA\r\nBBBBBBBB\r\nCCCCCCCC\r\nDDDDDDDD\r\nEEEEEEEE\r\nFFFFFFFF\r\n");
    auto check_frame = [&] {
        renderer.render(engine, target.get(), &damage);
        CpuRenderer fresh(fonts);
        fresh.render(engine, reference.get(), nullptr, true);
        for (int i = 0; i < height * target.stride() / 4; ++i) REQUIRE(target.data()[i] == reference.data()[i]);
    };
    check_frame();

    // Two independent regions (split panes) shift in the same frame.
    Cell blank;
    engine.grid().scroll_up(0, 2, 1, blank);
    engine.grid().scroll_up(3, 5, 1, blank);
    // And one pane repaints part of its content in the same frame.
    Cell fresh_cell;
    fresh_cell.codepoint = U'Z';
    engine.grid().set_cell(0, 2, fresh_cell);
    engine.grid().set_cell(0, 5, fresh_cell);
    const auto moves_before = renderer.stats().scroll_moves;
    check_frame();
    CHECK(renderer.stats().scroll_moves == moves_before + 2);

    // A conflicting shift in the queue (same region, opposite direction) falls
    // back to repainting every queued region and is still pixel-exact.
    check_frame();
    engine.grid().scroll_up(0, 2, 1, blank);
    engine.grid().scroll_down(0, 2, 1, blank);
    check_frame();
}

TEST_CASE("Batched glyph compositing is pixel-identical to per-glyph drawing") {
    FontEngine fonts;
    TerminalEngine engine(40, 6);
    engine.set_cursor_visible(false);
    auto& grid = engine.grid();

    const char32_t sample[] = U"Hello, world! 0123456789 ====----####";
    for (int y = 0; y < 6; ++y) {
        for (int x = 0; x < 40; ++x) {
            Cell cell;
            cell.codepoint = sample[(y * 40 + x) % (sizeof(sample) / sizeof(sample[0]) - 1)];
            if (y == 1) cell.fg = Color::make_rgb(0xff, 0x40, 0x40);          // Different colour run.
            if (y == 2) cell.fg = Color::make_rgb(0x40, 0xff, 0x40);
            if (y == 3) cell.attrs.bold = true;                                // Style changes.
            if (y == 4) cell.attrs.underline_style = 1;                        // Decorations flush batches.
            if (y == 5 && x == 0) cell.attrs.strikethrough = true;
            if (y == 5 && x == 1) cell.attrs.inverse = true;
            if (y == 5 && x == 2) cell.attrs.dim = true;
            grid.set_cell(x, y, cell);
        }
    }

    const int width = 40 * fonts.metrics().cell_width;
    const int height = 6 * fonts.metrics().cell_height;
    gritty::util::PixmanImage batched(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    gritty::util::PixmanImage single(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    gritty::util::PixmanRegion damage;
    CpuRenderer with_batching(fonts);
    CpuRenderer without_batching(fonts);
    without_batching.set_glyph_batching(false);
    with_batching.render(engine, batched.get(), &damage, true);
    without_batching.render(engine, single.get(), &damage, true);
    for (int i = 0; i < height * batched.stride() / 4; ++i)
        REQUIRE(batched.data()[i] == single.data()[i]);
}

TEST_CASE("Combining marks are shaped and drawn as a cluster") {
    FontEngine fonts;
    REQUIRE(fonts.load_font(find_test_font(), 12.0, 96));
    const char32_t mark = U'\u0301';
    if (fonts.get_glyph(mark, false, false) == nullptr) {
        WARN("test font has no combining acute, skipping");
        return;
    }
    // 'x' + combining acute has no precomposed form, so HarfBuzz must return
    // the base plus the mark (fonts may legitimately compose 'e' + acute).
    const std::array<char32_t, 2> cluster{U'x', mark};
    const auto* shaped = fonts.shape_cluster(cluster, false, false);
    REQUIRE(shaped != nullptr);
    REQUIRE(shaped->glyphs.size() == 2);
    CHECK(fonts.shape_cluster(cluster, false, false) == shaped); // Cached.
    const auto* base = fonts.glyph_for_index(shaped->face_idx, shaped->glyphs[0].glyph_index, false, false);
    REQUIRE(base != nullptr);
    CHECK(base->mask.valid());

    // Rendering the cluster must put more ink on the cell than the base alone.
    TerminalEngine engine(4, 3);
    engine.set_cursor_visible(false);
    Cell plain;
    plain.codepoint = U'x';
    Cell combined = plain;
    const std::array<char32_t, 1> marks{mark};
    const auto combined_idx = engine.grid().store_combining_codepoints(marks);
    auto& grid = engine.grid();
    grid.set_cell(0, 1, plain);
    grid.set_cell_with_extra(1, 1, combined, CellExtra{.combining_idx = combined_idx});

    const int width = 4 * fonts.metrics().cell_width;
    const int height = 3 * fonts.metrics().cell_height;
    gritty::util::PixmanImage target(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    gritty::util::PixmanRegion damage;
    CpuRenderer renderer(fonts);
    renderer.render(engine, target.get(), &damage, true);

    // Count the whole column: accents may legitimately extend above the row.
    auto ink = [&](int col) {
        int count = 0;
        const int stride = target.stride() / 4;
        for (int y = 0; y < height; ++y)
            for (int x = col * fonts.metrics().cell_width; x < (col + 1) * fonts.metrics().cell_width; ++x)
                if (target.data()[y * stride + x] != 0xff111111u) ++count; // Not the default background.
        return count;
    };
    const int plain_ink = ink(0);
    const int combined_ink = ink(1);
    CHECK(plain_ink > 0);
    CHECK(combined_ink > plain_ink);
}

TEST_CASE("OSC 8 hyperlink cells draw an underline") {
    FontEngine fonts;
    TerminalEngine engine(4, 1);
    engine.set_cursor_visible(false);
    auto& grid = engine.grid();
    const auto link = engine.get_or_create_hyperlink_id("https://example.com");
    Cell plain;
    plain.codepoint = U'x';
    Cell linked = plain;
    grid.set_cell(0, 0, plain);
    grid.set_cell_with_extra(1, 0, linked, gritty::core::CellExtra{.hyperlink_id = link});

    const int cell_w = fonts.metrics().cell_width;
    const int cell_h = fonts.metrics().cell_height;
    gritty::util::PixmanImage target(pixman_image_create_bits(
        PIXMAN_a8r8g8b8, 4 * cell_w, cell_h, nullptr, 0));
    gritty::util::PixmanRegion damage;
    CpuRenderer renderer(fonts);
    renderer.render(engine, target.get(), &damage, true);

    const int underline_y = fonts.metrics().ascent + fonts.metrics().underline_pos;
    const int stride = target.stride() / 4;
    const auto at = [&](int col) { return target.data()[underline_y * stride + col * cell_w + cell_w / 2]; };
    CHECK(at(0) == 0xff111111u); // Plain cell: default background.
    CHECK(at(1) == 0xffddddddu); // Linked cell: foreground underline.
}

TEST_CASE("Placed images are scaled into their cell box and follow z-order") {
    auto make_image = [](int w, int h, uint32_t color) {
        gritty::util::PixmanImage image(pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, nullptr, 0));
        std::fill(image.data(), image.data() + static_cast<size_t>(image.stride() / 4) * h, color);
        return image;
    };
    FontEngine fonts;
    const int cell_w = fonts.metrics().cell_width;
    const int cell_h = fonts.metrics().cell_height;

    auto check = [&](int z, bool block_glyph, uint32_t expected) {
        TerminalEngine engine(4, 2);
        engine.set_cursor_visible(false);
        if (block_glyph) {
            Cell cell;
            cell.codepoint = 0x2588; // Full block: solid foreground over the whole cell.
            cell.fg = Color::make_rgb(0x00, 0xff, 0x00);
            cell.bg = Color::make_rgb(0x00, 0x00, 0x00);
            engine.grid().set_cell(0, 0, cell);
        }
        PlacedImage placed;
        placed.image = make_image(2, 2, 0xffff0000u);
        placed.col = 0; placed.row = 0; placed.cols = 1; placed.rows = 1; placed.z_index = z;
        engine.place_image(std::move(placed));
        gritty::util::PixmanImage target(pixman_image_create_bits(
            PIXMAN_a8r8g8b8, 4 * cell_w, 2 * cell_h, nullptr, 0));
        gritty::util::PixmanRegion damage;
        CpuRenderer renderer(fonts);
        renderer.render(engine, target.get(), &damage, true);
        const int stride = target.stride() / 4;
        CHECK(target.data()[(cell_h / 2) * stride + cell_w / 2] == expected);
        CHECK(target.data()[(cell_h - 1) * stride + cell_w - 1] == expected);
    };

    check(0, false, 0xffff0000u);  // Above text, no glyph: image wins over the background.
    check(-1, false, 0xffff0000u); // Below text but no glyph: still visible.
    check(0, true, 0xffff0000u);   // Above text: the image covers the block glyph.
    check(-1, true, 0xff00ff00u);  // Below text: the block glyph wins.
}

TEST_CASE("Kitty Unicode placeholders draw image tiles without glyphs") {
    FontEngine fonts;
    TerminalEngine engine(6, 4);
    engine.set_cursor_visible(false);
    auto& grid = engine.grid();

    // 2x2 RGBA image: red, green / blue, white. Virtual placement covers 2x2 cells.
    const std::string reply = engine.kitty_graphics().process_apc_sequence(
        "Ga=T,U=1,i=5,c=2,r=2,f=32,s=2,v=4;/wAA/wD/AP//AAD/AP8A/wAA////////AAD///////8=", 0, 0);
    CHECK(reply.find("OK") != std::string::npos);
    REQUIRE(engine.kitty_graphics().virtual_placements().size() == 1);

    auto placeholder = [&](int x, int y, std::initializer_list<char32_t> marks, uint32_t fg_index = 5) {
        Cell cell;
        cell.codepoint = 0x10EEEE;
        cell.fg = Color::make_indexed(static_cast<uint8_t>(fg_index));
        const std::vector<char32_t> seq(marks);
        const auto extra_idx = grid.store_combining_codepoints(seq);
        grid.set_cell_with_extra(x, y, cell, gritty::core::CellExtra{.combining_idx = extra_idx});
    };
    // Row 0: explicit row/column diacritics (0x305 = 0, 0x30D = 1).
    placeholder(1, 1, {0x305, 0x305});
    placeholder(2, 1, {0x305, 0x30D});
    // Row 1: the first cell carries only the row diacritic; the second cell
    // inherits row/column from the left placeholder.
    placeholder(1, 2, {0x30D});
    placeholder(2, 2, {});
    // No virtual placement for image 6: the placeholder must stay background.
    placeholder(4, 3, {0x305, 0x305}, 6);

    const int cw = fonts.metrics().cell_width;
    const int ch = fonts.metrics().cell_height;
    gritty::util::PixmanImage target(pixman_image_create_bits(PIXMAN_a8r8g8b8, 6 * cw, 4 * ch, nullptr, 0));
    gritty::util::PixmanRegion damage;
    CpuRenderer renderer(fonts);
    renderer.render(engine, target.get(), &damage, true);

    const int stride = target.stride() / 4;
    auto center = [&](int x, int y) { return target.data()[(y * ch + ch / 2) * stride + x * cw + cw / 2]; };
    // Every placeholder cell of the virtual 2x2 placement has image ink (the
    // exact quadrant depends on the fit, so assert "not the cell background").
    CHECK(center(1, 1) != 0xFF111111u);
    CHECK(center(2, 1) != 0xFF111111u);
    CHECK(center(1, 2) != 0xFF111111u);
    CHECK(center(2, 2) != 0xFF111111u);
    CHECK(center(1, 1) != center(4, 3)); // Placeholder without a virtual placement has no image.
    CHECK(center(4, 3) == 0xFF111111u);
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

TEST_CASE("Unicode placeholder local clipping keeps a scrolled top row visible") {
    FontEngine fonts;
    TerminalEngine engine(3, 3);
    engine.set_cursor_visible(false);
    auto& grid = engine.grid();
    // 1x6 image fills a 1x3 cell rectangle. Image rows 0 and 1 have scrolled
    // away; only rows 2 and 3 remain as placeholders at grid rows 0 and 1.
    const std::string seq =
        "Ga=T,U=1,i=9,c=1,r=3,f=32,s=1,v=6;/wAA//8AAP8A/wD/AP8A/wAA//8AAP//";
    REQUIRE(engine.kitty_graphics().process_apc_sequence(seq, 0, 0).find("OK") != std::string::npos);

    auto placeholder = [&](int x, int y, char32_t row_mark) {
        Cell cell;
        cell.codepoint = 0x10EEEE;
        cell.fg = Color::make_indexed(9);
        const std::vector<char32_t> marks{row_mark, 0x305};
        const auto extra_idx = grid.store_combining_codepoints(marks);
        grid.set_cell_with_extra(x, y, cell, gritty::core::CellExtra{.combining_idx = extra_idx});
    };
    placeholder(1, 0, 0x30D); // image row 1 (top row scrolled away)
    placeholder(1, 1, 0x30E); // image row 2

    const int cw = fonts.metrics().cell_width, ch = fonts.metrics().cell_height;
    gritty::util::PixmanImage target(pixman_image_create_bits(PIXMAN_a8r8g8b8, 3 * cw, 3 * ch, nullptr, 0));
    gritty::util::PixmanRegion damage;
    CpuRenderer renderer(fonts);
    renderer.render(engine, target.get(), &damage, true);
    const int stride = target.stride() / 4;
    auto center = [&](int x, int y) { return target.data()[(y * ch + ch / 2) * stride + x * cw + cw / 2]; };
    CHECK(center(1, 0) == 0xFF00FF00u); // green: virtual image row 1, center pixel row 2
    CHECK(center(1, 1) == 0xFF0000FFu); // blue: virtual image row 2, center pixel row 4
    CHECK(center(1, 2) == 0xFF111111u); // no placeholder below
    const auto bounds = damage.extents();
    CHECK(bounds.x1 <= 1 * cw);
    CHECK(bounds.y1 == 0);
    CHECK(bounds.x2 >= 2 * cw);
    CHECK(bounds.y2 >= 2 * ch);
}

TEST_CASE("Kitty source crop and image layers disable pixel memmove scrolling") {
    FontEngine fonts;
    TerminalEngine engine(4, 3);
    auto raw = pixman_image_create_bits(PIXMAN_a8r8g8b8, 2, 2, nullptr, 0);
    REQUIRE(raw != nullptr);
    auto* data = pixman_image_get_data(raw);
    data[0] = 0xFFFF0000; data[1] = 0xFF00FF00;
    data[2] = 0xFF0000FF; data[3] = 0xFFFFFFFF;
    gritty::util::PixmanImage source(raw);
    PlacedImage placed;
    placed.image.reset(pixman_image_ref(source.get()));
    placed.col = 1; placed.row = 1; placed.cols = 1; placed.rows = 1;
    placed.src_x = 1; placed.src_y = 0; placed.src_w = 1; placed.src_h = 1;
    engine.place_image(std::move(placed));

    CpuRenderer renderer(fonts);
    renderer.set_scroll_mode(CpuRenderer::ScrollMode::AlwaysMove);
    const int cw = fonts.metrics().cell_width, ch = fonts.metrics().cell_height;
    gritty::util::PixmanImage target(pixman_image_create_bits(PIXMAN_a8r8g8b8, 4 * cw, 3 * ch, nullptr, 0));
    renderer.render(engine, target.get(), nullptr, true);
    CHECK(target.data()[(ch + ch / 2) * target.stride() / 4 + cw + cw / 2] == 0xFF00FF00u);

    VtParser parser(engine);
    parser.feed("line1\r\nline2\r\n");
    renderer.render(engine, target.get(), nullptr, false);
    CHECK(renderer.stats().scroll_moves == 0);
    CHECK(renderer.stats().scroll_repaints > 0);
}

TEST_CASE("Disconnected placeholder blocks with one placement are drawn separately") {
    FontEngine fonts;
    TerminalEngine engine(7, 3);
    engine.set_cursor_visible(false);
    auto& grid = engine.grid();
    REQUIRE(engine.kitty_graphics().process_apc_sequence(
        "Ga=T,U=1,i=11,c=1,r=1,f=32,s=1,v=1;/wAA/w==", 0, 0).find("OK") != std::string::npos);
    auto placeholder = [&](int x, int y) {
        Cell cell;
        cell.codepoint = 0x10EEEE;
        cell.fg = Color::make_indexed(11);
        const std::vector<char32_t> marks{0x305, 0x305};
        const auto extra_idx = grid.store_combining_codepoints(marks);
        grid.set_cell_with_extra(x, y, cell, gritty::core::CellExtra{.combining_idx = extra_idx});
    };
    placeholder(1, 1);
    placeholder(5, 1); // Same image/placement, disconnected by three cells.

    const int cw = fonts.metrics().cell_width, ch = fonts.metrics().cell_height;
    gritty::util::PixmanImage target(pixman_image_create_bits(PIXMAN_a8r8g8b8, 7 * cw, 3 * ch, nullptr, 0));
    CpuRenderer renderer(fonts);
    renderer.render(engine, target.get(), nullptr, true);
    const int stride = target.stride() / 4;
    auto center = [&](int x, int y) { return target.data()[(y * ch + ch / 4) * stride + x * cw + cw / 2]; };
    CHECK(center(1, 1) == 0xFFFF0000u);
    CHECK(center(5, 1) == 0xFFFF0000u);
    CHECK(center(3, 1) == 0xFF111111u); // The gap must not be covered by one stretched image.
}

TEST_CASE("Pixel gutters outside the cell grid are repainted on incremental frames") {
    FontEngine fonts;
    TerminalEngine engine(4, 2);
    engine.set_cursor_visible(false);
    engine.grid().reset_damage();

    const int cw = fonts.metrics().cell_width, ch = fonts.metrics().cell_height;
    const int width = 4 * cw + 7, height = 2 * ch + 5; // Non-cell-sized window.
    gritty::util::PixmanImage target(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    REQUIRE(target.valid());

    // Simulate stale pixels in a reused display buffer, with no dirty grid rows.
    const pixman_color_t stale = {0xffff, 0, 0xffff, 0xffff};
    const pixman_rectangle16_t all{0, 0,
        static_cast<uint16_t>(width), static_cast<uint16_t>(height)};
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, target.get(), &stale, 1, &all);

    CpuRenderer renderer(fonts);
    gritty::util::PixmanRegion damage;
    renderer.render(engine, target.get(), &damage, false);

    const int stride = target.stride() / 4;
    CHECK(target.data()[(height / 2) * stride + 4 * cw + 3] == 0xFF111111u);
    CHECK(target.data()[(2 * ch + 2) * stride + width / 2] == 0xFF111111u);
    CHECK(damage.extents().x2 == width);
    CHECK(damage.extents().y2 == height);
}

TEST_CASE("Transparent placeholders are not recomposited on clean rows") {
    FontEngine fonts;
    TerminalEngine engine(4, 3);
    engine.set_cursor_visible(false);
    auto& grid = engine.grid();
    REQUIRE(engine.kitty_graphics().process_apc_sequence(
        "Ga=T,U=1,i=12,c=1,r=1,f=32,s=1,v=1;/wAAgA==", 0, 0).find("OK") != std::string::npos);
    Cell cell;
    cell.codepoint = 0x10EEEE;
    cell.fg = Color::make_indexed(12);
    const std::vector<char32_t> marks{0x305, 0x305};
    const auto extra_idx = grid.store_combining_codepoints(marks);
    grid.set_cell_with_extra(2, 1, cell, gritty::core::CellExtra{.combining_idx = extra_idx});

    const int cw = fonts.metrics().cell_width, ch = fonts.metrics().cell_height;
    gritty::util::PixmanImage target(pixman_image_create_bits(PIXMAN_a8r8g8b8, 4 * cw, 3 * ch, nullptr, 0));
    CpuRenderer renderer(fonts);
    renderer.render(engine, target.get(), nullptr, true);
    const int index = (ch + ch / 4) * target.stride() / 4 + 2 * cw + cw / 2;
    const uint32_t before = target.data()[index];
    grid.row(0).mark_dirty(0); // Cursor-blink-like dirty work away from the image.
    renderer.render(engine, target.get(), nullptr, false);
    CHECK(target.data()[index] == before); // No second OVER, no darker alpha.
}

TEST_CASE("Short plain-glyph runs are pixel-identical with lookahead removed") {
    FontEngine fonts;
    REQUIRE(fonts.load_font(find_test_font(), 12.0, 96));
    TerminalEngine engine(6, 2);
    engine.set_cursor_visible(false);
    auto& grid = engine.grid();
    const char32_t sample[] = U"abcXY";
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 6; ++x) {
            Cell cell;
            cell.codepoint = sample[y * 6 + x];
            cell.fg = Color::make_rgb(0x20 + x * 8, 0x80, 0xc0 - y * 16);
            grid.set_cell(x, y, cell);
        }

    const int width = 6 * fonts.metrics().cell_width;
    const int height = 2 * fonts.metrics().cell_height;
    gritty::util::PixmanImage batched(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    gritty::util::PixmanImage single(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    CpuRenderer with_batching(fonts);
    CpuRenderer without_batching(fonts);
    without_batching.set_glyph_batching(false);
    with_batching.render(engine, batched.get(), nullptr, true);
    without_batching.render(engine, single.get(), nullptr, true);
    for (size_t i = 0; i < static_cast<size_t>(height) * batched.stride() / 4; ++i)
        REQUIRE(batched.data()[i] == single.data()[i]);
}

TEST_CASE("Dynamic solid source preserves consecutive TrueColor glyph pixels") {
    FontEngine fonts;
    REQUIRE(fonts.load_font(find_test_font(), 13.0, 96));
    const CachedGlyph* probe = fonts.get_glyph(U'M', false, false);
    REQUIRE(probe != nullptr);
    REQUIRE(probe->mask.valid());
    REQUIRE(pixman_image_get_format(probe->mask.get()) == PIXMAN_a8);

    int mask_x = -1, mask_y = -1, best_alpha = -1;
    const int mask_stride = probe->mask.stride();
    const auto* mask_bits = reinterpret_cast<const uint8_t*>(probe->mask.data());
    for (int y = 0; y < probe->height; ++y)
        for (int x = 0; x < probe->width; ++x)
            if (mask_bits[y * mask_stride + x] > best_alpha) {
                best_alpha = mask_bits[y * mask_stride + x];
                mask_x = x; mask_y = y;
            }
    REQUIRE(best_alpha > 0);
    const int glyph_x = probe->bearing_x + mask_x;
    const int glyph_y = fonts.metrics().ascent - probe->bearing_y + mask_y;

    constexpr int cols = 12;
    TerminalEngine engine(cols, 1);
    engine.set_cursor_visible(false);
    auto& grid = engine.grid();
    const uint32_t colors[cols] = {
        0xff000000, 0xffffffff, 0xffff0000, 0xff00ff00, 0xff0000ff,
        0xff123456, 0xff654321, 0xffabcdef, 0xff102030, 0xfffedcba,
        0xff00ff7f, 0xff7f00ff,
    };
    for (int x = 0; x < cols; ++x) {
        Cell cell;
        cell.codepoint = U'M';
        cell.fg = Color::make_rgb((colors[x] >> 16) & 255,
            (colors[x] >> 8) & 255, colors[x] & 255);
        grid.set_cell(x, 0, cell);
    }

    const int width = cols * fonts.metrics().cell_width;
    const int height = fonts.metrics().cell_height;
    gritty::util::PixmanImage batched(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    gritty::util::PixmanImage single(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    REQUIRE(batched.valid());
    REQUIRE(single.valid());

    CpuRenderer with_batching(fonts);
    CpuRenderer without_batching(fonts);
    without_batching.set_glyph_batching(false);
    with_batching.render(engine, batched.get(), nullptr, true);
    without_batching.render(engine, single.get(), nullptr, true);

    const int stride = batched.stride() / 4;
    for (int x = 0; x < cols; ++x) {
        const int px = x * fonts.metrics().cell_width + glyph_x;
        const uint32_t expected = colors[x]; // Fully opaque foreground at max coverage.
        REQUIRE(px >= 0);
        REQUIRE(px < width);
        REQUIRE(glyph_y >= 0);
        REQUIRE(glyph_y < height);
        CHECK(batched.data()[glyph_y * stride + px] == expected);
        CHECK(single.data()[glyph_y * stride + px] == expected);
    }
    for (size_t i = 0; i < static_cast<size_t>(height) * stride; ++i)
        REQUIRE(batched.data()[i] == single.data()[i]);
}

TEST_CASE("Dynamic solid source keeps premultiplied alpha exact") {
    FontEngine fonts;
    REQUIRE(fonts.load_font(find_test_font(), 13.0, 96));
    const CachedGlyph* probe = fonts.get_glyph(U'M', false, false);
    REQUIRE(probe != nullptr);
    REQUIRE(probe->mask.valid());
    int mask_x = -1, mask_y = -1, best_alpha = -1;
    const int mask_stride = probe->mask.stride();
    const auto* mask_bits = reinterpret_cast<const uint8_t*>(probe->mask.data());
    for (int y = 0; y < probe->height; ++y)
        for (int x = 0; x < probe->width; ++x)
            if (mask_bits[y * mask_stride + x] > best_alpha) {
                best_alpha = mask_bits[y * mask_stride + x];
                mask_x = x; mask_y = y;
            }
    REQUIRE(best_alpha == 255);

    RenderConfig cfg;
    // 1/255 alpha exposes the difference between truncating Pixman's 16-bit
    // premultiplication and the project's 8-bit (c*a+127)/255 rounding.
    cfg.default_fg = 0x01808080;
    cfg.default_bg = 0xff000000;
    cfg.render_cursor = false;
    TerminalEngine engine(1, 1);
    engine.grid().row(0).cells[0].codepoint = U'M';

    const int width = fonts.metrics().cell_width;
    const int height = fonts.metrics().cell_height;
    gritty::util::PixmanImage target(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    REQUIRE(target.valid());
    CpuRenderer renderer(fonts, cfg);
    renderer.render(engine, target.get(), nullptr, true);

    const int x = probe->bearing_x + mask_x;
    const int y = fonts.metrics().ascent - probe->bearing_y + mask_y;
    REQUIRE(x >= 0); REQUIRE(x < width);
    REQUIRE(y >= 0); REQUIRE(y < height);
    // Opaque target: alpha is 255 after OVER, RGB stays premultiplied (1,1,1).
    CHECK(target.data()[y * target.stride() / 4 + x] == 0xff010101u);
}

TEST_CASE("OSC 8 hyperlink underlines spaces as well as non-space cells") {
    FontEngine fonts;
    REQUIRE(fonts.load_font(find_test_font(), 13.0, 96));
    TerminalEngine engine(4, 1);
    engine.set_cursor_visible(false);
    const uint16_t id = engine.get_or_create_hyperlink_id("https://example.com");
    Cell space;
    space.codepoint = U' ';
    engine.grid().set_cell_with_extra(1, 0, space, CellExtra{.hyperlink_id = id});

    const int cw = fonts.metrics().cell_width;
    const int ch = fonts.metrics().cell_height;
    gritty::util::PixmanImage target(pixman_image_create_bits(
        PIXMAN_a8r8g8b8, 4 * cw, ch, nullptr, 0));
    RenderConfig cfg;
    cfg.default_fg = 0xff00ff00;
    cfg.default_bg = 0xff000000;
    cfg.render_cursor = false;
    CpuRenderer renderer(fonts, cfg);
    renderer.render(engine, target.get(), nullptr, true);

    const int ul_y = fonts.metrics().ascent + fonts.metrics().underline_pos;
    REQUIRE(ul_y >= 0);
    REQUIRE(ul_y < ch);
    CHECK(target.data()[ul_y * (target.stride() / 4) + cw + cw / 2] == 0xff00ff00u);
    CHECK(target.data()[ul_y * (target.stride() / 4) + cw / 2] == 0xff000000u);
}

TEST_CASE("Overwriting or erasing half of a wide character clears its paired cell") {
    TerminalEngine engine(6, 2);
    engine.set_cursor_pos(0, 0);
    engine.print_char(U'中');
    REQUIRE(engine.grid().cell(0, 0).attrs.width == 2);
    REQUIRE(engine.grid().cell(1, 0).attrs.width == 0);

    // Overwrite continuation half with ASCII.
    engine.set_cursor_pos(1, 0);
    engine.print_char(U'a');
    CHECK(engine.grid().cell(0, 0).codepoint == U' ');
    CHECK(engine.grid().cell(0, 0).attrs.width == 1);
    CHECK(engine.grid().cell(1, 0).codepoint == U'a');

    // Overwrite leader half with ASCII.
    engine.set_cursor_pos(2, 0);
    engine.print_char(U'文');
    engine.set_cursor_pos(2, 0);
    engine.print_ascii_run("b");
    CHECK(engine.grid().cell(2, 0).codepoint == U'b');
    CHECK(engine.grid().cell(3, 0).codepoint == U' ');
    CHECK(engine.grid().cell(3, 0).attrs.width == 1);

    // Erase-in-line splitting a wide char clears its leader too.
    engine.set_cursor_pos(4, 0);
    engine.print_char(U'字');
    engine.set_cursor_pos(5, 0);
    engine.erase_in_line(0);
    CHECK(engine.grid().cell(4, 0).codepoint == U' ');
    CHECK(engine.grid().cell(4, 0).attrs.width == 1);
}

TEST_CASE("Glyph batching stays safe across small LRU eviction and atlas rebuilds") {
    FontEngine limited(AntialiasMode::Grayscale, {1024 * 1024, 4});
    REQUIRE(limited.load_font(find_test_font(), 13.0, 96));
    TerminalEngine engine(20, 1);
    engine.set_cursor_visible(false);
    for (int i = 0; i < 20; ++i) {
        Cell c;
        c.codepoint = static_cast<char32_t>(U'A' + i);
        engine.grid().set_cell(i, 0, c);
    }
    const int w = 20 * limited.metrics().cell_width;
    const int h = limited.metrics().cell_height;
    gritty::util::PixmanImage batched(pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, nullptr, 0));
    gritty::util::PixmanImage unbatched(pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, nullptr, 0));
    CpuRenderer r_batch(limited), r_plain(limited);
    r_plain.set_glyph_batching(false);
    r_batch.render(engine, batched.get(), nullptr, true);
    r_plain.render(engine, unbatched.get(), nullptr, true);
    for (int y = 0; y < h; ++y) {
        const auto* a = batched.data() + y * (batched.stride() / 4);
        const auto* b = unbatched.data() + y * (unbatched.stride() / 4);
        for (int x = 0; x < w; ++x) REQUIRE(a[x] == b[x]);
    }
}

TEST_CASE("Placed images respect x_offset/y_offset and exact draw_w/draw_h") {
    FontEngine fonts;
    TerminalEngine engine(4, 3);
    engine.set_cursor_visible(false);
    const int cw = fonts.metrics().cell_width;
    const int ch = fonts.metrics().cell_height;

    gritty::util::PixmanImage src(pixman_image_create_bits(PIXMAN_a8r8g8b8, 2, 2, nullptr, 0));
    REQUIRE(src.valid());
    std::fill(src.data(), src.data() + static_cast<size_t>(src.stride() / 4) * 2, 0xFF00FF00u);

    PlacedImage placed;
    placed.image = std::move(src);
    placed.col = 1;
    placed.row = 1;
    placed.cols = 1;
    placed.rows = 1;
    placed.x_offset = 2;
    placed.y_offset = 3;
    placed.draw_w = std::max(2, cw - 3);
    placed.draw_h = std::max(2, ch - 4);
    engine.place_image(std::move(placed));

    gritty::util::PixmanImage target(pixman_image_create_bits(PIXMAN_a8r8g8b8, 4 * cw, 3 * ch, nullptr, 0));
    CpuRenderer renderer(fonts);
    renderer.render(engine, target.get(), nullptr, true);
    const int stride = target.stride() / 4;
    CHECK(target.data()[ch * stride + cw] == 0xFF111111u);
    CHECK(target.data()[(ch + 3) * stride + cw + 2] == 0xFF00FF00u);
}

#if defined(PLATFORM_MACOS)
TEST_CASE("Oversized Nerd Font PUA icons are constrained within single-cell dimensions") {
    const char* home = std::getenv("HOME");
    if (!home) return;
    const auto nf_path = std::filesystem::path(home) / "Library/Fonts/JetBrainsMonoNerdFont-Regular.ttf";
    if (!std::filesystem::exists(nf_path)) {
        WARN("JetBrainsMonoNerdFont-Regular.ttf not installed, skipping Nerd Font icon test");
        return;
    }
    FontEngine fonts;
    REQUIRE(fonts.load_font(nf_path.string(), 14.0, 192));
    const auto& m = fonts.metrics();
    for (const char32_t cp : {char32_t{0xF07B}, char32_t{0xE5FF}, char32_t{0xF15B},
                              char32_t{0xF10C}, char32_t{0xEA87}}) {
        const auto* g = fonts.get_glyph(cp, false, false);
        REQUIRE(g != nullptr);
        CHECK(g->width <= m.cell_width + 1);
        CHECK(g->height <= m.cell_height);
    }
}
#endif

TEST_CASE("Unicode placeholders clip strictly to placeholder cells and never paint over floating windows") {
    FontEngine fonts;
    TerminalEngine engine(7, 5);
    engine.set_cursor_visible(false);
    auto& grid = engine.grid();

    // Red 2x3 image scaled into a 4x3 virtual placeholder grid (32x48 px at default 8x16 cells).
    REQUIRE(engine.kitty_graphics().process_apc_sequence(
                "Ga=T,U=1,i=21,c=4,r=3,f=32,s=2,v=3;"
                "/wAA//8AAP//AAD//wAA//8AAP//AAD/", 0, 0).find("OK") != std::string::npos);

    auto place_cell = [&](int gx, int gy, int img_row, int img_col) {
        Cell cell;
        cell.codepoint = 0x10EEEE;
        cell.fg = Color::make_indexed(21);
        const std::vector<char32_t> marks{
            kPlaceholderDiacritics[static_cast<size_t>(img_row)],
            kPlaceholderDiacritics[static_cast<size_t>(img_col)]};
        const auto extra_idx = grid.store_combining_codepoints(marks);
        grid.set_cell_with_extra(gx, gy, cell, CellExtra{.combining_idx = extra_idx});
    };
    // Fill a 4x3 placeholder block at (1,1)..(4,3).
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            place_cell(1 + c, 1 + r, r, c);

    // Overwrite a top-middle notch (2,1) and (3,1) with a green popup ('P') -> inverted U-shape
    // that tests both exact hole clipping AND Union-Find uniting left & right arms on row 2.
    Cell popup;
    popup.codepoint = U'P';
    popup.bg = Color::make_rgb(0x00, 0xff, 0x00);
    popup.fg = Color::make_rgb(0xff, 0xff, 0xff);
    grid.set_cell(2, 1, popup);
    grid.set_cell(3, 1, popup);

    const int cw = fonts.metrics().cell_width, ch = fonts.metrics().cell_height;
    gritty::util::PixmanImage target(pixman_image_create_bits(PIXMAN_a8r8g8b8, 7 * cw, 5 * ch, nullptr, 0));
    CpuRenderer renderer(fonts);
    renderer.render(engine, target.get(), nullptr, true);

    const int stride = target.stride() / 4;
    auto at_tl = [&](int gx, int gy) { return target.data()[gy * ch * stride + gx * cw]; };
    auto at_mid = [&](int gx, int gy) { return target.data()[(gy * ch + ch / 2) * stride + gx * cw + cw / 2]; };
    // Popup cells must NOT be painted red by the placeholder bounding box.
    CHECK(at_tl(2, 1) == 0xFF00FF00u);
    CHECK(at_tl(3, 1) == 0xFF00FF00u);
    // Remaining placeholder cells across both arms and bottom row show red image ink.
    CHECK(at_mid(1, 1) == 0xFFFF0000u);
    CHECK(at_mid(4, 1) == 0xFFFF0000u);
    CHECK(at_mid(2, 2) == 0xFFFF0000u);
    CHECK(at_mid(4, 3) == 0xFFFF0000u);

    // Incremental update: overwriting one more cell (1,1) with popup preserves (4,1) and keeps (1,1) green.
    grid.set_cell(1, 1, popup);
    renderer.render(engine, target.get(), nullptr, false);
    CHECK(at_tl(1, 1) == 0xFF00FF00u);
    CHECK(at_tl(2, 1) == 0xFF00FF00u);
    CHECK(at_mid(4, 1) == 0xFFFF0000u);
}

TEST_CASE("Partially scrolled-out direct placement stays clipped to scroll region top") {
    FontEngine fonts;
    TerminalEngine engine(6, 5);
    engine.set_cursor_visible(false);
    // Reserve row 0 as a header (tabline); scroll region is rows 1..4.
    engine.set_margins(1, 4);
    Cell header;
    header.codepoint = U' ';
    header.bg = Color::make_rgb(0x00, 0x00, 0xff);
    for (int x = 0; x < 6; ++x) engine.grid().set_cell(x, 0, header);

    gritty::util::PixmanImage src(pixman_image_create_bits(PIXMAN_a8r8g8b8, 2, 2, nullptr, 0));
    std::fill(src.data(), src.data() + static_cast<size_t>(src.stride() / 4) * 2, 0xFFFF0000u);
    PlacedImage placed;
    placed.image = std::move(src);
    placed.col = 1; placed.row = 1; placed.cols = 2; placed.rows = 3;
    engine.place_image(std::move(placed));
    engine.scroll_up(1); // Moves placed.row to 0, but clip_top_row stays 1!

    const int cw = fonts.metrics().cell_width, ch = fonts.metrics().cell_height;
    gritty::util::PixmanImage target(pixman_image_create_bits(PIXMAN_a8r8g8b8, 6 * cw, 5 * ch, nullptr, 0));
    CpuRenderer renderer(fonts);
    renderer.render(engine, target.get(), nullptr, true);
    const int stride = target.stride() / 4;
    CHECK(target.data()[(ch / 2) * stride + cw + cw / 2] == 0xFF0000FFu); // Header stays blue!
    CHECK(target.data()[(ch + ch / 2) * stride + cw + cw / 2] == 0xFFFF0000u); // Visible part inside region!
}

TEST_CASE("apply_pending_scrolls rejects DECSTBM scroll when bottom exceeds clipped target height") {
    FontEngine fonts;
    TerminalEngine engine(10, 8);
    engine.set_cursor_visible(false);
    const int cw = fonts.metrics().cell_width, ch = fonts.metrics().cell_height;
    // Target is only 6 rows tall (6 * ch), while the DECSTBM region reaches row 7 ((bottom+1)*ch = 8*ch).
    gritty::util::PixmanImage target(pixman_image_create_bits(PIXMAN_a8r8g8b8, 10 * cw, 6 * ch, nullptr, 0));
    CpuRenderer renderer(fonts);
    renderer.render(engine, target.get(), nullptr, true);

    Cell marker;
    marker.codepoint = U'X';
    marker.bg = Color::make_rgb(0x20, 0x60, 0xa0);
    engine.grid().set_cell(0, 4, marker);
    renderer.render(engine, target.get(), nullptr, false);

    // Scroll region [3..7] up by 1: region_rows() == 5 (<= 6), but (bottom + 1) == 8 (> 6).
    Cell blank;
    engine.grid().scroll_up(3, 7, 1, blank);
    const auto skips_before = renderer.stats().scroll_skips;
    renderer.render(engine, target.get(), nullptr, false);
    // Must fall back safely without out-of-bounds memmove and render row 3 with marker's blue bg.
    CHECK(renderer.stats().scroll_moves == 0);
    (void)skips_before;
    const int stride = target.stride() / 4;
    CHECK(target.data()[(3 * ch) * stride] == 0xFF2060A0u);
}

TEST_CASE("FontEngine set_style_font and add_fallback_font invalidate L1 glyph caches") {
    const auto path = find_test_font();
    if (path.empty()) return;
    FontEngine fonts;
    REQUIRE(fonts.load_font(path, 12.0));
    // Populate L1 caches (ASCII + non-ASCII + shaped cluster).
    REQUIRE(fonts.get_glyph(U'A', true, false) != nullptr);
    REQUIRE(fonts.get_glyph(U'\u4e2d', false, false) != nullptr);
    const auto misses_before = fonts.cache_stats().misses;
    REQUIRE(fonts.set_style_font(true, false, path));
    REQUIRE(fonts.get_glyph(U'A', true, false) != nullptr);
    CHECK(fonts.cache_stats().misses > misses_before);
    const auto misses_mid = fonts.cache_stats().misses;
    const std::string fallback_path = "/System/Library/Fonts/Apple Symbols.ttf";
    if (std::filesystem::exists(fallback_path)) {
        REQUIRE(fonts.add_fallback_font(fallback_path));
        REQUIRE(fonts.get_glyph(U'\u4e2d', false, false) != nullptr);
        CHECK(fonts.cache_stats().misses > misses_mid);
    }
}

TEST_CASE("CpuRenderer draws inline IME preedit and cleanly restores underlying cells on clear") {
    const auto path = find_test_font();
    if (path.empty()) return;
    FontEngine fonts;
    REQUIRE(fonts.load_font(path, 12.0));
    TerminalEngine engine(12, 3);
    engine.set_cursor_visible(false);
    engine.print_ascii_run("hello world!");
    engine.set_cursor_col(2);
    engine.set_cursor_row(0);

    const int cw = fonts.metrics().cell_width, ch = fonts.metrics().cell_height;
    gritty::util::PixmanImage baseline(pixman_image_create_bits(PIXMAN_a8r8g8b8, 12 * cw, 3 * ch, nullptr, 0));
    gritty::util::PixmanImage target(pixman_image_create_bits(PIXMAN_a8r8g8b8, 12 * cw, 3 * ch, nullptr, 0));
    CpuRenderer ref_renderer(fonts);
    CpuRenderer renderer(fonts);
    ref_renderer.render(engine, baseline.get(), nullptr, true);
    renderer.render(engine, target.get(), nullptr, true);

    gritty::util::PixmanRegion damage;
    renderer.set_preedit("ni'好", 0, 2);
    renderer.render(engine, target.get(), &damage, false);
    CHECK_FALSE(damage.empty());
    bool differed = false;
    for (int i = 0; i < 3 * ch * (target.stride() / 4); ++i) {
        if (target.data()[i] != baseline.data()[i]) {
            differed = true;
            break;
        }
    }
    CHECK(differed);

    renderer.set_preedit("", 0, 0);
    renderer.render(engine, target.get(), &damage, false);
    CHECK_FALSE(damage.empty());
    for (int i = 0; i < 3 * ch * (target.stride() / 4); ++i) {
        REQUIRE(target.data()[i] == baseline.data()[i]);
    }
}
