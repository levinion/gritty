// Opt-in real-window latency / throughput probe. Not part of the automatic
// gate: it measures this machine's PTY, compositor and frame clock, and it
// needs a GUI session. Run it directly:
//
//   gritty_perf_probe latency
//   gritty_perf_probe throughput
//
// Compare modes inside one process so machine state cannot differ between the
// two halves of an A/B measurement.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <IOSurface/IOSurface.h>
#include <mach/mach_time.h>
#include <sys/resource.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "config/ini_loader.hpp"
#include "core/terminal_session.hpp"
#include "platform/macos/cocoa_surface.hpp"
#include "platform/macos/ct_discovery.hpp"
#include "platform/macos/kqueue_loop.hpp"

namespace {
using gritty::core::SessionConfig;
using gritty::core::TerminalSession;
using gritty::platform::macos::CocoaLayerSurface;
using gritty::platform::macos::CoreTextFinder;
using gritty::platform::macos::KqueueRunLoop;

double now_ms() {
    static mach_timebase_info_data_t info{};
    if (info.denom == 0) mach_timebase_info(&info);
    return static_cast<double>(mach_absolute_time()) * info.numer / info.denom / 1.0e6;
}

double thread_cpu_ms() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

double cpu_ms() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    auto ms = [](const timeval& tv) { return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0; };
    return ms(usage.ru_utime) + ms(usage.ru_stime);
}

// KqueueRunLoop has no CFRunLoop source: fd callbacks only run when the probe
// polls it, exactly like the real app's loop.
void pump(double seconds, KqueueRunLoop* events = nullptr) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        @autoreleasepool {
            if (events) events->poll_once(0);
            [NSApp updateWindows];
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0005, false);
        }
    }
}

struct Harness {
    std::unique_ptr<CocoaLayerSurface> surface;
    std::unique_ptr<KqueueRunLoop> events;
    std::unique_ptr<TerminalSession> session;
    KqueueRunLoop* pump_events{nullptr};

    bool start(const SessionConfig& options, const char* title) {
        auto config = gritty::config::IniLoader::load_from_file(GRITTY_TEST_SOURCE_DIR "/examples/gritty.ini");
        if (!config) return false;
        const auto& cfg = **config;
        CoreTextFinder finder;
        const auto font = finder.find_font({.family = cfg.font.family});
        if (!font) return false;

        SessionConfig opts = options;
        opts.font_path = font->path;
        opts.font_size = cfg.font.size;
        opts.font_dpi = cfg.font.dpi;
        opts.antialias = cfg.font.antialias;
        opts.palette = cfg.colors.palette;
        opts.scrollback_lines = cfg.scrollback.lines;
        opts.render_config.default_fg = cfg.colors.foreground;
        opts.render_config.default_bg = cfg.colors.background_argb();

        auto new_surface = std::make_unique<CocoaLayerSurface>(880, 640);
        auto* raw_surface = new_surface.get();
        auto new_events = std::make_unique<KqueueRunLoop>();
        pump_events = new_events.get();
        session = std::make_unique<TerminalSession>(opts, std::move(new_surface), std::move(new_events), nullptr);
        if (!session->start()) return false;
        raw_surface->show_window(title);
        session->render_frame(true);
        return true;
    }

    [[nodiscard]] size_t frames() const {
        return static_cast<CocoaLayerSurface&>(session->surface()).presented_frame_count();
    }

    void tick(double seconds) { pump(seconds, pump_events); }

    void close() {
        session.reset();
        pump(0.05);
    }
};

struct Latencies {
    std::vector<double> samples;
    [[nodiscard]] double median() const {
        if (samples.empty()) return 0;
        auto copy = samples;
        std::sort(copy.begin(), copy.end());
        return copy[copy.size() / 2];
    }
    [[nodiscard]] double p95() const {
        if (samples.empty()) return 0;
        auto copy = samples;
        std::sort(copy.begin(), copy.end());
        return copy[std::min(copy.size() - 1, (copy.size() * 95) / 100)];
    }
};

