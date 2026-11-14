#include <doctest/doctest.h>
#include "core/sixel_decoder.hpp"

using namespace squirt::core;

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
