// Opt-in real-window btop/resize probe; not an automatic machine-dependent RSS gate.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <mach/mach.h>
#include <cstdio>
#include <cstdlib>
#include "config/ini_loader.hpp"
#include "core/terminal_session.hpp"
#include "platform/macos/cocoa_surface.hpp"
#include "platform/macos/ct_discovery.hpp"
#include "platform/macos/kqueue_loop.hpp"

namespace {
uint64_t physical_footprint() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS
        ? info.phys_footprint : 0;
}
void run_for_twelve_seconds(gritty::platform::macos::KqueueRunLoop& loop) {
    loop.set_timer(12000, [&] { loop.stop(); });
    loop.run();
}
size_t cell_bytes(const gritty::core::ScreenGrid& grid) {
    size_t bytes = 0;
    for (int y = 0; y < grid.rows(); ++y) bytes += grid.row(y).cells.capacity() * sizeof(gritty::core::Cell);
    for (size_t y = 0; y < grid.scrollback_size(); ++y)
        bytes += grid.scrollback_row(y).cells.capacity() * sizeof(gritty::core::Cell);
    return bytes;
}
int empty_window(int width, int height) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    const CGFloat scale = [NSScreen mainScreen].backingScaleFactor;
    auto* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, width / scale, height / scale)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    window.releasedWhenClosed = NO;
    window.title = @"Gritty empty AppKit baseline (no terminal pixels)";
    auto* view = [[NSView alloc] initWithFrame:NSZeroRect];
    [view setLayer:[CALayer layer]];
    [view setWantsLayer:YES];
    view.layer.backgroundColor = NSColor.blackColor.CGColor;
    [window setContentView:view];
    [view release];
    [window makeKeyAndOrderFront:nil];
    gritty::platform::macos::KqueueRunLoop loop;
    run_for_twelve_seconds(loop);
    std::printf("{\"workload\":\"window-only\",\"width\":%d,\"height\":%d,\"scale\":%.1f,\"footprint\":%llu}\n",
        width, height, scale, static_cast<unsigned long long>(physical_footprint()));
    [window close];
    [window release];
    return 0;
}
}

int main(int argc, char** argv) {
    @autoreleasepool {
        const int width = argc > 1 ? std::atoi(argv[1]) : 2940;
        const int height = argc > 2 ? std::atoi(argv[2]) : 1790;
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return 2;
        if (argc > 3 && std::string_view(argv[3]) == "window-only") return empty_window(width, height);
        const bool idle = argc > 3 && std::string_view(argv[3]) == "idle";
        const auto loaded = gritty::config::IniLoader::load_from_file(GRITTY_TEST_SOURCE_DIR "/examples/gritty.ini");
        if (!loaded) return 3;
        const auto& config = **loaded;
        gritty::platform::macos::CoreTextFinder finder;
        const auto font = finder.find_font({.family = config.font.family});
        if (!font) return 4;
        gritty::core::SessionConfig options;
        options.font_path = font->path;
        options.font_size = config.font.size;
        options.font_dpi = config.font.dpi;
        options.antialias = config.font.antialias;
        options.palette = config.colors.palette;
        options.scrollback_lines = config.scrollback.lines;
        options.render_config.default_fg = config.colors.foreground;
        options.render_config.default_bg = config.colors.background_argb();
        options.shell = idle ? "/bin/sh" : "/opt/homebrew/bin/btop";
        if (idle) options.args = {"-c", "printf 'idle memory probe'; sleep 20"};
        auto surface = std::make_unique<gritty::platform::macos::CocoaLayerSurface>(640, 384);
        auto* output = surface.get();
        auto events = std::make_unique<gritty::platform::macos::KqueueRunLoop>();
        auto* loop = events.get();
        bool batching = true;
        for (int i = 3; i < argc; ++i)
            if (std::string_view(argv[i]) == "no-batch") batching = false;
        gritty::core::TerminalSession session(options, std::move(surface), std::move(events), nullptr);
        session.set_glyph_batching(batching);
        if (!session.start()) return 5;
        output->show_window("Gritty presentation memory probe");
        session.render_frame(true);
        NSWindow* window = nil;
        for (NSWindow* candidate in NSApp.windows)
            if ([candidate.title isEqualToString:@"Gritty presentation memory probe"]) window = candidate;
        if (!window) return 6;
        [window setContentSize:NSMakeSize(width / output->scale(), height / output->scale())];
        output->flush_pending_resize();
        CFRunLoopTimerContext context{0, loop, nullptr, nullptr, nullptr};
        auto timer = CFRunLoopTimerCreate(nullptr, CFAbsoluteTimeGetCurrent() + 12, 0, 0, 0,
            [](CFRunLoopTimerRef, void* data) { static_cast<gritty::platform::macos::KqueueRunLoop*>(data)->stop(); }, &context);
        if (!timer) return 7;
        CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
        loop->run();
        CFRunLoopTimerInvalidate(timer);
        CFRelease(timer);
        task_vm_info_data_t info{};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count);
        const auto glyphs = session.font_engine().cache_stats();
        const auto& metrics = session.font_engine().metrics();
        std::printf("{\"workload\":\"%s\",\"font\":\"%s\",\"font_size\":%.2f,\"dpi\":%u,\"scale\":%.1f,"
            "\"cols\":%d,\"rows\":%d,\"cell_width\":%d,\"cell_height\":%d,"
            "\"primary_cell_bytes\":%zu,\"alt_cell_bytes\":%zu,\"history_rows\":%zu,\"loaded_faces\":%zu,\"copied_bytes\":%zu}\n",
            idle ? "idle" : "btop", font->path.c_str(), config.font.size, config.font.dpi, output->scale(),
            session.engine().cols(), session.engine().rows(), metrics.cell_width, metrics.cell_height,
            cell_bytes(session.engine().primary_grid()), cell_bytes(session.engine().alt_grid()),
            session.engine().primary_grid().scrollback_size(), session.font_engine().loaded_face_count(), output->buffer_copied_bytes());
        std::printf("{\"width\":%d,\"height\":%d,\"footprint\":%llu,\"frames\":%zu,\"live_bytes\":%zu,\"peak_bytes\":%zu,\"allocations\":%zu,\"presented\":%zu,\"blocked\":%zu,"
            "\"glyph_entries\":%zu,\"glyph_bitmap_bytes\":%zu,\"glyph_hits\":%zu,\"glyph_misses\":%zu,\"glyph_evictions\":%zu}\n",
            output->pixel_width(), output->pixel_height(), static_cast<unsigned long long>(info.phys_footprint),
            output->buffer_live_frames(), output->buffer_live_bytes(), output->buffer_peak_bytes(), output->buffer_allocation_count(),
            output->presented_frame_count(), output->buffer_blocked_acquisitions(),
            glyphs.entries, glyphs.bitmap_bytes, glyphs.hits, glyphs.misses, glyphs.evictions);
        std::fflush(stdout);
    } // Session, pool, NSWindow and callback autorelease pools are now destroyed.
    {
        gritty::platform::macos::KqueueRunLoop events;
        events.set_timer(2200, [&] { events.stop(); });
        events.run(); // Let the compositor release detached surfaces before the close sample.
    }
    mach_task_basic_info_data_t basic{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&basic), &count);
    std::fprintf(stderr, "{\"phase\":\"closed\",\"rss_bytes\":%llu,\"footprint\":%llu}\n",
        static_cast<unsigned long long>(basic.resident_size), static_cast<unsigned long long>(physical_footprint()));
}