Latencies measure_latency(bool coalesce, int iterations) {
    Latencies result;
    Harness harness;
    SessionConfig options;
    options.cols = 80;
    options.rows = 24;
    options.shell = "/bin/sh";
    options.args = {"-c", "while IFS= read -r l; do printf '%s\\n' \"$l\"; done"};
    if (!harness.start(options, coalesce ? "perf probe latency coalesced" : "perf probe latency legacy")) {
        std::printf("{\"error\":\"session start failed\"}\n");
        return result;
    }
    auto* surface = static_cast<CocoaLayerSurface*>(&harness.session->surface());
    harness.session->set_frame_coalescing(coalesce);
    harness.tick(0.6); // Settle the first frame and the shell prompt.

    for (int i = 0; i < iterations; ++i) {
        // Settle first: the one-shot frame clock must have fired, otherwise the
        // sample would measure a commit that was already pending.
        harness.tick(0.06);
        const size_t before = harness.frames();
        const double t0 = now_ms();
        harness.session->write_pty("x\n"); // Canonical mode: cat sees it after the newline.
        while (now_ms() - t0 < 200 && harness.frames() == before) harness.tick(0.0005);
        if (harness.frames() != before) result.samples.push_back(now_ms() - t0);
        harness.tick(0.01);
    }
    (void)surface;
    std::printf(
        "{\"mode\":\"latency\",\"coalesce\":%d,\"samples\":%zu,\"median_ms\":%.3f,\"p95_ms\":%.3f,\"max_ms\":%.3f}\n",
        coalesce ? 1 : 0, result.samples.size(), result.median(), result.p95(),
        result.samples.empty() ? 0.0 : *std::max_element(result.samples.begin(), result.samples.end()));
    harness.close();
    return result;
}

const char* policy_name(gritty::render::CpuRenderer::ScrollMode mode) {
    switch (mode) {
        case gritty::render::CpuRenderer::ScrollMode::Adaptive: return "adaptive";
        case gritty::render::CpuRenderer::ScrollMode::AlwaysMove: return "always_move";
        case gritty::render::CpuRenderer::ScrollMode::Repaint: return "repaint";
    }
    return "?";
}

struct ScrollBench {
    double ms_per_frame{0};
    size_t moves{0}, repaints{0};
};

// Isolated renderer benchmark: no PTY, no window, no vsync. Renders one frame
// per scrolled line, which is exactly the cost the memmove optimization targets.
ScrollBench measure_scroll(int density, gritty::render::CpuRenderer::ScrollMode mode, int frames, int lines_per_frame) {
    ScrollBench out;
    auto config = gritty::config::IniLoader::load_from_file(GRITTY_TEST_SOURCE_DIR "/examples/gritty.ini");
    if (!config) return out;
    CoreTextFinder finder;
    const auto font = finder.find_font({.family = (**config).font.family});
    if (!font) return out;

    gritty::render::FontEngine fonts;
    if (!fonts.load_font(font->path, (**config).font.size, (**config).font.dpi)) return out;
    gritty::core::TerminalEngine engine(120, 40);
    gritty::core::VtParser parser(engine);
    gritty::render::CpuRenderer renderer(fonts);
    renderer.set_scroll_mode(mode);

    const int cell_w = fonts.metrics().cell_width, cell_h = fonts.metrics().cell_height;
    gritty::util::PixmanImage target(pixman_image_create_bits(
        PIXMAN_a8r8g8b8, 120 * cell_w, 40 * cell_h, nullptr, 0));
    gritty::util::PixmanRegion damage;

    auto make_line = [&](int i) {
        if (density == 0) return std::to_string(i) + "\r\n";
        std::string text = "line " + std::to_string(i) + " ";
        const int cols = density == 1 ? 118 : 59; // full row, or every other cell
        for (int c = static_cast<int>(text.size()); c < cols; ++c)
            text += (density == 1 || (c % 2) == 0) ? "x" : " ";
        return text + "\r\n";
    };
    for (int i = 0; i < 40; ++i) parser.feed(make_line(i));
    renderer.render(engine, target.get(), &damage, true);

    const double cpu_before = cpu_ms();
    for (int i = 0; i < frames; ++i) {
        for (int n = 0; n < lines_per_frame; ++n) parser.feed(make_line(i * lines_per_frame + n));
        renderer.render(engine, target.get(), &damage);
    }
    out.ms_per_frame = (cpu_ms() - cpu_before) / frames;
    out.moves = renderer.stats().scroll_moves;
    out.repaints = renderer.stats().scroll_repaints;
    std::printf(
        "{\"mode\":\"scroll\",\"density\":%d,\"policy\":\"%s\",\"frames\":%d,\"lines_per_frame\":%d,"
        "\"ms_per_frame\":%.4f,\"moves\":%zu,\"repaints\":%zu}\n",
        density, policy_name(mode), frames, lines_per_frame, out.ms_per_frame, out.moves, out.repaints);
    return out;
}

