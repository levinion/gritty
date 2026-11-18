#include <doctest/doctest.h>
#include <array>
#include <cstdio>
#include "core/vt_parser.hpp"

namespace {
std::string color_reply(int command, uint32_t color) {
    char text[80];
    std::snprintf(text, sizeof(text), "\033]%d;rgb:%04x/%04x/%04x\033\\", command,
                  ((color >> 16) & 255) * 257, ((color >> 8) & 255) * 257, (color & 255) * 257);
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
        CHECK(replies == color_reply(10, fg) + color_reply(11, bg));
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
    CHECK(replies == color_reply(10, 0xff010203) + color_reply(11, 0x80040506));
}
