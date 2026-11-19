#include <doctest/doctest.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <IOSurface/IOSurface.h>
#include <chrono>
#include <cstring>
#include <memory>
#include <type_traits>
#include <mach/mach.h>
#include "platform/macos/cocoa_surface.hpp"
#include "platform/macos/cocoa_view.hpp"
#include "platform/macos/kqueue_loop.hpp"
#include "platform/macos/application_icon.hpp"

namespace {
using Surface = gritty::platform::macos::CocoaLayerSurface;
NSWindow* find_window(NSString* title) {
    for (NSWindow* window in NSApp.windows)
        if ([window.title isEqualToString:title]) return window;
    return nil;
}
void pump(double seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        @autoreleasepool {
            [NSApp updateWindows];
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, false);
        }
    }
}
template<class Predicate> bool until(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!predicate() && std::chrono::steady_clock::now() < end) pump(0.002);
    return predicate();
}
IOSurfaceRef contents(NSWindow* window) {
    auto surface = (IOSurfaceRef)window.contentView.layer.contents;
    REQUIRE(surface != nullptr);
    REQUIRE(CFGetTypeID(surface) == IOSurfaceGetTypeID());
    return surface;
}
class SurfacePin final {
    IOSurfaceRef surface_;
public:
    explicit SurfacePin(IOSurfaceRef surface) : surface_(surface) {
        CFRetain(surface_);
        IOSurfaceIncrementUseCount(surface_); // Explicit external reader, not merely a CF object reference.
    }
    SurfacePin(const SurfacePin&) = delete;
    SurfacePin& operator=(const SurfacePin&) = delete;
    ~SurfacePin() { reset(); }
    void reset() {
        if (!surface_) return;
        IOSurfaceDecrementUseCount(surface_);
        CFRelease(surface_);
        surface_ = nullptr;
    }
    IOSurfaceRef get() const { return surface_; }
};
uint32_t first_pixel(IOSurfaceRef surface) {
    REQUIRE(surface != nullptr);
    REQUIRE(IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr) == kIOReturnSuccess);
    uint32_t pixel = 0;
    std::memcpy(&pixel, IOSurfaceGetBaseAddress(surface), sizeof(pixel));
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
    return pixel;
}
pixman_image_t* acquire(Surface& surface) {
    pixman_image_t* pixels = nullptr;
    until([&] { pixels = surface.get_back_buffer(); return pixels != nullptr; });
    return pixels;
}
uint64_t footprint() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS
        ? info.phys_footprint : 0;
}
void white_frames(Surface& surface) {
    surface.resize(1536, 1024, surface.scale());
    gritty::util::PixmanRegion damage;
    damage.union_rect(0, 0, 1536, 1024);
    for (int i = 0; i < 8; ++i) {
        auto* buffer = acquire(surface);
        REQUIRE(buffer != nullptr);
        std::memset(pixman_image_get_data(buffer), 0xff,
            static_cast<size_t>(pixman_image_get_stride(buffer)) * 1024);
        surface.present(damage);
    }
}
}

TEST_CASE("Runtime Dock icons decode only the requested Retina representation") {
    using gritty::platform::macos::copy_application_icon;
    using Image = std::unique_ptr<std::remove_pointer_t<CGImageRef>, decltype(&CGImageRelease)>;
    for (const char* path : {GRITTY_TEST_SOURCE_DIR "/resources/gritty.icns",
                             GRITTY_TEST_SOURCE_DIR "/resources/gritty.png"}) {
        CAPTURE(path);
        Image icon(copy_application_icon(path), CGImageRelease);
        REQUIRE(icon != nullptr);
        CHECK(CGImageGetWidth(icon.get()) == 256);
        CHECK(CGImageGetHeight(icon.get()) == 256);
        CHECK(CGImageGetAlphaInfo(icon.get()) != kCGImageAlphaNone);
        Image small(copy_application_icon(path, 32), CGImageRelease);
        REQUIRE(small != nullptr);
        CHECK(CGImageGetWidth(small.get()) == 32);
        CHECK(CGImageGetHeight(small.get()) == 32);
    }
    CHECK(copy_application_icon(nullptr) == nullptr);
    CHECK(copy_application_icon("") == nullptr);
    CHECK(copy_application_icon("/nonexistent/gritty-icon.icns") == nullptr);
    CHECK(copy_application_icon(GRITTY_TEST_SOURCE_DIR "/resources/gritty.icns", 0) == nullptr);
}

