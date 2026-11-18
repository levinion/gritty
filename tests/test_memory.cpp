#include <doctest/doctest.h>
#include <memory>
#include "core/screen_grid.hpp"
#include "util/ring_buffer.hpp"
#include "platform/cpu_frame_pool.hpp"

TEST_CASE("Equal combining sequences share a side-table entry") {
    gritty::core::ScreenGrid grid(8, 3, 2);
    const char32_t marks[] = {U'\u0301', U'\u0327'};
    const auto first = grid.store_combining_codepoints(marks);
    REQUIRE(first != 0);
    for (int i = 0; i < 1000; ++i)
        REQUIRE(grid.store_combining_codepoints(marks) == first);
}

TEST_CASE("Removing ring entries releases owned storage immediately") {
    gritty::util::RingBuffer<std::shared_ptr<int>> ring(4);
    auto value = std::make_shared<int>(42);
    std::weak_ptr<int> observer = value;
    ring.push_back(std::move(value));
    SUBCASE("clear") { ring.clear(); }
    SUBCASE("pop front") { ring.pop_front(); }
    SUBCASE("pop back") { ring.pop_back(); }
    CHECK(observer.expired());
}

TEST_CASE("Scrolling reuses discarded row storage rather than allocating each line") {
    gritty::core::ScreenGrid grid(8, 3, 0);
    grid.row(0).cells.reserve(128);
    grid.row(1).cells.reserve(256);
    grid.row(2).cells.reserve(512);
    const auto* first = grid.row(0).cells.data();
    const auto capacity = grid.row(0).cells.capacity();
    grid.scroll_up(0, 2, 1);
    CHECK(grid.row(2).cells.data() == first);
    CHECK(grid.row(2).cells.capacity() == capacity);
    grid.scroll_down(0, 2, 1);
    CHECK(grid.row(0).cells.data() == first);
    CHECK(grid.row(0).cells.capacity() == capacity);
}

TEST_CASE("A full scrollback ring returns its oldest row buffer for the next blank line") {
    gritty::core::ScreenGrid grid(8, 3, 2);
    grid.row(0).cells.reserve(128);
    const auto capacity = grid.row(0).cells.capacity();
    const auto* first = grid.row(0).cells.data();
    grid.scroll_up(0, 2, 1);
    grid.scroll_up(0, 2, 1);
    REQUIRE(grid.scrollback_size() == 2);
    grid.scroll_up(0, 2, 1);
    CHECK(grid.row(2).cells.data() == first);
    CHECK(grid.row(2).cells.capacity() == capacity);
    CHECK(grid.scrollback_size() == 2);
}

TEST_CASE("Recycled history does not retain obsolete wide row allocations") {
    gritty::core::ScreenGrid grid(128, 3, 2);
    grid.scroll_up(0, 2, 2);
    grid.resize(8, 3);
    CHECK(grid.row(0).cells.capacity() <= 16);
    REQUIRE(grid.scrollback_row(0).cells.size() == 128);
    grid.scroll_up(0, 2, 1);
    CHECK(grid.row(2).cells.capacity() <= 16);
}

TEST_CASE("Mapped pixel storage follows the last Pixman reference, not just the pool") {
    gritty::platform::CpuFramePool pool;
    pool.resize(64, 64);
    auto* image = pool.get_back_buffer();
    REQUIRE(image != nullptr);
    pixman_image_get_data(image)[0] = 0xff123456;
    gritty::util::PixmanImage retained(pixman_image_ref(image));
    pool.resize(80, 64);
    CHECK(pool.live_frames() == 2);
    CHECK(retained.data()[0] == 0xff123456);
    pool.resize(0, 0);
    CHECK(pool.live_frames() == 1);
    CHECK(pool.live_bytes() == 64 * 64 * 4);
    retained.reset();
    CHECK(pool.live_frames() == 0);
    CHECK(pool.live_bytes() == 0);
}

TEST_CASE("Idle frame trimming preserves leases and the incremental baseline") {
    gritty::platform::CpuFramePool pool;
    pool.resize(64, 64);
    auto* old_image = pool.get_back_buffer();
    REQUIRE(old_image != nullptr);
    pixman_image_get_data(old_image)[0] = 0xff123456;
    auto old_lease = pool.lease();
    auto* current = pool.get_back_buffer();
    REQUIRE(current != nullptr);
    pixman_image_get_data(current)[1] = 0xffabcdef;
    auto displayed = pool.lease();
    auto* newest = pool.get_back_buffer();
    REQUIRE(newest != nullptr);
    CHECK(pool.live_frames() == 3);
    displayed.reset();
    pool.release_spare_buffers();
    CHECK(pool.live_frames() == 2); // The external old-image lease still owns its mapping.
    CHECK(pixman_image_get_data(old_image)[0] == 0xff123456);
    old_lease.reset();
    CHECK(pool.live_frames() == 1);
    CHECK(pool.live_bytes() == 64 * 64 * 4);
    auto live = pool.lease();
    auto* next = pool.get_back_buffer();
    REQUIRE(next != nullptr);
    CHECK(pixman_image_get_data(next)[0] == 0xff123456);
    CHECK(pixman_image_get_data(next)[1] == 0xffabcdef);
    live.reset();
    pool.release_spare_buffers();
    pool.release_spare_buffers(); // Idempotent, including an empty pool.
    CHECK(pool.live_frames() == 1);
    pool.resize(0, 0);
    pool.release_spare_buffers();
    CHECK(pool.live_frames() == 0);
}

TEST_CASE("CPU frame pool recycles snapshots without altering leased images") {
    gritty::platform::CpuFramePool pool;
    pool.resize(64, 64);
    auto* original = pool.get_back_buffer();
    REQUIRE(original != nullptr);
    pixman_image_get_data(original)[0] = 0xff123456;
    pixman_image_get_data(original)[1] = 0;
    auto oldest = pool.lease();
    std::unique_ptr<gritty::platform::CpuFramePool::Lease> displayed;
    for (int frame = 0; frame < 200; ++frame) {
        auto* writable = pool.get_back_buffer();
        REQUIRE(writable != nullptr);
        CHECK(pixman_image_get_data(writable)[0] == 0xff123456);
        pixman_image_get_data(writable)[1] = frame + 1;
        displayed = pool.lease();
        CHECK(pixman_image_get_data(original)[1] == 0);
    }
    CHECK(pool.allocation_count() == 3);
    pool.resize(128, 64);
    CHECK(pixman_image_get_data(original)[0] == 0xff123456);
    CHECK(pool.allocation_count() == 4);
}
