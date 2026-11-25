#include <doctest/doctest.h>
#include "util/raii.hpp"

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