TEST_CASE("Cocoa idle timer releases spare CPU mappings without changing displayed pixels") {
    @autoreleasepool {
        Surface surface(128, 96);
        surface.show_window("Gritty idle memory regression");
        surface.flush_pending_resize();
        NSWindow* window = find_window(@"Gritty idle memory regression");
        REQUIRE(window != nil);
        white_frames(surface);
        pump(0.2);
        const auto active_bytes = surface.buffer_live_bytes();
        const auto active_footprint = footprint();
        CHECK(surface.buffer_live_frames() >= 2); // Attached front and released/in-use backs.
        CHECK(surface.buffer_live_frames() <= 3);
        pump(2.3);
        const auto idle_bytes = surface.buffer_live_bytes();
        std::printf("IDLE_MEMORY active_cpu=%.1f idle_cpu=%.1f active_footprint=%.1f idle_footprint=%.1f MiB\n",
            active_bytes / 1048576., idle_bytes / 1048576., active_footprint / 1048576., footprint() / 1048576.);
        CHECK(surface.buffer_live_frames() == 1);
        CHECK(idle_bytes == 1536 * 1024 * 4);
        CHECK(idle_bytes < active_bytes);
        CHECK(first_pixel(contents(window)) == 0xffffffff);
        auto* writable = acquire(surface);
        REQUIRE(writable != nullptr);
        CHECK(pixman_image_get_data(writable)[0] == 0xffffffff);
        gritty::util::PixmanRegion damage;
        damage.union_rect(0, 0, 1, 1);
        surface.present(damage); // Pending trim timer must be cancelled at destruction.
    }
    pump(2.2);
}

TEST_CASE("KqueueRunLoop with NSApp dispatches IME text and window close") {
    @autoreleasepool {
        Surface surface(320, 240);
        surface.show_window("Gritty NSApp event regression");
        surface.flush_pending_resize();
        NSWindow* window = find_window(@"Gritty NSApp event regression");
        REQUIRE(window != nil);

        std::string committed;
        surface.set_text_commit_callback([&](std::string_view text) { committed.assign(text); });
        NSEvent* key = [NSEvent keyEventWithType:NSEventTypeKeyDown
            location:NSZeroPoint
            modifierFlags:0
            timestamp:0
            windowNumber:[window windowNumber]
            context:nil
            characters:@"A"
            charactersIgnoringModifiers:@"A"
            isARepeat:NO
            keyCode:0];
        REQUIRE(key != nullptr);
        [NSApp postEvent:key atStart:YES];

        gritty::platform::macos::KqueueRunLoop events;
        bool closed = false;
        surface.set_close_callback([&] {
            closed = true;
            events.stop();
        });
        events.set_timer(100, [&] { [window close]; });
        events.run();
        CHECK(committed == "A");
        CHECK(closed); // Window close must actually stop the run loop.
    }
}

TEST_CASE("Cocoa Command keys paste, swallow bare characters and close gracefully") {
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSPasteboard* board = [NSPasteboard generalPasteboard];
        NSString* previous = [board stringForType:NSPasteboardTypeString];
        [board clearContents];
        REQUIRE([board setString:@"native-paste" forType:NSPasteboardTypeString]);

        Surface surface(320, 240);
        surface.show_window("Gritty command key regression");
        surface.flush_pending_resize();
        NSWindow* window = find_window(@"Gritty command key regression");
        REQUIRE(window != nil);
        std::string pasted;
        int keys = 0;
        surface.set_paste_callback([&](std::string_view text) { pasted.assign(text); });
        surface.set_key_callback([&](const gritty::input::KeyEvent&) { ++keys; });
        auto key = [&](NSString* chars, NSString* unmodified, NSEventModifierFlags flags, unsigned short code) {
            return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:flags
                timestamp:0 windowNumber:[window windowNumber] context:nil characters:chars
                charactersIgnoringModifiers:unmodified isARepeat:NO keyCode:code];
        };

        gritty::platform::macos::KqueueRunLoop events;
        bool closed = false;
        surface.set_close_callback([&] {
            closed = true;
            events.stop();
        });
        [NSApp postEvent:key(@"v", @"v", NSEventModifierFlagCommand | NSEventModifierFlagCapsLock, 9)
                 atStart:YES];
        [NSApp postEvent:key(@"x", @"x", NSEventModifierFlagCommand, 7) atStart:YES];
        events.set_timer(gritty::platform::IEventLoop::TimerId::FrameCoalesce, 100, [&] {
            [NSApp postEvent:key(@"q", @"q", NSEventModifierFlagCommand, 12) atStart:YES];
        });
        events.set_timer(gritty::platform::IEventLoop::TimerId::SyncWatchdog, 1500, [&] {
            events.stop();
        });
        events.run();
        CHECK(pasted == "native-paste");
        CHECK(keys == 0); // Cmd+X must not produce a bare 'x'.
        CHECK(closed);   // Cmd+Q uses the close-button path, not NSApp terminate.

        [board clearContents];
        if (previous) [board setString:previous forType:NSPasteboardTypeString];
    }
}