// Full-redraw microbenchmark per cell content class. Isolates how much a cell
// costs in the renderer (background fill, glyph composite, procedural fills).
struct CellsBench {
    double ms_per_frame{0};
};

IOSurfaceRef make_perf_surface(int w, int h) {
    const void* keys[] = {kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat};
    int64_t numbers[] = {w, h, 4, 0x42475241};
    CFNumberRef values[4];
    for (int i = 0; i < 4; ++i) values[i] = CFNumberCreate(nullptr, kCFNumberSInt64Type, &numbers[i]);
    auto props = CFDictionaryCreate(nullptr, keys, (const void**)values, 4,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    for (int i = 0; i < 4; ++i) CFRelease(values[i]);
    auto surface = IOSurfaceCreate(props);
    CFRelease(props);
    return surface;
}

CellsBench measure_cells(const std::string& variant, int frames, bool on_iosurface = false, bool batch = true) {
    CellsBench out;
    auto config = gritty::config::IniLoader::load_from_file(GRITTY_TEST_SOURCE_DIR "/examples/gritty.ini");
    if (!config) return out;
    CoreTextFinder finder;
    const auto font = finder.find_font({.family = (**config).font.family});
    if (!font) return out;

    gritty::render::FontEngine fonts;
    if (!fonts.load_font(font->path, (**config).font.size, (**config).font.dpi)) return out;
    gritty::core::TerminalEngine engine(120, 40);
    gritty::render::CpuRenderer renderer(fonts);
    renderer.set_glyph_batching(batch);
    const int cell_w = fonts.metrics().cell_width, cell_h = fonts.metrics().cell_height;
    IOSurfaceRef surface = nullptr;
    gritty::util::PixmanImage target;
    if (on_iosurface) {
        surface = make_perf_surface(120 * cell_w, 40 * cell_h);
        IOSurfaceLock(surface, 0, nullptr);
        target.reset(pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, 120 * cell_w, 40 * cell_h,
            static_cast<uint32_t*>(IOSurfaceGetBaseAddress(surface)), IOSurfaceGetBytesPerRow(surface)));
    } else {
        target.reset(pixman_image_create_bits(PIXMAN_a8r8g8b8, 120 * cell_w, 40 * cell_h, nullptr, 0));
    }
    gritty::util::PixmanRegion damage;

    const char32_t box_chars[] = {0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518,
                                  0x2503, 0x2501, 0x2801, 0x28FF, 0x2588, 0x2591};
    const char32_t text_chars[] = {U'T', U'h', U'e', U'q', U'u', U'i', U'c', U'k', U'0', U'1', U'2', U'3'};
    auto& grid = engine.grid();
    for (int y = 0; y < 40; ++y) {
        for (int x = 0; x < 120; ++x) {
            gritty::core::Cell cell;
            const int i = y * 120 + x;
            if (variant == "spaces") cell.codepoint = U' ';
            else if (variant == "text") cell.codepoint = text_chars[i % 12];
            else if (variant == "box") cell.codepoint = box_chars[i % 12];
            else { // mixed: UI-ish, 40% procedural
                cell.codepoint = (i % 5 < 2) ? box_chars[i % 12] : text_chars[i % 12];
            }
            grid.set_cell(x, y, cell);
        }
    }
    renderer.render(engine, target.get(), &damage, true);
    const double cpu_before = cpu_ms();
    for (int i = 0; i < frames; ++i) renderer.render(engine, target.get(), &damage, true);
    out.ms_per_frame = (cpu_ms() - cpu_before) / frames;
    if (surface) { IOSurfaceUnlock(surface, 0, nullptr); CFRelease(surface); }
    std::printf("{\"mode\":\"cells\",\"variant\":\"%s\",\"iosurface\":%d,\"batch\":%d,\"frames\":%d,\"ms_per_frame\":%.4f}\n",
        variant.c_str(), on_iosurface ? 1 : 0, batch ? 1 : 0, frames, out.ms_per_frame);
    return out;
}

