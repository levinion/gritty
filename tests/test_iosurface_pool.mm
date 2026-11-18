#include <doctest/doctest.h>
#include <IOSurface/IOSurface.h>
#include <limits>
#include <cstring>
#include "platform/macos/iosurface_pool.hpp"
#include "core/terminal_engine.hpp"
#include "core/vt_parser.hpp"
#include "render/cpu_renderer.hpp"
#include "font_fixture.hpp"

using gritty::platform::macos::IOSurfaceFramePool;
namespace {
class Pin final {
    IOSurfaceRef surface_;
public:
    explicit Pin(IOSurfaceRef surface) : surface_(surface) {
        CFRetain(surface_); IOSurfaceIncrementUseCount(surface_);
    }
    ~Pin() { release(); }
    void release() {
        if (!surface_) return;
        IOSurfaceDecrementUseCount(surface_); CFRelease(surface_); surface_ = nullptr;
    }
};
void equal_pixels(pixman_image_t* a, pixman_image_t* b) {
    REQUIRE(pixman_image_get_width(a) == pixman_image_get_width(b));
    REQUIRE(pixman_image_get_height(a) == pixman_image_get_height(b));
    for (int y = 0; y < pixman_image_get_height(a); ++y) {
        const auto* pa = reinterpret_cast<const uint8_t*>(pixman_image_get_data(a)) + y * pixman_image_get_stride(a);
        const auto* pb = reinterpret_cast<const uint8_t*>(pixman_image_get_data(b)) + y * pixman_image_get_stride(b);
        REQUIRE(std::memcmp(pa, pb, pixman_image_get_width(a) * 4) == 0);
    }
}
}

TEST_CASE("IOSurface pixels stay alive through the last raw Pixman reference") {
    IOSurfaceFramePool pool;
    pool.resize(64, 64);
    auto* pixels = pool.get_back_buffer();
    REQUIRE(pixels != nullptr);
    pixman_image_get_data(pixels)[0] = 0xff123456;
    gritty::util::PixmanImage retained(pixman_image_ref(pixels));
    pool.resize(80, 64);
    CHECK(pool.live_frames() == 2);
    CHECK(retained.data()[0] == 0xff123456);
    pool.resize(0, 0);
    CHECK(pool.live_frames() == 1);
    retained.reset();
    CHECK(pool.live_frames() == 0);
    CHECK(pool.live_bytes() == 0);
    {
        IOSurfaceFramePool temporary;
        temporary.resize(64, 64);
        auto* image = temporary.get_back_buffer();
        REQUIRE(image != nullptr);
        pixman_image_get_data(image)[0] = 0xffabcdef;
        retained.reset(pixman_image_ref(image));
    }
    CHECK(retained.data()[0] == 0xffabcdef);
}

TEST_CASE("Attached and in-use surfaces stay immutable and acquisition is bounded") {
    IOSurfaceFramePool pool;
    pool.resize(64, 64);
    auto* a = pool.get_back_buffer();
    REQUIRE(a != nullptr);
    pixman_image_get_data(a)[0] = 0xff123456;
    auto first = pool.prepare_present();
    REQUIRE(first != nullptr);
    pool.did_present();
    const size_t one_frame = IOSurfaceGetAllocSize(first);
    Pin first_reader(first);
    auto* b = pool.get_back_buffer();
    REQUIRE(b != nullptr);
    CHECK(b != a);
    CHECK(pixman_image_get_data(b)[0] == 0xff123456);
    pixman_image_get_data(b)[0] = 0xffabcdef;
    CHECK(pixman_image_get_data(a)[0] == 0xff123456);
    auto second = pool.prepare_present();
    pool.did_present();
    Pin second_reader(second);
    auto* c = pool.get_back_buffer();
    REQUIRE(c != nullptr);
    CHECK(c != a);
    CHECK(c != b);
    CHECK(pixman_image_get_data(c)[0] == 0xffabcdef);
    REQUIRE(pool.prepare_present() != nullptr);
    pool.did_present();
    // a/b are externally pinned; c is still the layer's contents, even if a
    // pacing tick fired and IOSurfaceIsInUse has not yet been set by the OS.
    for (int i = 0; i < 100; ++i) CHECK(pool.get_back_buffer() == nullptr);
    CHECK(pool.blocked_acquisitions() == 100);
    CHECK(pool.allocation_count() == 3);
    CHECK(pool.live_bytes() == one_frame * 3);
    first_reader.release();
    CHECK(pool.get_back_buffer() == a);
    CHECK(pool.allocation_count() == 3);
    CHECK(pixman_image_get_data(a)[0] == 0xffabcdef);
}

TEST_CASE("Old damage repair copies missed rectangles rather than the whole frame") {
    IOSurfaceFramePool pool;
    pool.resize(64, 64);
    auto* a = pool.get_back_buffer();
    REQUIRE(a != nullptr);
    REQUIRE(pool.prepare_present() != nullptr);
    pool.did_present();
    auto* b = pool.get_back_buffer();
    REQUIRE(b != nullptr);
    CHECK(b != a);
    pixman_image_get_data(b)[0] = 0xff123456;
    gritty::util::PixmanRegion damage(0, 0, 1, 1);
    auto second = pool.prepare_present();
    pool.did_present(&damage);
    Pin hold(second);
    const auto copied = pool.copied_bytes();
    CHECK(pool.get_back_buffer() == a);
    CHECK(pool.copied_bytes() - copied == 4);
    CHECK(pixman_image_get_data(a)[0] == 0xff123456);
    pixman_image_get_data(a)[2] = 0xffabcdef;
    auto first = pool.prepare_present();
    damage.clear(); damage.union_rect(2, 0, 1, 1);
    pool.did_present(&damage);
    Pin old(first);
    auto* c = pool.get_back_buffer(); // Both older frames are pinned.
    REQUIRE(c != nullptr);
    CHECK(c != a);
    CHECK(c != b);
    pixman_image_get_data(c)[4] = 0xff654321;
    REQUIRE(pool.prepare_present() != nullptr);
    damage.clear(); damage.union_rect(4, 0, 1, 1);
    pool.did_present(&damage);
    hold.release();
    const auto before = pool.copied_bytes();
    CHECK(pool.get_back_buffer() == b);
    CHECK(pool.copied_bytes() - before == 8); // Two missed commits, not just the last one.
    CHECK(pixman_image_get_data(b)[0] == 0xff123456);
    CHECK(pixman_image_get_data(b)[2] == 0xffabcdef);
    CHECK(pixman_image_get_data(b)[4] == 0xff654321);
}