TEST_CASE("Idle reclamation keeps polling a detached surface until its external reader releases it") {
    @autoreleasepool {
        Surface surface(128, 96);
        surface.show_window("Gritty deferred idle regression");
        surface.flush_pending_resize();
        NSWindow* window = find_window(@"Gritty deferred idle regression");
        REQUIRE(window != nil);
        auto* first = acquire(surface);
        REQUIRE(first != nullptr);
        pixman_image_get_data(first)[0] = 0xff123456;
        gritty::util::PixmanRegion damage(0, 0, 1, 1);
        surface.present(damage);
        SurfacePin reader(contents(window));
        surface.resize(256, 192, surface.scale());
        REQUIRE(acquire(surface) != nullptr);
        surface.present(damage);
        pump(2.2); // First trim cannot remove the pinned old geometry.
        CHECK(surface.buffer_live_frames() == 2);
        CHECK(first_pixel(reader.get()) == 0xff123456);
        reader.reset();
        pump(2.2); // No new rendering/request_frame/PTY input to trigger pruning.
        CHECK(surface.buffer_live_frames() == 1);
    }
}

TEST_CASE("Cocoa idle reclamation also fires in the production event loop") {
    @autoreleasepool {
        Surface surface(128, 96);
        surface.show_window("Gritty production idle regression");
        surface.flush_pending_resize();
        NSWindow* window = find_window(@"Gritty production idle regression");
        REQUIRE(window != nil);
        white_frames(surface);
        const auto active = surface.buffer_live_bytes();
        gritty::platform::macos::KqueueRunLoop events;
        events.set_timer(2700, [&] { events.stop(); });
        events.run();
        std::printf("PRODUCTION_IDLE active_cpu=%.1f idle_cpu=%.1f MiB frames=%zu\n",
            active / 1048576., surface.buffer_live_bytes() / 1048576., surface.buffer_live_frames());
        CHECK(surface.buffer_live_frames() == 1);
        CHECK(surface.buffer_live_bytes() == 1536 * 1024 * 4);
        CHECK(first_pixel(contents(window)) == 0xffffffff);
    }
}

TEST_CASE("Cocoa transparency is per-pixel and retained frames keep their alpha") {
    @autoreleasepool {
        Surface surface(128, 96);
        surface.set_transparent(true);
        surface.show_window("Gritty alpha regression");
        NSWindow* window = find_window(@"Gritty alpha regression");
        REQUIRE(window != nil);
        NSView* view = window.contentView;
        CHECK_FALSE(window.opaque);
        CHECK(window.backgroundColor.alphaComponent == 0.0);
        CHECK_FALSE(view.opaque);
        CHECK_FALSE(view.layer.opaque);
        CHECK(window.alphaValue == 1.0);
        auto* buffer = acquire(surface);
        REQUIRE(buffer != nullptr);
        pixman_image_get_data(buffer)[0] = 0x80402010;
        gritty::util::PixmanRegion damage;
        damage.union_rect(0, 0, 1, 1);
        surface.present(damage);
        SurfacePin retained(contents(window));
        CHECK(IOSurfaceGetPixelFormat(retained.get()) == 0x42475241);
        CHECK(first_pixel(retained.get()) == 0x80402010);
        buffer = acquire(surface);
        REQUIRE(buffer != nullptr);
        pixman_image_get_data(buffer)[0] = 0x40001020;
        surface.present(damage);
        CHECK(first_pixel(contents(window)) == 0x40001020);
        CHECK(first_pixel(retained.get()) == 0x80402010); // Published pixels are immutable.
        surface.resize(surface.pixel_width() + 8, surface.pixel_height() + 8, surface.scale());
        CHECK(first_pixel(retained.get()) == 0x80402010); // Resize cannot mutate the retained frame.
        auto* fresh = acquire(surface);
        REQUIRE(fresh != nullptr); // The replacement geometry is drawable before the old one is released.
        CHECK(surface.buffer_live_frames() <= 3); // Old readers plus new geometry remain bounded.
        pixman_image_get_data(fresh)[0] = 0x7f00ff00;
        surface.present(damage);
        CHECK(first_pixel(contents(window)) == 0x7f00ff00);
        retained.reset();
        const auto allocations = surface.buffer_allocation_count();
        for (uint32_t frame = 0; frame < 64; ++frame) {
            auto* next = acquire(surface);
            REQUIRE(next != nullptr);
            pixman_image_get_data(next)[0] = 0xff000000 | frame;
            surface.present(damage);
            pump(0.001);
            CHECK(surface.buffer_live_frames() <= 3); // Released fronts are reused; busy ones remain immutable.
        }
        CHECK(surface.buffer_allocation_count() <= allocations + 2); // Warm up the new geometry's backs.
        pump(2.3); // Idle trim reclaims the obsolete surface, exactly one full-size frame remains.
        CHECK(surface.buffer_live_frames() == 1);
        surface.set_transparent(false);
        CHECK(window.opaque);
        CHECK(window.backgroundColor.alphaComponent == 1.0);
        CHECK(view.opaque);
        CHECK(view.layer.opaque);
        CHECK(window.alphaValue == 1.0);
    }
}