// Isolate layer submission cost: same surface, same damage, no parsing or
// rasterization. Compares an explicit CATransaction flush with foot-macos's
// commit-only handoff.
void measure_present(bool flush, int presents) {
    auto* surface = new CocoaLayerSurface(880, 640);
    surface->set_present_flush(flush);
    surface->show_window(flush ? "perf probe present flush" : "perf probe present commit");
    pump(0.2);
    gritty::util::PixmanRegion damage;
    damage.union_rect(0, 0, 16, 16);
    const double before = cpu_ms();
    for (int i = 0; i < presents; ++i) {
        auto* buffer = surface->get_back_buffer();
        if (!buffer) { pump(0.001); continue; }
        pixman_image_get_data(buffer)[0] = 0xff000000u | static_cast<uint32_t>(i);
        surface->present(damage);
        pump(0.001); // Let the run loop deliver the transaction before the next frame.
    }
    const double elapsed = cpu_ms() - before;
    std::printf("{\"mode\":\"present\",\"flush\":%d,\"presents\":%d,\"cpu_ms\":%.1f,"
        "\"ms_per_present\":%.3f}\n", flush ? 1 : 0, presents, elapsed, elapsed / presents);
    delete surface;
    pump(0.05);
}

// Feed the same bytes the end-to-end benchmark produces, with no rendering and
// no window, to separate parse/terminal-state cost from draw and commit cost.
void measure_parse(int lines, int cols, int rows, int scrollback, size_t chunk) {
    gritty::core::TerminalEngine engine(cols, rows, static_cast<size_t>(scrollback));
    gritty::core::VtParser parser(engine);
    std::string payload;
    for (int i = 0; i < lines; ++i) payload += std::to_string(i) + "\r\n";
    const double before = cpu_ms();
    if (chunk == 0) parser.feed(payload);
    else {
        for (size_t off = 0; off < payload.size(); off += chunk)
            parser.feed(std::string_view(payload).substr(off, chunk));
    }
    const double elapsed = cpu_ms() - before;
    std::printf("{\"mode\":\"parse\",\"lines\":%d,\"cols\":%d,\"rows\":%d,\"scrollback\":%d,"
        "\"chunk\":%zu,\"bytes\":%zu,\"cpu_ms\":%.1f,\"us_per_line\":%.3f}\n",
        lines, cols, rows, scrollback, chunk, payload.size(), elapsed, elapsed * 1000.0 / lines);
}

