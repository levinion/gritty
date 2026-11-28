#include <doctest/doctest.h>
#include "util/base64.hpp"
#include "util/raii.hpp"
#include <string>
#include <vector>

using namespace gritty::util;

TEST_CASE("PixmanRegion RAII operations") {
    PixmanRegion r1(0, 0, 100, 100);
    CHECK(!r1.empty());
    CHECK(r1.num_rects() == 1);

    PixmanRegion r2(50, 50, 100, 100);
    r1.intersect(r2);
    CHECK(!r1.empty());

    pixman_box32_t ext = r1.extents();
    CHECK(ext.x1 == 50);
    CHECK(ext.y1 == 50);
    CHECK(ext.x2 == 100);
    CHECK(ext.y2 == 100);
}

TEST_CASE("PixmanImage RAII operations") {
    pixman_image_t* raw = pixman_image_create_bits(PIXMAN_a8r8g8b8, 64, 32, nullptr, 64 * 4);
    PixmanImage img(raw);
    CHECK(img.valid());
    CHECK(img.width() == 64);
    CHECK(img.height() == 32);
    CHECK(img.stride() == 64 * 4);
    CHECK(img.data() != nullptr);
}

TEST_CASE("NEON Base64 decoder matches scalar round-trips across boundaries and error modes") {
    for (size_t len = 0; len <= 300; ++len) {
        std::string raw(len, '\0');
        for (size_t i = 0; i < len; ++i) raw[i] = static_cast<char>((i * 73 + 19) & 0xff);
        const std::string encoded = encode_base64(raw);
        const auto strict = decode_base64(encoded, Base64Mode::Strict, len + 16);
        REQUIRE(strict.has_value());
        CHECK(strict->size() == len);
        CHECK(std::string(reinterpret_cast<const char*>(strict->data()), strict->size()) == raw);
    }

    // Strict rejects invalid ASCII and high-bit bytes at every lane offset 0..63.
    const std::string payload(150, 'x');
    const std::string valid = encode_base64(payload);
    for (size_t pos = 0; pos < 64; ++pos) {
        for (const char bad_char : {'!', '@', ' ', static_cast<char>(0x80)}) {
            std::string corrupted = valid;
            corrupted[pos] = bad_char;
            CHECK_FALSE(decode_base64(corrupted, Base64Mode::Strict, 256).has_value());
        }
    }

    // Relaxed skips whitespace/newlines between and across 64-byte chunks.
    std::string wrapped;
    for (size_t i = 0; i < valid.size(); i += 64) {
        wrapped.append(valid.substr(i, 64));
        wrapped.push_back('\n');
    }
    const auto relaxed = decode_base64(wrapped, Base64Mode::Relaxed, 256);
    REQUIRE(relaxed.has_value());
    CHECK(std::string(reinterpret_cast<const char*>(relaxed->data()), relaxed->size()) == payload);

    // max_bytes enforcement works with NEON + tail boundaries.
    CHECK_FALSE(decode_base64(valid, Base64Mode::Strict, payload.size() - 1).has_value());
    CHECK(decode_base64(valid, Base64Mode::Strict, payload.size()).has_value());
}
