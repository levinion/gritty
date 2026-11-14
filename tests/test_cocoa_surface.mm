#include <doctest/doctest.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <cstring>
#include <memory>
#include <type_traits>
#include "platform/macos/cocoa_surface.hpp"

namespace {
NSWindow* find_window(NSString* title) {
    for (NSWindow* window in NSApp.windows)
        if ([window.title isEqualToString:title]) return window;
    return nil;
}

uint32_t first_pixel(CGImageRef image) {
    REQUIRE(image != nullptr);
    std::unique_ptr<const void, decltype(&CFRelease)> bytes(
        CGDataProviderCopyData(CGImageGetDataProvider(image)), CFRelease);
    REQUIRE(bytes != nullptr);
    const auto data = static_cast<CFDataRef>(bytes.get());
    REQUIRE(CFDataGetLength(data) >= 4);
    uint32_t pixel = 0;
    std::memcpy(&pixel, CFDataGetBytePtr(data), sizeof(pixel));
    return pixel;
}
}

// Opt-in: creates an actual NSWindow; do not run this suite on headless CI.
TEST_CASE("Cocoa transparency is per-pixel and retained frames keep their alpha") {
    @autoreleasepool {
        using gritty::platform::macos::CocoaLayerSurface;
        CocoaLayerSurface surface(128, 96);
        surface.set_transparent(true);
        surface.show_window("Gritty alpha regression");
        NSWindow* window = find_window(@"Gritty alpha regression");
        REQUIRE(window != nil);
        NSView* view = window.contentView;
        CHECK_FALSE(window.opaque);
        CHECK(window.backgroundColor.alphaComponent == 0.0);
        CHECK_FALSE(view.opaque);
        CHECK_FALSE(view.layer.opaque);
        CHECK(window.alphaValue == 1.0); // Do not fade the glyphs with window-wide opacity.

        auto* buffer = surface.get_back_buffer();
        REQUIRE(buffer != nullptr);
        pixman_image_get_data(buffer)[0] = 0x80402010;
        gritty::util::PixmanRegion damage;
        damage.union_rect(0, 0, 1, 1);
        surface.present(damage);
        CGImageRef image = (CGImageRef)view.layer.contents;
        REQUIRE(image != nullptr);
        using ImageLease = std::unique_ptr<std::remove_pointer_t<CGImageRef>, decltype(&CGImageRelease)>;
        ImageLease retained(CGImageRetain(image), CGImageRelease);
        CHECK(CGImageGetAlphaInfo(image) == kCGImageAlphaPremultipliedFirst);
        CHECK(first_pixel(image) == 0x80402010);

        // Editing the next frame cannot alter a CGImage still held by CALayer.
        buffer = surface.get_back_buffer();
        REQUIRE(buffer != nullptr);
        pixman_image_get_data(buffer)[0] = 0x40001020;
        surface.present(damage);
        CHECK(first_pixel((CGImageRef)view.layer.contents) == 0x40001020);
        CHECK(first_pixel(retained.get()) == 0x80402010);
        surface.resize(surface.pixel_width() + 8, surface.pixel_height() + 8, surface.scale());
        CHECK(first_pixel(retained.get()) == 0x80402010);

        surface.set_transparent(false);
        CHECK(window.opaque);
        CHECK(window.backgroundColor.alphaComponent == 1.0);
        CHECK(view.opaque);
        CHECK(view.layer.opaque);
        CHECK(window.alphaValue == 1.0);
    }
}