// Parse inside a fully initialized session (window, renderer, fonts, PTY),
// but without presenting, to separate parse cost from scheduling effects.
void measure_session_parse(int lines) {
    Harness harness;
    SessionConfig options;
    options.cols = 80;
    options.rows = 24;
    options.shell = "/bin/sh";
    options.args = {"-c", "sleep 5"};
    if (!harness.start(options, "perf probe session parse")) return;
    harness.tick(0.3);
    std::string payload;
    for (int i = 0; i < lines; ++i) payload += std::to_string(i) + "\r\n";
    gritty::core::VtParser parser(harness.session->engine());
    const double before = cpu_ms();
    parser.feed(payload);
    const double elapsed = cpu_ms() - before;
    std::printf("{\"mode\":\"session_parse\",\"lines\":%d,\"cpu_ms\":%.1f,\"us_per_line\":%.3f}\n",
        lines, elapsed, elapsed * 1000.0 / lines);
    harness.close();
}

// Same input, batched into frames exactly like a vsync-paced session, but
// drawing into a plain pixman image (no layer, no commit).
void measure_parse_render(int lines, int lines_per_frame) {
    auto config = gritty::config::IniLoader::load_from_file(GRITTY_TEST_SOURCE_DIR "/examples/gritty.ini");
    if (!config) return;
    CoreTextFinder finder;
    const auto font = finder.find_font({.family = (**config).font.family});
    if (!font) return;
    gritty::render::FontEngine fonts;
    if (!fonts.load_font(font->path, (**config).font.size, (**config).font.dpi)) return;
    gritty::core::TerminalEngine engine(120, 40);
    gritty::core::VtParser parser(engine);
    gritty::render::CpuRenderer renderer(fonts);
    const int cell_w = fonts.metrics().cell_width, cell_h = fonts.metrics().cell_height;
    gritty::util::PixmanImage target(pixman_image_create_bits(
        PIXMAN_a8r8g8b8, 120 * cell_w, 40 * cell_h, nullptr, 0));
    gritty::util::PixmanRegion damage;
    renderer.render(engine, target.get(), &damage, true);

    const int frames = lines / lines_per_frame;
    std::string line_text;
    const double before = cpu_ms();
    for (int f = 0; f < frames; ++f) {
        line_text.clear();
        for (int n = 0; n < lines_per_frame; ++n)
            line_text += std::to_string(f * lines_per_frame + n) + "\r\n";
        parser.feed(line_text);
        renderer.render(engine, target.get(), &damage);
    }
    const double elapsed = cpu_ms() - before;
    std::printf(
        "{\"mode\":\"parse_render\",\"lines\":%d,\"frames\":%d,\"lines_per_frame\":%d,"
        "\"cpu_ms\":%.1f,\"ms_per_frame\":%.3f}\n",
        lines, frames, lines_per_frame, elapsed, elapsed / frames);
}

// Split-pane workload: two independent scroll regions shift once per frame.
void measure_split_scroll(int max_ops, int frames) {
    auto config = gritty::config::IniLoader::load_from_file(GRITTY_TEST_SOURCE_DIR "/examples/gritty.ini");
    if (!config) return;
    CoreTextFinder finder;
    const auto font = finder.find_font({.family = (**config).font.family});
    if (!font) return;
    gritty::render::FontEngine fonts;
    if (!fonts.load_font(font->path, (**config).font.size, (**config).font.dpi)) return;
    gritty::core::TerminalEngine engine(120, 40);
    gritty::core::VtParser parser(engine);
    gritty::render::CpuRenderer renderer(fonts);
    engine.grid().set_max_pending_scrolls(static_cast<size_t>(max_ops));
    const int cell_w = fonts.metrics().cell_width, cell_h = fonts.metrics().cell_height;
    gritty::util::PixmanImage target(pixman_image_create_bits(
        PIXMAN_a8r8g8b8, 120 * cell_w, 40 * cell_h, nullptr, 0));
    gritty::util::PixmanRegion damage;
    std::string text;
    for (int i = 0; i < 40; ++i) text += "line " + std::to_string(i) +
        " The quick brown fox jumps over the lazy dog 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n";
    parser.feed(text);
    renderer.render(engine, target.get(), &damage, true);

    gritty::core::Cell blank;
    gritty::core::Cell text_cell;
    text_cell.codepoint = U'x';
    const double before = cpu_ms();
    for (int i = 0; i < frames; ++i) {
        engine.grid().scroll_up(0, 18, 1, blank);
        engine.grid().scroll_up(20, 39, 1, blank);
        // Both panes keep producing text, so their regions stay dense.
        for (int x = 0; x < 100; ++x) {
            engine.grid().set_cell(x, 18, text_cell);
            engine.grid().set_cell(x, 39, text_cell);
        }
        renderer.render(engine, target.get(), &damage);
    }
    const double elapsed = cpu_ms() - before;
    std::printf(
        "{\"mode\":\"split_scroll\",\"max_ops\":%d,\"frames\":%d,\"ms_per_frame\":%.4f,"
        "\"moves\":%zu,\"repaints\":%zu}\n",
        max_ops, frames, elapsed / frames, renderer.stats().scroll_moves, renderer.stats().scroll_repaints);
}