TEST_CASE("Native frame requests coalesce and stop ticking while idle") {
    int frames = 0;
    {
        Surface surface(256, 192);
        surface.show_window("Gritty frame clock regression");
        surface.flush_pending_resize();
        gritty::util::PixmanRegion damage;
        damage.union_rect(0, 0, 1, 1);
        surface.set_frame_ready_callback([&] {
            auto* pixels = surface.get_back_buffer();
            if (!pixels) return;
            pixman_image_get_data(pixels)[0] = 0xff123456;
            surface.present(damage);
            ++frames;
        });
        for (int i = 0; i < 1000; ++i) CHECK(surface.request_frame());
        CHECK(frames == 0);
        REQUIRE(until([&] { return frames == 1; }));
        pump(0.04);
        CHECK(frames == 1);
        surface.on_visibility_changed(false);
        for (int i = 0; i < 1000; ++i) surface.request_frame();
        pump(0.04);
        CHECK(frames == 1);
        surface.on_visibility_changed(true);
        REQUIRE(until([&] { return frames == 2; })); // Restore pending work without a new PTY update.
        surface.request_frame(); // Leave a display-link callback pending at destruction.
    }
    pump(0.1);
    CHECK(frames == 2);
}

TEST_CASE("Presentation retries compositor backpressure without changing published surfaces") {
    @autoreleasepool {
        Surface surface(256, 192);
        surface.show_window("Gritty buffer release regression");
        surface.flush_pending_resize();
        NSWindow* window = find_window(@"Gritty buffer release regression");
        REQUIRE(window != nil);
        gritty::util::PixmanRegion damage;
        damage.union_rect(0, 0, 1, 1);
        uint32_t color = 0xff123456;
        int frames = 0;
        IOSurfaceRef presented = nullptr;
        surface.set_frame_ready_callback([&] {
            auto* pixels = surface.get_back_buffer();
            if (!pixels) return; // The surface arranges the next tick itself.
            pixman_image_get_data(pixels)[0] = color;
            surface.present(damage);
            presented = (IOSurfaceRef)window.contentView.layer.contents;
            ++frames;
        });
        surface.request_frame();
        REQUIRE(until([&] { return frames == 1; }));
        SurfacePin first(contents(window));
        CHECK(first.get() == presented);
        color = 0xff654321;
        surface.request_frame();
        REQUIRE(until([&] { return frames == 2; }));
        CHECK(first_pixel(first.get()) == 0xff123456);
        SurfacePin second(contents(window));
        CHECK(second.get() != first.get());
        color = 0xffabcdef;
        surface.request_frame();
        REQUIRE(until([&] { return frames == 3; }));
        CHECK(first_pixel(contents(window)) == 0xffabcdef);
        CHECK(surface.presented_frame_count() == 3);
        CHECK(surface.buffer_live_frames() == 3);
        color = 0xff112233;
        surface.request_frame();
        pump(0.05); // Frame ticks are not release notifications: all three are busy.
        CHECK(frames == 3);
        CHECK(surface.buffer_blocked_acquisitions() > 0);
        CHECK(first_pixel(first.get()) == 0xff123456);
        CHECK(first_pixel(second.get()) == 0xff654321);
        CHECK(first_pixel(contents(window)) == 0xffabcdef);
        second.reset(); // Resume the already pending frame, without a new request/PTY read.
        REQUIRE(until([&] { return frames == 4; }));
        CHECK(first_pixel(contents(window)) == 0xff112233);
        CHECK(first_pixel(first.get()) == 0xff123456);
        CHECK(surface.buffer_allocation_count() <= 4); // Includes initial window geometry.
    }
}

