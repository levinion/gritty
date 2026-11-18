#include <doctest/doctest.h>
#include "core/sixel_decoder.hpp"
#include "core/vt_parser.hpp"
#include "core/terminal_session.hpp"

using namespace gritty::core;

TEST_CASE("SixelDecoder basic stream decode") {
    // Defines color 1 as Red (100% R, 0% G, 0% B), selects #1, draws 2 sixels of all 6 bits (~)
    std::string sixel_data = "q#1;2;100;0;0#1~~";
    auto img_opt = SixelDecoder::decode(sixel_data);
    REQUIRE(img_opt.has_value());

    const auto& img = *img_opt;
    CHECK(img.width() >= 2);
    CHECK(img.height() >= 6);

    uint32_t* pixels = img.data();
    // Pixels should be red (0xFFFF0000)
    CHECK(pixels[0] == 0xFFFF0000);
    CHECK(pixels[1] == 0xFFFF0000);
}

TEST_CASE("SixelDecoder repeat count operator") {
    // !5~ repeats '~' 5 times
    std::string sixel_data = "q#1;2;0;100;0#1!5~";
    auto img_opt = SixelDecoder::decode(sixel_data);
    REQUIRE(img_opt.has_value());

    const auto& img = *img_opt;
    CHECK(img.width() >= 5);
    uint32_t* pixels = img.data();
    for (int x = 0; x < 5; ++x) {
        CHECK(pixels[x] == 0xFF00FF00); // Green
    }
}

TEST_CASE("VtParser streams DCS strings larger than 256 KiB without truncation") {
    gritty::core::TerminalEngine engine(80, 24);
    gritty::core::VtParser parser(engine);
    std::string received;
    parser.set_dcs_handler([&](std::string_view payload) { received.assign(payload); });
    std::string stream = "\x1bPq";
    stream.append(300 * 1024, '~');
    stream += "\x1b\\";
    parser.feed(stream);
    CHECK(received.size() == 300 * 1024 + 1); // q + sixel data
}

TEST_CASE("tmux DCS passthrough unwraps Sixel data") {
    gritty::core::TerminalEngine engine(80, 24);
    gritty::core::VtParser parser(engine);
    std::string received;
    parser.set_dcs_handler([&](std::string_view payload) { received.assign(payload); });
    parser.feed("\x1bPtmux;\x1b\x1bPq?\x1b\x1b\\\x1b\\");
    CHECK(received == "q?");
}

TEST_CASE("Sixel HLS palette and P2 background mode") {
    auto hls = SixelDecoder::decode("q#1;1;0;50;100#1~");
    REQUIRE(hls.has_value());
    CHECK(hls->data()[0] == 0xFFFF0000);

    auto opaque = SixelDecoder::decode("q#1?", false);
    REQUIRE(opaque.has_value());
    CHECK(opaque->data()[opaque->stride() / 4 + 1] == 0xFF000000);
    auto transparent = SixelDecoder::decode("q#1?", true);
    REQUIRE(transparent.has_value());
    CHECK(transparent->data()[transparent->stride() / 4 + 1] == 0x00000000);
}