// Per-operation costs behind a rendered cell: glyph cache hit, glyph mask
// composite, grid scroll, cell write. Tells which part of a frame is worth
// attacking next.
void measure_primitives() {
    auto config = gritty::config::IniLoader::load_from_file(GRITTY_TEST_SOURCE_DIR "/examples/gritty.ini");
    if (!config) return;
    CoreTextFinder finder;
    const auto font = finder.find_font({.family = (**config).font.family});
    if (!font) return;
    gritty::render::FontEngine fonts;
    if (!fonts.load_font(font->path, (**config).font.size, (**config).font.dpi)) return;
    const auto cell_w = fonts.metrics().cell_width, cell_h = fonts.metrics().cell_height;

    gritty::util::PixmanImage target(pixman_image_create_bits(PIXMAN_a8r8g8b8, 120 * cell_w, 40 * cell_h, nullptr, 0));
    const int n = 500000;

    // Hot single glyph (hash hit + LRU splice).
    const auto* glyph = fonts.get_glyph(U'M', false, false);
    if (!glyph) return;
    double t0 = cpu_ms();
    volatile size_t sink = 0;
    for (int i = 0; i < n; ++i) sink += reinterpret_cast<size_t>(fonts.get_glyph(U'M', false, false));
    const double hit_ns = (cpu_ms() - t0) * 1e6 / n;

    // Glyph hit across a working set of ASCII letters (cache still warm).
    t0 = cpu_ms();
    for (int i = 0; i < n; ++i) sink += reinterpret_cast<size_t>(fonts.get_glyph(U'A' + (i & 31), false, false));
    const double varied_ns = (cpu_ms() - t0) * 1e6 / n;

    // One glyph composite into the target.
    pixman_color_t white = {0xffff, 0xffff, 0xffff, 0xffff};
    gritty::util::PixmanImage solid(pixman_image_create_solid_fill(&white));
    t0 = cpu_ms();
    for (int i = 0; i < n; ++i) {
        pixman_image_composite32(PIXMAN_OP_OVER, solid.get(), glyph->mask.get(), target.get(),
            0, 0, 0, 0, (i % 100) * cell_w, (i % 30) * cell_h, glyph->width, glyph->height);
    }
    const double composite_ns = (cpu_ms() - t0) * 1e6 / n;

    // Grid scroll and cell write.
    gritty::core::TerminalEngine engine(120, 40);
    gritty::core::Cell blank;
    blank.codepoint = U'x';
    t0 = cpu_ms();
    for (int i = 0; i < n; ++i) engine.grid().scroll_up(0, 39, 1, blank);
    const double scroll_ns = (cpu_ms() - t0) * 1e6 / n;
    t0 = cpu_ms();
    for (int i = 0; i < n; ++i) engine.grid().set_cell(i % 120, (i / 120) % 40, blank);
    const double setcell_ns = (cpu_ms() - t0) * 1e6 / n;

    std::printf(
        "{\"mode\":\"primitives\",\"glyph_hit_ns\":%.2f,\"glyph_varied_ns\":%.2f,"
        "\"glyph_composite_ns\":%.2f,\"grid_scroll_ns\":%.2f,\"set_cell_ns\":%.2f}\n",
        hit_ns, varied_ns, composite_ns, scroll_ns, setcell_ns);

    // Batched glyph compositing through pixman's glyph atlas, versus one
    // composite32 per glyph.
    auto* cache = pixman_glyph_cache_create();
    if (!cache) return;
    const int per_row = 80;
    const void* handles[32]{};
    pixman_glyph_cache_freeze(cache);
    for (int i = 0; i < 32; ++i) {
        const auto* g = fonts.get_glyph(U'A' + i, false, false);
        if (g && g->mask.valid())
            handles[i] = pixman_glyph_cache_insert(cache, (void*)1, (void*)(intptr_t)(i + 1), 0, 0, g->mask.get());
    }
    pixman_glyph_cache_thaw(cache);
    std::vector<pixman_glyph_t> glyphs(per_row);
    for (int i = 0; i < per_row; ++i) {
        glyphs[i] = pixman_glyph_t{i * cell_w, 0, handles[(i + 1) % 32]};
    }
    const auto mask_format = pixman_glyph_get_mask_format(cache, per_row, glyphs.data());
    pixman_box32_t extents{};
    pixman_glyph_get_extents(cache, per_row, glyphs.data(), &extents);
    t0 = cpu_ms();
    for (int i = 0; i < n / per_row; ++i) {
        pixman_composite_glyphs(PIXMAN_OP_OVER, solid.get(), target.get(), mask_format,
            0, 0, 0, 0, 0, 0,
            extents.x2 - extents.x1, extents.y2 - extents.y1, cache, per_row, glyphs.data());
    }
    const double batched_ns = (cpu_ms() - t0) * 1e6 / n;
    t0 = cpu_ms();
    for (int i = 0; i < n; ++i) {
        const auto* g = fonts.get_glyph(U'A' + (i & 31), false, false);
        pixman_image_composite32(PIXMAN_OP_OVER, solid.get(), g->mask.get(), target.get(),
            0, 0, 0, 0, (i % 100) * cell_w, (i % 30) * cell_h, g->width, g->height);
    }
    const double per_glyph_ns = (cpu_ms() - t0) * 1e6 / n;
    std::printf("{\"mode\":\"glyphbatch\",\"per_glyph_ns\":%.2f,\"batched_ns\":%.2f,\"speedup\":%.2f}\n",
        per_glyph_ns, batched_ns, per_glyph_ns / batched_ns);
    pixman_glyph_cache_destroy(cache);
}

