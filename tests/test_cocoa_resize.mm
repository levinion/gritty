#include <doctest/doctest.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <mach/mach.h>
#include <algorithm>
#include "config/toml_loader.hpp"
#include "core/terminal_session.hpp"
#include "platform/macos/cocoa_surface.hpp"
#include "platform/macos/ct_discovery.hpp"
#include "platform/macos/kqueue_loop.hpp"

namespace {
uint64_t footprint() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS
        ? info.phys_footprint : 0;
}
}

TEST_CASE("Resize storm keeps native example-config memory bounded") {
    @autoreleasepool {
        using namespace gritty;
        const auto loaded = config::TomlLoader::load_from_file(GRITTY_TEST_SOURCE_DIR "/examples/config.toml");
        REQUIRE(loaded.has_value());
        const auto& cfg = **loaded;
        platform::macos::CoreTextFinder discovery;
        const auto font = discovery.find_font({.family = cfg.font.family});
        REQUIRE(font.has_value());
        core::SessionConfig options;
        options.font_path = font->path;
        options.font_size = cfg.font.size;
        options.font_dpi = cfg.font.dpi;
        options.antialias = cfg.font.antialias;
        options.palette = cfg.colors.palette;
        options.render_config.default_fg = cfg.colors.foreground;
        options.render_config.default_bg = cfg.colors.background_argb();
        options.shell = "/bin/sh";
        options.args = {"-c", "printf resize; read line"};
        auto surface = std::make_unique<platform::macos::CocoaLayerSurface>(640, 384);
        auto* output = surface.get();
        auto loop = std::make_unique<platform::macos::KqueueRunLoop>();
        auto* events = loop.get();
        core::TerminalSession session(options, std::move(surface), std::move(loop), nullptr);
        REQUIRE(session.start());
        output->show_window("Gritty resize memory regression");
        session.render_frame(true);
        NSWindow* window = nil;
        for (NSWindow* candidate in NSApp.windows)
            if ([candidate.title isEqualToString:@"Gritty resize memory regression"]) window = candidate;
        REQUIRE(window != nil);
        output->flush_pending_resize();
        for (int i = 0; i < 5; ++i) {
            @autoreleasepool {
                events->poll_once(0);
                [NSApp updateWindows];
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
            }
        }
        const NSSize original = window.contentView.bounds.size;
        const int original_cols = session.engine().cols(), original_rows = session.engine().rows();
        const auto allocations_before = output->buffer_allocation_count();
        const auto initial = footprint();
        auto peak = initial;
        const int steps = 2000;
        @autoreleasepool { // One native mouse-tracking gesture, not one pool per pixel.
            [window.contentView viewWillStartLiveResize];
            for (int i = 0; i < steps; ++i) {
                const int phase = i % 80;
                const int ramp = phase < 40 ? phase : 80 - phase;
                [window setContentSize:NSMakeSize(560 + ramp * 16 + (i % 3) * 0.5, 350 + ramp * 10 + (i % 5) * 0.5)];
                events->poll_once(0);
                [NSApp updateWindows];
                CFRunLoopRunInMode((CFStringRef)NSEventTrackingRunLoopMode, 0.001, false);
                peak = std::max(peak, footprint());
                if (peak > 512ull * 1024 * 1024) break; // Do not exhaust the developer's machine.
            }
            CFRunLoopRunInMode((CFStringRef)NSEventTrackingRunLoopMode, 0.02, false);
            CHECK(output->buffer_allocation_count() > allocations_before); // Must update during tracking, not only on mouse-up.
            [window setContentSize:original];
            [window.contentView viewDidEndLiveResize];
        }
        CHECK(session.engine().cols() == original_cols);
        CHECK(session.engine().rows() == original_rows);
        CHECK(NSEqualSizes(window.contentView.layer.bounds.size, window.contentView.bounds.size));
        for (int i = 0; i < 10; ++i) {
            @autoreleasepool {
                [NSApp updateWindows];
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
            }
        }
        const auto after = footprint();
        std::printf("RESIZE_MEMORY initial=%.1f peak=%.1f after=%.1f MiB buffers=%zu live=%zu peak_buffers=%.1f MiB\n",
            initial / 1048576., peak / 1048576., after / 1048576., output->buffer_allocation_count(),
            output->buffer_live_frames(), output->buffer_peak_bytes() / 1048576.);
        CHECK(peak < initial + 128ull * 1024 * 1024);
        CHECK(after < initial + 96ull * 1024 * 1024);
    }
}

TEST_CASE("Queued native resizes allocate only the latest geometry and cancel safely") {
    @autoreleasepool {
        using gritty::platform::macos::CocoaLayerSurface;
        {
            CocoaLayerSurface surface(128, 96);
            surface.show_window("Gritty coalesced resize regression");
            surface.flush_pending_resize();
            int callbacks = 0, final_width = 0, final_height = 0;
            surface.set_resize_callback([&](int width, int height, double) {
                ++callbacks; final_width = width; final_height = height;
            });
            const auto allocated = surface.buffer_allocation_count();
            for (int i = 0; i < 500; ++i) surface.on_view_resize(640 + i, 384 + i % 37, 2.0);
            CHECK(callbacks == 0);
            CHECK(surface.buffer_allocation_count() == allocated);
            surface.on_live_resize(false);
            CHECK(callbacks == 1);
            CHECK(final_width == 1139);
            CHECK(final_height == 384 + 499 % 37);
            CHECK(surface.buffer_allocation_count() == allocated + 1);
            surface.on_view_resize(1280, 720, 2.0); // Leave one pending at destruction.
        }
        CFRunLoopRunInMode((CFStringRef)NSEventTrackingRunLoopMode, 0.03, false);
    }
}
