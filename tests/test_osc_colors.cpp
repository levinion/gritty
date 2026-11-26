#include <doctest/doctest.h>
#include <array>
#include <cstdio>
#include "config/toml_loader.hpp"
#include "core/vt_parser.hpp"

namespace {
std::string color_reply(int command, uint32_t color, bool with_st = true) {
    char text[80];
    std::snprintf(text, sizeof(text), "\033]%d;rgb:%04x/%04x/%04x%s", command,
                  ((color >> 16) & 255) * 257, ((color >> 8) & 255) * 257, (color & 255) * 257,
                  with_st ? "\033\\" : "\a");
    return text;
}
}

TEST_CASE("OSC default colors use bounded foot-style hex components and notify real rendering") {
    struct ColorCase { const char* value; uint32_t rgb; };
    const std::array cases{
        ColorCase{"#fff", 0xf0f0f0}, ColorCase{"#000000", 0x000000},
        ColorCase{"#12aB34", 0x12ab34},
        ColorCase{"#f00123abc", 0xf012ab}, ColorCase{"#f0001234abcd", 0xf012ab},
        ColorCase{"rgb:f/0/a", 0xf000a0}, ColorCase{"rgb:012/ab/cdef", 0x01abcd},
        ColorCase{"rgb:f0f0/8080/1234", 0xf08012},
    };
    for (const auto& item : cases) {
        INFO(std::string(item.value));
        gritty::core::TerminalEngine engine(4, 2);
        gritty::core::VtParser parser(engine);
        parser.set_default_colors(0xff010203, 0x80040506);
        uint32_t fg = 0, bg = 0;
        int updates = 0;
        parser.set_default_color_handler([&](uint32_t foreground, uint32_t background) {
            fg = foreground; bg = background; ++updates;
        });
        std::string replies;
        parser.set_response_callback([&](std::string_view reply) { replies += reply; });
        const auto sequence = std::string("\033]10;") + item.value + "\a\033]11;" + item.value + "\033\\";
        for (const char byte : sequence) parser.feed(std::string_view(&byte, 1));
        CHECK(fg == (0xff000000 | item.rgb));
        CHECK(bg == (0x80000000 | item.rgb));
        CHECK(updates == 2);
        parser.feed("\033]10;?\a\033]11;?\033\\");
        // Replies mirror each request's own terminator.
        CHECK(replies == color_reply(10, fg, false) + color_reply(11, bg, true));
        parser.feed(sequence); // Identical colors must not force another full redraw.
        CHECK(updates == 2);
        parser.feed("\033]110\a\033]111\033\\"); // Resets have no semicolon.
        CHECK(fg == 0xff010203);
        CHECK(bg == 0x80040506);
        CHECK(updates == 4);
        parser.feed(sequence);
        parser.feed("\033c");
        CHECK(fg == 0xff010203);
        CHECK(bg == 0x80040506);
    }
}

TEST_CASE("OSC 4/12 queries and palette resets use request terminators") {
    gritty::core::TerminalEngine engine(4, 2);
    gritty::core::VtParser parser(engine);
    parser.set_default_colors(0xff010203, 0x80040506, 0xff0a0b0c);
    std::string replies;
    parser.set_response_callback([&](std::string_view reply) { replies.append(reply); });

    parser.feed("\033]10;?\a\033]11;?\033\\\033]12;?\a");
    CHECK(replies == color_reply(10, 0xff010203, false) + color_reply(11, 0x80040506, true) +
          color_reply(12, 0xff0a0b0c, false));

    replies.clear();
    parser.feed("\033]4;1;?;2;?\033\\");
    const std::string indexed_st = "\033]4;1;rgb:f6f6/7e7e/7d7d\033\\"
                                   "\033]4;2;rgb:caca/e7e7/b9b9\033\\";
    CHECK(replies == indexed_st);
    const std::string indexed = "\033]4;1;rgb:f6f6/7e7e/7d7d\a"
                                "\033]4;2;rgb:caca/e7e7/b9b9\a";
    replies.clear();
    parser.feed("\033]4;1;?;2;?\a");
    CHECK(replies == indexed);

    parser.feed("\033]4;1;#010203\a");
    CHECK(engine.palette_color(1) == 0x010203);
    parser.feed("\033]104;1\a");
    CHECK(engine.palette_color(1) == 0xf67e7d);
    parser.feed("\033]4;1;#010203\a\033]104;\033\\");
    CHECK(engine.palette_color(1) == 0xf67e7d);
    parser.feed("\033]4;2;#010203\a\033]104\a");
    CHECK(engine.palette_color(2) == 0xcae7b9);

    parser.feed("\033]12;#112233\a");
    replies.clear();
    parser.feed("\033]12;?\a");
    CHECK(replies == color_reply(12, 0xff112233, false));
    parser.feed("\033]112\a");
    replies.clear();
    parser.feed("\033]12;?\a");
    CHECK(replies == color_reply(12, 0xff0a0b0c, false));
}

TEST_CASE("Malformed OSC colors and cancelled setters cannot change query or renderer state") {
    gritty::core::TerminalEngine engine(4, 2);
    gritty::core::VtParser parser(engine);
    parser.set_default_colors(0xff010203, 0x80040506);
    int updates = 0;
    parser.set_default_color_handler([&](uint32_t, uint32_t) { ++updates; });
    std::string replies;
    parser.set_response_callback([&](std::string_view reply) { replies += reply; });
    for (const auto* bad : {"#12", "#1234", "#12345678", "#1234567890123", "#12345g",
                            "rgb:/0/0", "rgb:0/0/", "rgb:00000/0/0", "rgb:0/0/0/0",
                            "rgb:00/00/00junk", "rgba:ffff/ffff/ffff/ffff", "red"}) {
        INFO(std::string(bad));
        parser.feed(std::string("\033]10;") + bad + "\a\033]11;" + bad + "\a");
    }
    parser.feed("\033]10;#ffffff\030\033]11;#ffffff\032");
    parser.feed("\033]10;?\a\033]11;?\a");
    CHECK(updates == 0);
    CHECK(replies == color_reply(10, 0xff010203, false) + color_reply(11, 0x80040506, false));
}

TEST_CASE("Custom ANSI colors preserve the xterm color cube and grayscale ramp") {
    const auto loaded = gritty::config::TomlLoader::load_from_string("[colors]\nregular1 = '#123456'\n");
    REQUIRE(loaded.has_value());
    const auto& palette = (*loaded)->colors.palette;
    CHECK(palette[1] == 0x123456);
    CHECK(palette[33] == 0x0087ff);
    CHECK(palette[196] == 0xff0000);
    CHECK(palette[244] == 0x808080);
    gritty::core::TerminalEngine engine(4, 2);
    engine.configure_palette(palette);
    engine.set_palette_color(1, 0xabcdef);
    engine.reset();
    CHECK(engine.palette_color(1) == 0x123456);
    CHECK(engine.palette_color(33) == 0x0087ff);
    CHECK(engine.palette_color(196) == 0xff0000);
    CHECK(engine.palette_color(244) == 0x808080);
}
