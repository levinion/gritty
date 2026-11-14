#include <doctest/doctest.h>
#include <algorithm>
#include <filesystem>
#include "core/vt_parser.hpp"
#include "render/cpu_renderer.hpp"

using namespace gritty::core;
using namespace gritty::render;

namespace {
std::string alpha_test_font() {
    for (const auto* path : {"/System/Library/Fonts/Monaco.ttf", "/System/Library/Fonts/Menlo.ttc",
                            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"}) {
        if (std::filesystem::exists(path)) return path;
    }
    return {};
}
}

TEST_CASE("Transparent background pixels are premultiplied, including window margins") {
    FontEngine fonts;
    TerminalEngine engine(4, 2);
    engine.set_cursor_visible(false);
    RenderConfig config;
    config.default_bg = 0x80804020;
    CpuRenderer renderer(fonts, config);
    const int width = 4 * fonts.metrics().cell_width + 3;
    const int height = 2 * fonts.metrics().cell_height + 5;
    gritty::util::PixmanImage image(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    renderer.render(engine, image.get(), nullptr, true);
    CHECK(image.data()[0] == 0x80402010);
    CHECK(image.data()[(height - 1) * image.stride() / 4 + width - 1] == 0x80402010);

    gritty::util::PixmanImage backdrop(pixman_image_create_bits(PIXMAN_a8r8g8b8, 1, 1, nullptr, 0));
    backdrop.data()[0] = 0xffffffff;
    pixman_image_composite32(PIXMAN_OP_OVER, image.get(), nullptr, backdrop.get(),
        0, 0, 0, 0, 0, 0, 1, 1);
    CHECK(backdrop.data()[0] == 0xffbf9f8f);
}

TEST_CASE("Fully transparent backgrounds contain no hidden unpremultiplied RGB") {
    FontEngine fonts;
    TerminalEngine engine(2, 1);
    engine.set_cursor_visible(false);
    RenderConfig config;
    config.default_bg = 0x00ffffff;
    CpuRenderer renderer(fonts, config);
    gritty::util::PixmanImage image(pixman_image_create_bits(PIXMAN_a8r8g8b8, 16, 16, nullptr, 0));
    renderer.render(engine, image.get(), nullptr, true);
    CHECK(image.data()[0] == 0);
    CHECK(image.data()[image.stride() * 16 / 4 - 1] == 0);
}

TEST_CASE("Background alpha does not dim explicit terminal backgrounds or reverse-video text") {
    FontEngine fonts;
    TerminalEngine engine(5, 1);
    engine.set_cursor_visible(false);
    VtParser parser(engine);
    parser.feed(" \x1b[48;2;128;64;32m \x1b[44m \x1b[0;7m█\x1b[0m█");
    RenderConfig config;
    config.default_bg = 0x80804020;
    CpuRenderer renderer(fonts, config);
    const int width = 5 * fonts.metrics().cell_width;
    gritty::util::PixmanImage image(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, 16, nullptr, 0));
    renderer.render(engine, image.get(), nullptr, true);
    const int cell = fonts.metrics().cell_width;
    CHECK(image.data()[0] == 0x80402010);
    CHECK(image.data()[cell] == 0xff804020); // explicit RGB, even when it matches default RGB
    CHECK(image.data()[2 * cell] == (0xff000000 | engine.palette_color(4)));
    CHECK(image.data()[3 * cell] == 0xff804020); // reverse-video foreground must stay opaque
    CHECK(image.data()[4 * cell] == config.default_fg);
}

TEST_CASE("Transparent incremental redraws do not accumulate opacity or leave cursor trails") {
    FontEngine fonts;
    TerminalEngine engine(5, 2);
    RenderConfig config;
    config.default_bg = 0x80604020;
    CpuRenderer renderer(fonts, config);
    gritty::util::PixmanImage image(pixman_image_create_bits(PIXMAN_a8r8g8b8, 40, 32, nullptr, 0));
    gritty::util::PixmanImage expected(pixman_image_create_bits(PIXMAN_a8r8g8b8, 40, 32, nullptr, 0));
    auto check = [&] {
        renderer.render(engine, image.get());
        CpuRenderer fresh(fonts, config);
        fresh.render(engine, expected.get(), nullptr, true);
        CHECK(std::equal(image.data(), image.data() + 32 * image.stride() / 4, expected.data()));
    };
    check();
    check();
    engine.set_cursor_pos(3, 1);
    check();
    engine.set_cursor_visible(false);
    check();
    engine.print_char(U'█');
    check();
    engine.erase_in_display(2);
    check();
}

TEST_CASE("Transparent windows use grayscale instead of LCD without losing the requested AA mode") {
    const auto font = alpha_test_font();
    if (font.empty()) { WARN("No font for alpha glyph test"); return; }
    FontEngine fonts(AntialiasMode::SubpixelLcd);
    REQUIRE(fonts.load_font(font, 13.0));
    TerminalEngine engine(2, 1);
    engine.set_cursor_visible(false);
    engine.print_char(U'A');
    RenderConfig config;
    CpuRenderer renderer(fonts, config);
    const int width = 2 * fonts.metrics().cell_width + 3;
    const int height = fonts.metrics().cell_height + 2;
    gritty::util::PixmanImage image(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    renderer.render(engine, image.get());
    REQUIRE(fonts.get_glyph(U'A', false, false) != nullptr);
    CHECK(fonts.get_glyph(U'A', false, false)->is_subpixel);
    config.default_bg = 0x80604020;
    renderer.set_config(config);
    renderer.render(engine, image.get()); // config change must repaint even the margins
    CHECK(fonts.antialias_mode() == AntialiasMode::SubpixelLcd);
    CHECK(fonts.effective_antialias_mode() == AntialiasMode::Grayscale);
    CHECK_FALSE(fonts.get_glyph(U'A', false, false)->is_subpixel);
    CHECK(image.data()[(height - 1) * image.stride() / 4 + width - 1] == 0x80302010);
    config.default_bg = 0xff604020;
    renderer.set_config(config);
    renderer.render(engine, image.get());
    CHECK(fonts.effective_antialias_mode() == AntialiasMode::SubpixelLcd);
    CHECK(fonts.get_glyph(U'A', false, false)->is_subpixel);
    CHECK(image.data()[(height - 1) * image.stride() / 4 + width - 1] == 0xff604020);
}

TEST_CASE("Grayscale glyph masks compose correctly over a completely clear background") {
    const auto font = alpha_test_font();
    if (font.empty()) { WARN("No font for alpha glyph test"); return; }
    FontEngine fonts;
    REQUIRE(fonts.load_font(font, 13.0));
    TerminalEngine engine(2, 1);
    engine.set_cursor_visible(false);
    engine.print_char(U'M');
    RenderConfig config;
    config.default_bg = 0x00ffffff;
    config.default_fg = 0xffffffff;
    CpuRenderer renderer(fonts, config);
    const int width = 2 * fonts.metrics().cell_width;
    const int height = fonts.metrics().cell_height;
    gritty::util::PixmanImage image(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    renderer.render(engine, image.get(), nullptr, true);
    bool visible = false;
    for (int i = 0; i < height * image.stride() / 4; ++i) {
        const uint32_t pixel = image.data()[i];
        const auto alpha = pixel >> 24;
        visible |= alpha > 0;
        REQUIRE(((pixel >> 16) & 255) == alpha);
        REQUIRE(((pixel >> 8) & 255) == alpha);
        REQUIRE((pixel & 255) == alpha);
    }
    CHECK(visible);
}