TEST_CASE("Resize keeps the old attached surface until commit and external release") {
    IOSurfaceFramePool pool;
    pool.resize(64, 64);
    auto* a = pool.get_back_buffer();
    REQUIRE(a != nullptr);
    pixman_image_get_data(a)[0] = 0xff111111;
    auto first = pool.prepare_present();
    pool.did_present();
    Pin reader(first);
    pool.resize(128, 64);
    CHECK(pool.live_frames() == 2);
    pool.release_spare_buffers();
    CHECK(pool.live_frames() == 2);
    auto* b = pool.get_back_buffer();
    REQUIRE(b != nullptr);
    pixman_image_get_data(b)[0] = 0xff222222;
    REQUIRE(pool.prepare_present() != nullptr);
    pool.did_present();
    pool.release_spare_buffers();
    CHECK(pool.live_frames() == 2);
    CHECK(pixman_image_get_data(a)[0] == 0xff111111);
    reader.release();
    pool.release_spare_buffers();
    CHECK(pool.live_frames() == 1);
    CHECK(pool.image().data()[0] == 0xff222222);
}

TEST_CASE("Damage repair precedes scroll moves and matches a fresh full redraw") {
    const auto font_path = test_font_path();
    if (font_path.empty()) { WARN("No test font available"); return; }
    gritty::render::FontEngine font;
    REQUIRE(font.load_font(font_path, 13.0, 96));
    gritty::core::TerminalEngine engine(32, 12, 16);
    gritty::core::VtParser parser(engine);
    gritty::render::CpuRenderer renderer(font), reference(font);
    renderer.set_scroll_mode(gritty::render::CpuRenderer::ScrollMode::AlwaysMove);
    const auto& metrics = font.metrics();
    const int width = engine.cols() * metrics.cell_width, height = engine.rows() * metrics.cell_height;
    IOSurfaceFramePool pool;
    pool.resize(width, height);
    gritty::util::PixmanImage expected(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, nullptr, 0));
    REQUIRE(expected.valid());
    for (int frame = 0; frame < 40; ++frame) {
        parser.feed("abcdefghijklm0123456789\r\n");
        if (frame == 17) parser.feed("\x1b[2;10r\x1b[10;1H");
        if (frame == 23) parser.feed("\x1b[r\x1b[12;1H");
        if (frame == 30) parser.feed("\x1b[?1049hAlternate\x1b[?1049l");
        auto* buffer = pool.get_back_buffer();
        REQUIRE(buffer != nullptr);
        gritty::util::PixmanRegion damage;
        renderer.render(engine, buffer, &damage, frame == 0);
        reference.render(engine, expected.get(), nullptr, true);
        equal_pixels(buffer, expected.get());
        REQUIRE(pool.prepare_present() != nullptr);
        pool.did_present(&damage);
    }
    CHECK(pool.allocation_count() == 2);
    CHECK(pool.live_frames() == 2);
    CHECK(renderer.stats().scroll_moves > 0);
    pool.release_spare_buffers();
    CHECK(pool.live_frames() == 1);
}

TEST_CASE("Invalid IOSurface dimensions fail without overflowing allocation math") {
    IOSurfaceFramePool pool;
    pool.resize(0, 0);
    CHECK(pool.get_back_buffer() == nullptr);
    pool.resize(-1, 8);
    CHECK(pool.get_back_buffer() == nullptr);
    pool.resize(std::numeric_limits<int>::max(), 64);
    CHECK(pool.get_back_buffer() == nullptr);
    CHECK(pool.allocation_count() == 0);
    CHECK(pool.live_frames() == 0);
}

TEST_CASE("Three-buffer burst reuses surfaces instead of churning full frames") {
    IOSurfaceFramePool pool;
    pool.resize(96, 64);
    IOSurfaceRef pinned[2]{};
    for (int i = 0; i < 3; ++i) {
        auto* pixels = pool.get_back_buffer();
        REQUIRE(pixels != nullptr);
        pixman_image_get_data(pixels)[0] = 0xff000000u | static_cast<uint32_t>(i + 1);
        auto* frame = pool.prepare_present();
        REQUIRE(frame != nullptr);
        pool.did_present();
        if (i < 2) {
            pinned[i] = frame;
            CFRetain(pinned[i]);
            IOSurfaceIncrementUseCount(pinned[i]); // Pinned readers force reuse of the third.
        }
    }
    CHECK(pool.live_frames() == 3);
    for (auto* frame : pinned) { IOSurfaceDecrementUseCount(frame); CFRelease(frame); }
    const auto allocations = pool.allocation_count();
    CHECK(allocations <= 3);
    for (int i = 0; i < 50; ++i) {
        auto* pixels = pool.get_back_buffer();
        REQUIRE(pixels != nullptr);
        REQUIRE(pool.prepare_present() != nullptr);
        pool.did_present();
    }
    CHECK(pool.allocation_count() == allocations);
}