struct Throughput {
    double wall_ms{0}, cpu_ms{0};
    size_t frames{0}, scroll_moves{0};
    long long scrolled_rows{0};
};

Throughput measure_throughput(bool scroll_memmove, bool coalesce, int lines, bool full_rows, bool flush, bool batching = true) {
    Throughput out;
    Harness harness;
    SessionConfig options;
    options.cols = 80;
    options.rows = 24;
    options.shell = "/bin/sh";
    // Wait for a "go" line so the measured window only covers the workload,
    // not window creation and the first frame.
    options.args = {"-c", std::string("read -r _; ") + (full_rows
        ? "yes 'The quick brown fox jumps over the lazy dog 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ' | head -n " +
              std::to_string(lines)
        : "seq 1 " + std::to_string(lines))};
    if (!harness.start(options, scroll_memmove ? "perf probe throughput memmove" : "perf probe throughput repaint")) {
        std::printf("{\"error\":\"session start failed\"}\n");
        return out;
    }
    harness.session->set_scroll_optimization(scroll_memmove);
    harness.session->set_glyph_batching(batching);
    harness.session->set_frame_coalescing(coalesce);
    static_cast<CocoaLayerSurface&>(harness.session->surface()).set_present_flush(flush);
    harness.tick(0.3); // Prompt/first frame.

    const size_t frames_before = harness.frames();
    const double cpu_before = cpu_ms();
    const double thread_before = thread_cpu_ms();
    harness.session->write_pty("go\n");
    const double start = now_ms();
    // Run until the child closes the PTY (the session stops on EOF).
    harness.session->run();
    const double wall = now_ms() - start;
    const double cpu = cpu_ms() - cpu_before;
    const double thread_cpu = thread_cpu_ms() - thread_before;
    // Let the final committed frame land.
    harness.tick(0.05);

    const auto& stats = harness.session->render_stats();
    out.wall_ms = wall;
    out.cpu_ms = cpu;
    out.frames = harness.frames() - frames_before;
    out.scroll_moves = stats.scroll_moves;
    out.scrolled_rows = stats.scrolled_rows;
    std::printf(
        "{\"mode\":\"throughput\",\"scroll_memmove\":%d,\"coalesce\":%d,\"full_rows\":%d,\"flush\":%d,"
        "\"batching\":%d,\"lines\":%d,\"wall_ms\":%.1f,\"cpu_ms\":%.1f,\"thread_cpu_ms\":%.1f,"
        "\"frames\":%zu,\"scroll_moves\":%zu,\"scrolled_rows\":%lld}\n",
        scroll_memmove ? 1 : 0, coalesce ? 1 : 0, full_rows ? 1 : 0, flush ? 1 : 0,
        batching ? 1 : 0, lines, out.wall_ms, out.cpu_ms, thread_cpu, out.frames,
        out.scroll_moves, out.scrolled_rows);
    harness.close();
    return out;
}
} // namespace

