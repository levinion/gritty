#include <doctest/doctest.h>
#include "util/raii.hpp"
#include "util/ring_buffer.hpp"
#include "util/arena.hpp"

using namespace gritty::util;

TEST_CASE("RingBuffer basic operations") {
    RingBuffer<int> rb(3);
    CHECK(rb.size() == 0);
    CHECK(rb.capacity() == 3);
    CHECK(rb.empty());
    CHECK(!rb.full());

    rb.push_back(10);
    rb.push_back(20);
    CHECK(rb.size() == 2);
    CHECK(rb[0] == 10);
    CHECK(rb[1] == 20);

    rb.push_back(30);
    CHECK(rb.size() == 3);
    CHECK(rb.full());
    CHECK(rb.front() == 10);
    CHECK(rb.back() == 30);

    // Push when full overwrites oldest
    rb.push_back(40);
    CHECK(rb.size() == 3);
    CHECK(rb[0] == 20);
    CHECK(rb[1] == 30);
    CHECK(rb[2] == 40);

    rb.pop_front();
    CHECK(rb.size() == 2);
    CHECK(rb[0] == 30);
    CHECK(rb[1] == 40);

    REQUIRE(rb.at(0) != nullptr);
    CHECK(*rb.at(0) == 30);
    CHECK(rb.at(2) == nullptr);
    CHECK(rb.at(99) == nullptr);
}

TEST_CASE("RingBuffer resizing capacity") {
    RingBuffer<int> rb(2);
    rb.push_back(1);
    rb.push_back(2);
    rb.set_capacity(4);
    CHECK(rb.size() == 2);
    CHECK(rb.capacity() == 4);
    CHECK(rb[0] == 1);
    CHECK(rb[1] == 2);

    rb.push_back(3);
    rb.push_back(4);
    CHECK(rb.size() == 4);
    CHECK(rb[3] == 4);

    RingBuffer<int> empty;
    CHECK(empty.emplace_back(7) == nullptr);
    REQUIRE(empty.emplace_back(8) == nullptr);
}

TEST_CASE("ScratchArena allocation") {
    ScratchArena arena(1024);
    int* p1 = arena.create<int>(42);
    CHECK(*p1 == 42);

    struct TestStruct {
        double a;
        int b;
    };
    TestStruct* s = arena.create<TestStruct>(3.14, 99);
    CHECK(s->a == 3.14);
    CHECK(s->b == 99);

    CHECK(arena.used_bytes() > 0);
    arena.reset();
    CHECK(arena.used_bytes() == 0);
}

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