TEST_CASE("Shared surface orientation and premultiplied alpha survive native layer composition") {
    @autoreleasepool {
        Surface surface(256, 192);
        surface.set_transparent(true);
        surface.show_window("Gritty shared pixel regression");
        surface.flush_pending_resize();
        NSWindow* window = find_window(@"Gritty shared pixel regression");
        REQUIRE(window != nil);
        auto* buffer = acquire(surface);
        REQUIRE(buffer != nullptr);
        const int w = pixman_image_get_width(buffer), h = pixman_image_get_height(buffer);
        const int stride = pixman_image_get_stride(buffer) / 4;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                pixman_image_get_data(buffer)[y * stride + x] = y < h / 2
                    ? (x < w / 2 ? 0x80402010 : 0xff007f00) : (x < w / 2 ? 0xff0000ff : 0xffeeee00);
        gritty::util::PixmanRegion damage;
        damage.union_rect(0, 0, w, h);
        surface.present(damage);
        auto cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        auto context = CGBitmapContextCreate(nullptr, w, h, 8, 0, cs,
            kCGBitmapByteOrder32Host | kCGImageAlphaPremultipliedFirst);
        REQUIRE(context != nullptr);
        CGContextScaleCTM(context, surface.scale(), surface.scale());
        [window.contentView.layer renderInContext:context];
        const auto* pixels = static_cast<const uint32_t*>(CGBitmapContextGetData(context));
        const size_t row = CGBitmapContextGetBytesPerRow(context) / 4;
        CHECK(pixels[0] == 0x80402010);
        CHECK(pixels[w - 1] == 0xff007f00);
        CHECK(pixels[(h - 1) * row] == 0xff0000ff);
        CHECK(pixels[(h - 1) * row + w - 1] == 0xffeeee00);
        CGContextRelease(context);
        CGColorSpaceRelease(cs);
    }
}

TEST_CASE("Cocoa window decorations modes and runtime transitions") {
    @autoreleasepool {
        using gritty::platform::WindowDecorations;
        Surface surface(256, 192);
        surface.show_window("Gritty decorations regression");
        NSWindow* window = find_window(@"Gritty decorations regression");
        REQUIRE(window != nil);
        REQUIRE([window isKindOfClass:[GrittyWindow class]]);
        CHECK([(GrittyWindow*)window canBecomeKeyWindow]);
        CHECK([(GrittyWindow*)window canBecomeMainWindow]);

        surface.set_decorations(WindowDecorations::None);
        pump(0.01);
        CHECK(window.styleMask == NSWindowStyleMaskBorderless);
        CHECK([(GrittyWindow*)window canBecomeKeyWindow]);

        surface.set_decorations(WindowDecorations::Transparent);
        pump(0.01);
        CHECK((window.styleMask & NSWindowStyleMaskFullSizeContentView) != 0);
        CHECK(window.titlebarAppearsTransparent);
        CHECK(window.titleVisibility == NSWindowTitleHidden);
        CHECK([window standardWindowButton:NSWindowCloseButton].hidden == NO);
        CHECK([window standardWindowButton:NSWindowMiniaturizeButton].hidden == NO);
        CHECK([window standardWindowButton:NSWindowZoomButton].hidden == NO);

        surface.set_decorations(WindowDecorations::Buttonless);
        pump(0.01);
        CHECK((window.styleMask & NSWindowStyleMaskFullSizeContentView) != 0);
        CHECK([window standardWindowButton:NSWindowCloseButton].hidden == YES);
        CHECK([window standardWindowButton:NSWindowMiniaturizeButton].hidden == YES);
        CHECK([window standardWindowButton:NSWindowZoomButton].hidden == YES);

        for (const auto mode : {WindowDecorations::Full, WindowDecorations::Transparent,
                                WindowDecorations::Buttonless, WindowDecorations::None,
                                WindowDecorations::Full}) {
            surface.set_decorations(mode);
            pump(0.01);
            CHECK([window.title isEqualToString:@"Gritty decorations regression"]);
            CHECK([window.firstResponder isKindOfClass:[GrittyView class]]);
            CHECK(surface.decorations() == mode);
        }
    }
}

TEST_CASE("Cocoa initial window decorations use the configured style mask") {
    @autoreleasepool {
        using gritty::platform::WindowDecorations;
        Surface surface(256, 192);
        surface.set_decorations(WindowDecorations::Buttonless);
        surface.show_window("Gritty initial decorations regression");
        NSWindow* window = find_window(@"Gritty initial decorations regression");
        REQUIRE(window != nil);
        CHECK((window.styleMask & NSWindowStyleMaskFullSizeContentView) != 0);
        CHECK(window.titlebarAppearsTransparent);
        CHECK([window standardWindowButton:NSWindowCloseButton].hidden == YES);
    }
}