int main(int argc, char* argv[]) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        const std::string mode = argc > 1 ? argv[1] : "all";
        if (mode == "latency" || mode == "all") {
            measure_latency(false, 30);
            measure_latency(true, 30);
        }
        if (mode == "scroll" || mode == "all") {
            using Mode = gritty::render::CpuRenderer::ScrollMode;
            for (int density : {0, 1, 2})
                for (Mode mode : {Mode::Adaptive, Mode::AlwaysMove, Mode::Repaint}) {
                    measure_scroll(density, mode, 600, 1);
                    measure_scroll(density, mode, 60, 400);
                }
        }
        if (mode == "cells" || mode == "all") {
            for (const char* variant : {"spaces", "text", "mixed"})
                for (bool ios : {false, true})
                    for (bool batch : {false, true}) {
                        measure_cells(variant, 300, ios, batch);
                        measure_cells(variant, 300, ios, batch);
                    }
        }
        if (mode == "present" || mode == "all") {
            for (int i = 0; i < 2; ++i) {
                measure_present(false, 200);
                measure_present(true, 200);
            }
        }
        if (mode == "session_parse") {
            measure_session_parse(200000);
            return 0;
        }
        if (mode == "primitives" || mode == "all") measure_primitives();
        if (mode == "parse" || mode == "all") {
            measure_parse(200000, 80, 24, 2000, 0);
            measure_parse(200000, 80, 24, 2000, 50);
            measure_parse(200000, 80, 24, 2000, 10);
            measure_parse(200000, 80, 24, 2000, 16384);
            measure_parse(200000, 80, 24, 2000, 2800);
            measure_parse(200000, 80, 24, 0, 0);
            measure_parse(200000, 120, 40, 2000, 0);
            measure_parse_render(200000, 400);
            measure_parse_render(200000, 1);
        }
        if (mode == "split" || mode == "all") {
            for (int i = 0; i < 2; ++i) {
                measure_split_scroll(1, 600); // Old single-region behaviour: repaint both.
                measure_split_scroll(8, 600); // Queue both shifts, apply both.
            }
        }
        if (mode == "throughput" || mode == "all") {
            constexpr int lines = 200000;
            for (bool full : {false, true}) {
                measure_throughput(true, true, lines, full, true, false);
                measure_throughput(true, true, lines, full, true, true);
            }
        }
    }
    return 0;
}
