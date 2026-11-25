#include <doctest/doctest.h>
#include "font_fixture.hpp"
#include <chrono>
#include <cstdio>
#include <thread>
#include <poll.h>
#include "core/terminal_session.hpp"
#include "config/toml_loader.hpp"
#include <unistd.h>

namespace {
using namespace gritty;

std::string session_test_font() { return test_font_path(); }
class TestSurface final : public platform::IWindowSurface {
public:
    void resize(int w, int h, double s) override {
        width = w;
        height = h;
        scaling = s;
        pixels.reset(pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, nullptr, 0));
    }
    pixman_image_t* get_back_buffer() override { return blocked ? nullptr : pixels.get(); }
    void set_frame_ready_callback(FrameReadyCallback callback) override { frame_ready = std::move(callback); }
    void set_paste_callback(PasteCallback callback) override { paste = std::move(callback); }
    void paste_text(std::string_view text) { if (paste) paste(text); }
    FrameReadyCallback frame_ready;
    PasteCallback paste;
    bool blocked{false};
    void present(const util::PixmanRegion& damage) override {
        ++frames;
        last_damage = damage.extents();
    }
    void set_title(const std::string& value) override { title = value; }
    void open_url(std::string_view url) override { opened_url.assign(url); }
    std::string opened_url;
    void set_transparent(bool value) override { transparent = value; }
    bool transparent{false};
    bool pacing{false};
    int pixel_width() const noexcept override { return width; }
    int pixel_height() const noexcept override { return height; }
    double scale() const noexcept override { return scaling; }
    bool supports_frame_pacing() const noexcept override { return pacing; }
    util::PixmanImage pixels;
    int width{0}, height{0}, frames{0};
    double scaling{1.0};
    pixman_box32_t last_damage{};
    std::string title;
};

class TestLoop final : public platform::IEventLoop {
public:
    void add_fd(int value, uint32_t mask, FdCallback callback) override {
        fd = value;
        watched = mask;
        cb = std::move(callback);
    }
    void remove_fd(int) override { cb = {}; ++remove_count; }
    void set_timer(int delay, std::function<void()> callback) override {
        timer_delay = delay;
        timer = std::move(callback);
    }
    int timer_delay{0};
    std::function<void()> timer;
    void run() override {}
    void stop() override { stopped = true; }
    void wake() override {}
    void readable() { auto copy = cb; if (copy) copy(fd, EventReadable); }
    void pump() {
        pollfd item{fd, static_cast<short>(POLLIN | ((watched & EventWritable) ? POLLOUT : 0)), 0};
        if (::poll(&item, 1, 10) <= 0) return;
        uint32_t flags = 0;
        if (item.revents & (POLLIN | POLLHUP | POLLERR)) flags |= EventReadable;
        if (item.revents & POLLOUT) flags |= EventWritable;
        auto copy = cb;
        if (copy) copy(fd, flags);
    }
    uint32_t watched{0};
    FdCallback cb;
    int fd{-1};
    int remove_count{0};
    bool stopped{false};
};
}

TEST_CASE("Session preserves full redraw and latest content across compositor backpressure") {
    auto surface = std::make_unique<TestSurface>();
    auto* output = surface.get();
    core::SessionConfig config;
    config.font_path = session_test_font();
    config.cols = 8;
    config.rows = 2;
    config.shell = "/bin/sh";
    config.args = {"-c", "read line"};
    core::TerminalSession session(config, std::move(surface), std::make_unique<TestLoop>(), nullptr);
    REQUIRE(session.start());
    const auto frames = output->frames;
    output->blocked = true;
    session.engine().print_utf8("LATEST");
    session.render_frame(true);
    CHECK(output->frames == frames);
    CHECK(session.engine().grid().row(0).dirty);
    output->blocked = false;
    REQUIRE(static_cast<bool>(output->frame_ready));
    output->frame_ready(); // No new PTY output: release notification must finish the last frame.
    CHECK(output->frames == frames + 1);
    CHECK(output->last_damage.x2 == output->width);
    CHECK(output->last_damage.y2 == output->height);
    CHECK_FALSE(session.engine().grid().row(0).dirty);
    CHECK(session.engine().grid().cell(0, 0).codepoint == U'L');
}

TEST_CASE("foot synchronized updates watchdog releases a frame without an end marker") {
    const auto font = session_test_font();
    if (font.empty()) { WARN("No test font found"); return; }
    auto loop = std::make_unique<TestLoop>();
    auto* events = loop.get();
    auto surface = std::make_unique<TestSurface>();
    auto* output = surface.get();
    core::SessionConfig config;
    config.font_path = font;
    config.shell = "/bin/sh";
    config.args = {"-c", "printf '\033[?2026hSYNC'; read line"};
    core::TerminalSession session(config, std::move(surface), std::move(loop), nullptr);
    REQUIRE(session.start());
    const int frames = output->frames;
    for (int i = 0; i < 100 && !events->timer; ++i) events->pump();
    REQUIRE(session.engine().synchronized_updates());
    CHECK(events->timer_delay == 16);
    CHECK(output->frames == frames);
    auto timeout = std::move(events->timer);
    REQUIRE(static_cast<bool>(timeout));
    timeout();
    CHECK_FALSE(session.engine().synchronized_updates());
    CHECK(output->frames == frames + 1);
    CHECK(session.engine().grid().cell(0, 0).codepoint == U'S');
}

TEST_CASE("DECSET 2026 suppresses commits until ESU then flushes one full frame") {
    const auto font = session_test_font();
    if (font.empty()) { WARN("No test font found"); return; }
    auto loop = std::make_unique<TestLoop>();
    auto* events = loop.get();
    auto surface = std::make_unique<TestSurface>();
    auto* output = surface.get();
    core::SessionConfig config;
    config.font_path = font;
    config.shell = "/bin/sh";
    config.args = {"-c",
        "printf '\\033[?2026hSTART'; sleep 0.2; printf 'MID\\033[?2026lEND'; read line"};
    core::TerminalSession session(config, std::move(surface), std::move(loop), nullptr);
    REQUIRE(session.start());
    const int frames = output->frames;
    for (int i = 0; i < 200 && session.engine().grid().cell(4, 0).codepoint != U'T'; ++i)
        events->pump();
    REQUIRE(session.engine().synchronized_updates());
    CHECK(static_cast<bool>(events->timer));
    CHECK(output->frames == frames); // BSU: no half-updated frame on the surface.

    for (int i = 0; i < 300 && session.engine().synchronized_updates(); ++i)
        events->pump();
    CHECK_FALSE(session.engine().synchronized_updates());
    CHECK_FALSE(static_cast<bool>(events->timer)); // ESU cancels the safety timer.
    CHECK(output->frames == frames + 1);
    CHECK(session.engine().grid().cell(0, 0).codepoint == U'S');
    CHECK(session.engine().grid().cell(5, 0).codepoint == U'M');
    CHECK(session.engine().grid().cell(8, 0).codepoint == U'E');
}

TEST_CASE("RIS releases a synchronized update and repaints atomically") {
    const auto font = session_test_font();
    if (font.empty()) { WARN("No test font found"); return; }
    auto loop = std::make_unique<TestLoop>();
    auto* events = loop.get();
    auto surface = std::make_unique<TestSurface>();
    auto* output = surface.get();
    core::SessionConfig config;
    config.font_path = font;
    config.shell = "/bin/sh";
    config.args = {"-c", "printf '\\033[?2026hBROKEN\\033cRESET'; read line"};
    core::TerminalSession session(config, std::move(surface), std::move(loop), nullptr);
    REQUIRE(session.start());
    const int frames = output->frames;
    for (int i = 0; i < 200 && session.engine().grid().cell(0, 0).codepoint != U'R'; ++i)
        events->pump();
    CHECK_FALSE(session.engine().synchronized_updates());
    CHECK_FALSE(static_cast<bool>(events->timer));
    CHECK(output->frames > frames);
    CHECK(session.engine().grid().cell(0, 0).codepoint == U'R');
}

TEST_CASE("TerminalSession enables surface transparency from the actual rendered background") {
    const auto font = session_test_font();
    if (font.empty()) { WARN("No test font found"); return; }
    auto surface = std::make_unique<TestSurface>();
    auto* output = surface.get();
    core::SessionConfig config;
    config.font_path = font;
    config.render_config.default_bg = 0x80604020;
    config.shell = "/bin/sh";
    config.args = {"-c", "exit 0"};
    core::TerminalSession session(config, std::move(surface), std::make_unique<TestLoop>(), nullptr);
    REQUIRE(session.start());
    CHECK(output->transparent);
    CHECK(output->pixels.data()[output->height * output->pixels.stride() / 4 - 1] == 0x80302010);
}

TEST_CASE("OSC default color changes repaint real pixels without removing configured transparency") {
    const auto font = session_test_font();
    if (font.empty()) { WARN("No test font found"); return; }
    auto surface = std::make_unique<TestSurface>();
    auto* output = surface.get();
    auto loop = std::make_unique<TestLoop>();
    auto* events = loop.get();
    core::SessionConfig config;
    config.font_path = font;
    config.cols = 3;
    config.rows = 1;
    config.render_config.default_fg = 0xffeeeeee;
    config.render_config.default_bg = 0x80000000;
    config.render_config.render_cursor = false;
    config.shell = "/bin/sh";
    config.args = {"-c", "stty -echo; printf '\\033]10;#12ab34\\007\\033]11;rgb:00/00/ff\\007"
        "\\342\\226\\210 \\033[41m \\033]2;changed\\007'; read line; "
        "printf '\\033c\\342\\226\\210\\033]2;reset\\007'; read line"};
    core::TerminalSession session(config, std::move(surface), std::move(loop), nullptr);
    REQUIRE(session.start());
    for (int i = 0; i < 100 && output->title != "changed"; ++i) events->pump();
    REQUIRE(output->title == "changed");
    const auto pixel = [&](int col) {
        const int cell_width = output->width / config.cols;
        return output->pixels.data()[(output->height / 2) * output->pixels.stride() / 4
                                     + col * cell_width + cell_width / 2];
    };
    CHECK(pixel(0) == 0xff12ab34);
    CHECK(pixel(1) == 0x80000080); // Premultiplied half-alpha blue.
    CHECK(pixel(2) == (0xff000000 | session.engine().palette_color(1)));
    CHECK(output->transparent);
    session.write_pty("\n");
    for (int i = 0; i < 100 && output->title != "reset"; ++i) events->pump();
    REQUIRE(output->title == "reset");
    CHECK(pixel(0) == 0xffeeeeee);
    CHECK(pixel(1) == 0x80000000);
    CHECK(output->transparent);
}

TEST_CASE("Session preserves configured ANSI colors and all 256 prompt colors through RIS") {
    const auto font = session_test_font();
    if (font.empty()) { WARN("No test font found"); return; }
    const auto loaded = config::TomlLoader::load_from_string("[colors]\nregular1 = '#123456'\n");
    REQUIRE(loaded.has_value());
    auto surface = std::make_unique<TestSurface>();
    auto* output = surface.get();
    auto loop = std::make_unique<TestLoop>();
    auto* events = loop.get();
    core::SessionConfig settings;
    settings.font_path = font;
    settings.cols = 3;
    settings.rows = 1;
    settings.palette = (*loaded)->colors.palette;
    settings.render_config.render_cursor = false;
    settings.shell = "/bin/sh";
    settings.args = {"-c", "stty -echo; printf '\\033c\\033[38;5;33m\\342\\226\\210"
        "\\033[48;5;196m \\033[41m \\033]2;palette\\007'; read line"};
    core::TerminalSession session(settings, std::move(surface), std::move(loop), nullptr);
    REQUIRE(session.start());
    for (int i = 0; i < 100 && output->title != "palette"; ++i) events->pump();
    REQUIRE(output->title == "palette");
    const auto pixel = [&](int col) {
        const int width = output->width / settings.cols;
        return output->pixels.data()[(output->height / 2) * output->pixels.stride() / 4 + col * width + width / 2];
    };
    CHECK(pixel(0) == 0xff0087ff);
    CHECK(pixel(1) == 0xffff0000);
    CHECK(pixel(2) == 0xff123456);
}

TEST_CASE("TerminalSession passes command arguments, title and handles child exit") {
    const auto font = session_test_font();
    if (font.empty()) { WARN("No test font found"); return; }
    auto surface = std::make_unique<TestSurface>();
    auto* output = surface.get();
    auto loop = std::make_unique<TestLoop>();
    auto* events = loop.get();
    core::SessionConfig config;
    config.font_path = font;
    config.cols = 20;
    config.rows = 3;
    config.shell = "/bin/sh";
    config.args = {"-c", "printf '\033]2;session-title\007ARGUMENTS_OK'"};
    core::TerminalSession session(config, std::move(surface), std::move(loop), nullptr);
    REQUIRE(session.start());
    for (int i = 0; i < 100 && !events->stopped; ++i) {
        events->readable();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(events->stopped);
    CHECK(session.engine().grid().cell(0, 0).codepoint == U'A');
    CHECK(session.engine().grid().cell(11, 0).codepoint == U'K');
    CHECK(output->title == "session-title");
}

TEST_CASE("TerminalSession hold_on_exit keeps the loop alive after child exit") {
    const auto font = session_test_font();
    if (font.empty()) { WARN("No test font found"); return; }
    auto loop = std::make_unique<TestLoop>();
    auto* events = loop.get();
    core::SessionConfig config;
    config.font_path = font;
    config.cols = 8;
    config.rows = 2;
    config.shell = "/bin/sh";
    config.args = {"-c", "exit 0"};
    config.hold_on_exit = true;
    core::TerminalSession session(config, std::make_unique<TestSurface>(), std::move(loop), nullptr);
    REQUIRE(session.start());
    const int removals_after_start = events->remove_count;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (events->remove_count == removals_after_start &&
           std::chrono::steady_clock::now() < deadline) events->pump();
    CHECK(events->remove_count > removals_after_start);
    CHECK_FALSE(events->stopped);
    CHECK_FALSE(static_cast<bool>(events->cb));
}

TEST_CASE("TerminalSession retries large nonblocking writes without losing bytes") {
    const auto font = session_test_font();
    if (font.empty()) { WARN("No test font found"); return; }
    auto loop = std::make_unique<TestLoop>();
    auto* events = loop.get();
    core::SessionConfig config;
    config.font_path = font;
    config.cols = 40;
    config.rows = 3;
    config.shell = "/bin/sh";
    config.args = {"-c", "stty -echo -icanon min 1 time 0; printf READY; "
        "dd bs=1 count=131072 2>/dev/null | wc -c"};
    core::TerminalSession session(config, std::make_unique<TestSurface>(), std::move(loop), nullptr);
    REQUIRE(session.start());
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (session.engine().grid().cell(0, 0).codepoint != U'R' &&
           std::chrono::steady_clock::now() < deadline) events->pump();
    REQUIRE(session.engine().grid().cell(0, 0).codepoint == U'R');
    session.write_pty(std::string(131072, 'x'));
    while (!events->stopped && std::chrono::steady_clock::now() < deadline) events->pump();
    CHECK(events->stopped);
    std::string output;
    for (int y = 0; y < session.engine().rows(); ++y)
        for (const auto& cell : session.engine().grid().row(y).cells)
            output.push_back(static_cast<char>(cell.codepoint));
    CHECK(output.find("131072") != std::string::npos);
    CHECK((events->watched & platform::IEventLoop::EventWritable) == 0);
}

TEST_CASE("TerminalSession redraws reallocated buffers and preserves logical size at HiDPI") {
    const auto font = session_test_font();
    if (font.empty()) { WARN("No test font found"); return; }
    auto surface = std::make_unique<TestSurface>();
    auto* output = surface.get();
    core::SessionConfig config;
    config.font_path = font;
    config.cols = 10;
    config.rows = 3;
    config.shell = "/bin/sh";
    config.args = {"-c", "exit 0"};
    core::TerminalSession session(config, std::move(surface), std::make_unique<TestLoop>(), nullptr);
    REQUIRE(session.start());
    const int width = output->width;
    const int height = output->height;
    const int frames = output->frames;
    // A pixel-only resize still replaces the buffer even if rows/cols match.
    output->resize(width + 1, height + 1, 1.0);
    session.handle_surface_resize(width + 1, height + 1, 1.0);
    CHECK(session.engine().cols() == 10);
    CHECK(session.engine().rows() == 3);
    CHECK(output->frames == frames + 1);
    CHECK(output->last_damage.x2 == width + 1);
    CHECK(output->last_damage.y2 == height + 1);
    CHECK(output->pixels.data()[(height + 1) * output->pixels.stride() / 4 - 1] == config.render_config.default_bg);

    render::FontEngine scaled;
    REQUIRE(scaled.load_font(font, config.font_size, config.font_dpi * 2));
    const auto metrics = scaled.metrics();
    output->resize(10 * metrics.cell_width, 3 * metrics.cell_height, 2.0);
    session.handle_surface_resize(output->width, output->height, 2.0);
    CHECK(session.engine().cols() == 10);
    CHECK(session.engine().rows() == 3);
    CHECK(metrics.cell_width > width / 10);

    const int before_sync = output->frames;
    session.engine().set_synchronized_updates(true);
    output->resize(output->width + 1, output->height + 1, 2.0);
    std::fill_n(output->pixels.data(), output->height * output->pixels.stride() / 4, 0xdeadbeef);
    session.handle_surface_resize(output->width, output->height, 2.0);
    CHECK(output->frames == before_sync);
    session.engine().set_synchronized_updates(false);
    session.render_frame();
    CHECK(output->frames == before_sync + 1);
    CHECK(output->last_damage.x2 == output->width);
    CHECK(output->last_damage.y2 == output->height);
    CHECK(output->pixels.data()[output->height * output->pixels.stride() / 4 - 1] == config.render_config.default_bg);
}

TEST_CASE("Cmd+click opens an OSC 8 hyperlink only when mouse reporting is off") {
    const auto font = session_test_font();
    if (font.empty()) { WARN("No test font found"); return; }
    auto surface = std::make_unique<TestSurface>();
    auto* output = surface.get();
    core::SessionConfig config;
    config.font_path = font;
    config.shell = "/bin/sh";
    config.args = {"-c", "read line"};
    core::TerminalSession session(config, std::move(surface), std::make_unique<TestLoop>(), nullptr);
    REQUIRE(session.start());

    const auto id = session.engine().get_or_create_hyperlink_id("https://example.com");
    core::Cell cell;
    cell.codepoint = U'x';
    session.engine().grid().set_cell_with_extra(0, 0, cell, core::CellExtra{.hyperlink_id = id});

    input::MouseEvent event;
    event.button = input::MouseButton::Left;
    event.action = input::MouseAction::Press;
    event.pixel_x = 2;
    event.pixel_y = 2;
    event.mods.meta = true;
    session.handle_mouse_event(event);
    CHECK(output->opened_url == "https://example.com");

    // No Cmd: nothing opens.
    output->opened_url.clear();
    event.mods.meta = false;
    session.handle_mouse_event(event);
    CHECK(output->opened_url.empty());

    // tmux has mouse reporting on: the click belongs to tmux.
    session.engine().set_mouse_tracking(core::MouseTrackingMode::Normal);
    event.mods.meta = true;
    session.handle_mouse_event(event);
    CHECK(output->opened_url.empty());
}

TEST_CASE("DECSET 1004 focus reports are forwarded to the child") {
    const auto font = session_test_font();
    if (font.empty()) { WARN("No test font found"); return; }
    const std::string path = "/tmp/gritty-focus-" + std::to_string(::getpid()) + ".bin";
    std::remove(path.c_str());
    auto surface = std::make_unique<TestSurface>();
    auto loop = std::make_unique<TestLoop>();
    auto* events = loop.get();
    core::SessionConfig config;
    config.font_path = font;
    config.shell = "/bin/sh";
    config.args = {"-c", "stty raw -echo; dd of=" + path + " bs=1 count=6 2>/dev/null"};
    core::TerminalSession session(config, std::move(surface), std::move(loop), nullptr);
    REQUIRE(session.start());

    // Disabled: no bytes are sent.
    session.on_window_focus(true);
    session.engine().set_focus_events(true);
    session.on_window_focus(true);
    session.on_window_focus(false);

    std::string data;
    for (int i = 0; i < 200 && data.size() < 6; ++i) {
        events->pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) continue;
        char buf[8]{};
        const size_t n = std::fread(buf, 1, sizeof(buf), f);
        std::fclose(f);
        data.assign(buf, n);
    }
    std::remove(path.c_str());
    CHECK(data == "\x1b[I\x1b[O");
}

TEST_CASE("Native paste is bracketed and sanitized before reaching the PTY") {
    const auto font = session_test_font();
    if (font.empty()) { WARN("No test font found"); return; }
    const std::string path = "/tmp/gritty-paste-" + std::to_string(::getpid()) + ".bin";
    std::remove(path.c_str());
    const std::string expected = "\x1b[200~secret tail\x1b[201~";
    auto surface = std::make_unique<TestSurface>();
    auto* output = surface.get();
    auto loop = std::make_unique<TestLoop>();
    auto* events = loop.get();
    core::SessionConfig config;
    config.font_path = font;
    config.shell = "/bin/sh";
    config.args = {"-c", "stty raw -echo; printf '\\033[?2004h'; dd of=" + path
        + " bs=1 count=" + std::to_string(expected.size()) + " 2>/dev/null; sleep 1"};
    core::TerminalSession session(config, std::move(surface), std::move(loop), nullptr);
    REQUIRE(session.start());

    for (int i = 0; i < 200 && !session.engine().bracketed_paste(); ++i) {
        events->pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(session.engine().bracketed_paste());
    output->paste_text("secret\x1b[201~ tail");

    std::string data;
    for (int i = 0; i < 200 && data.size() < expected.size(); ++i) {
        events->pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) continue;
        char buf[64]{};
        const size_t n = std::fread(buf, 1, sizeof(buf), f);
        std::fclose(f);
        data.assign(buf, n);
    }
    std::remove(path.c_str());
    CHECK(data == expected);
}

TEST_CASE("Kitty graphics APC is parsed, acknowledged and rendered") {
    const auto font = session_test_font();
    if (font.empty()) { WARN("No test font found"); return; }
    auto surface = std::make_unique<TestSurface>();
    auto* output = surface.get();
    auto loop = std::make_unique<TestLoop>();
    auto* events = loop.get();
    core::SessionConfig config;
    config.font_path = font;
    config.cols = 8;
    config.rows = 3;
    config.shell = "/bin/sh";
    config.args = {"-c", "stty -echo; printf '\\033_Ga=T,f=32,s=1,v=1,i=10;/wAA/w==\\033\\\\'; sleep 1"};
    core::TerminalSession session(config, std::move(surface), std::move(loop), nullptr);
    REQUIRE(session.start());

    for (int i = 0; i < 200 && session.engine().images().empty(); ++i) {
        events->pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(session.engine().images().size() == 1);
    const auto& image = session.engine().images().front();
    CHECK(image.image.width() == 1);
    CHECK(image.image.height() == 1);
    CHECK(image.z_index == 0);

    session.engine().set_cursor_visible(false);
    session.render_frame(true);
    const int stride = output->pixels.stride() / 4;
    const int cw = output->width / config.cols;
    const int ch = output->height / config.rows;
    // With c= and r= omitted, Kitty renders at natural 1x1 pixel size at (0, 0).
    CHECK(output->pixels.data()[0] == 0xFFFF0000u);
    CHECK(output->pixels.data()[(ch / 2) * stride + cw / 2] == config.render_config.default_bg);

    // Explicitly specifying c=1,r=1 scales the placement to fill the cell.
    CHECK(session.engine().kitty_graphics().process_apc_sequence(
              "Ga=p,i=10,p=1,c=1,r=1;", 0, 0) == "\x1b_Gi=10;OK\x1b\\");
    session.render_frame(true);
    CHECK(output->pixels.data()[(ch / 2) * stride + cw / 2] == 0xFFFF0000u);
}

TEST_CASE("Small paced PTY output bypasses the coalescing timer") {
    auto surface = std::make_unique<TestSurface>();
    auto* output = surface.get();
    output->pacing = true;
    auto loop = std::make_unique<TestLoop>();
    auto* events = loop.get();
    core::SessionConfig config;
    config.font_path = session_test_font();
    config.cols = 8;
    config.rows = 2;
    config.shell = "/bin/sh";
    config.args = {"-c", "printf A; sleep 1"};
    core::TerminalSession session(config, std::move(surface), std::move(loop), nullptr);
    REQUIRE(session.start());

    const auto frames = output->frames;
    for (int i = 0; i < 100 && output->frames == frames; ++i)
        events->pump();
    CHECK(output->frames > frames);
    CHECK(events->timer_delay == 0); // No FrameCoalesce timer was armed.
}

TEST_CASE("Window padding surrounds the grid and offsets input coordinates") {
    const auto font = session_test_font();
    if (font.empty()) { WARN("No test font found"); return; }
    auto surface = std::make_unique<TestSurface>();
    auto* output = surface.get();
    core::SessionConfig config;
    config.font_path = font;
    config.cols = 8;
    config.rows = 2;
    config.padding_x = 4;
    config.padding_y = 3;
    config.render_config.render_cursor = false;
    config.shell = "/bin/sh";
    config.args = {"-c", "read line"};
    core::TerminalSession session(config, std::move(surface), std::make_unique<TestLoop>(), nullptr);
    REQUIRE(session.start());

    const auto& metrics = session.font_engine().metrics();
    CHECK(output->width == config.cols * metrics.cell_width + 8);
    CHECK(output->height == config.rows * metrics.cell_height + 6);
    CHECK(output->last_damage.x1 == 0);
    CHECK(output->last_damage.y1 == 0);
    CHECK(output->last_damage.x2 == output->width);
    CHECK(output->last_damage.y2 == output->height);
    const auto pixel = [&](int x, int y) {
        return output->pixels.data()[y * (output->pixels.stride() / 4) + x];
    };
    CHECK(pixel(0, 0) == config.render_config.default_bg);
    CHECK(pixel(output->width - 1, output->height - 1) == config.render_config.default_bg);

    core::Cell block;
    block.codepoint = U'\u2588'; // Full block: exact foreground colour.
    session.engine().grid().set_cell(0, 0, block);
    session.render_frame(true);
    CHECK(pixel(config.padding_x + metrics.cell_width / 2,
                config.padding_y + metrics.cell_height / 2) == 0xffdddddd);

    const auto id = session.engine().get_or_create_hyperlink_id("https://example.com/pad");
    core::Cell linked;
    linked.codepoint = U'x';
    session.engine().grid().set_cell_with_extra(0, 0, linked,
        core::CellExtra{.hyperlink_id = id});
    input::MouseEvent click;
    click.button = input::MouseButton::Left;
    click.action = input::MouseAction::Press;
    click.mods.meta = true;
    click.pixel_x = config.padding_x + metrics.cell_width / 2;
    click.pixel_y = config.padding_y + metrics.cell_height / 2;
    session.handle_mouse_event(click);
    CHECK(output->opened_url == "https://example.com/pad");

    output->opened_url.clear();
    click.pixel_x = 1;
    click.pixel_y = 1;
    session.handle_mouse_event(click);
    CHECK(output->opened_url.empty());
}

TEST_CASE("Window padding with scroll moves stays in bounds and matches full redraw") {
    const auto font = session_test_font();
    if (font.empty()) { WARN("No test font found"); return; }
    auto surface = std::make_unique<TestSurface>();
    auto* output = surface.get();
    core::SessionConfig config;
    config.font_path = font;
    config.cols = 12;
    config.rows = 4;
    config.padding_x = 7;
    config.padding_y = 0;
    config.render_config.render_cursor = false;
    config.shell = "/bin/sh";
    config.args = {"-c", "read line"};
    core::TerminalSession session(config, std::move(surface), std::make_unique<TestLoop>(), nullptr);
    REQUIRE(session.start());

    for (int i = 0; i < 4; ++i) {
        session.engine().print_ascii_run("0123456789AB");
        if (i < 3) {
            session.engine().carriage_return();
            session.engine().line_feed();
        }
    }
    session.render_frame(false);
    // Now scroll one already-rendered line so destinations are clean and memmove runs.
    session.engine().carriage_return();
    session.engine().line_feed();
    session.engine().print_ascii_run("NEWLINE67890");
    session.render_frame(false);
    CHECK(session.render_stats().scroll_moves > 0);

    util::PixmanImage expected(pixman_image_create_bits(
        PIXMAN_a8r8g8b8, output->width, output->height, nullptr, 0));
    REQUIRE(expected.valid());
    render::FontEngine ref_fonts;
    REQUIRE(ref_fonts.load_font(font, config.font_size, config.font_dpi));
    render::CpuRenderer ref_renderer(ref_fonts, config.render_config);
    const auto bg = render::CpuRenderer::to_pixman_color(config.render_config.default_bg);
    const pixman_rectangle16_t full_rect{0, 0,
        static_cast<uint16_t>(output->width), static_cast<uint16_t>(output->height)};
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, expected.get(), &bg, 1, &full_rect);
    const auto& metrics = ref_fonts.metrics();
    const int content_w = config.cols * metrics.cell_width;
    const int content_h = config.rows * metrics.cell_height;
    auto* exp_bits = reinterpret_cast<uint32_t*>(
        reinterpret_cast<uint8_t*>(expected.data()) + config.padding_x * 4);
    util::PixmanImage exp_view(pixman_image_create_bits(
        PIXMAN_a8r8g8b8, content_w, content_h, exp_bits, expected.stride()));
    ref_renderer.render(session.engine(), exp_view.get(), nullptr, true);

    for (int y = 0; y < output->height; ++y) {
        const auto* actual_row = output->pixels.data() + y * (output->pixels.stride() / 4);
        const auto* expected_row = expected.data() + y * (expected.stride() / 4);
        for (int x = 0; x < output->width; ++x)
            REQUIRE(actual_row[x] == expected_row[x]);
    }
}

TEST_CASE("Transparent background premultiplies padding consistently with grid") {
    const auto font = session_test_font();
    if (font.empty()) { WARN("No test font found"); return; }
    auto surface = std::make_unique<TestSurface>();
    auto* output = surface.get();
    core::SessionConfig config;
    config.font_path = font;
    config.cols = 4;
    config.rows = 2;
    config.padding_x = 4;
    config.padding_y = 3;
    config.render_config.default_bg = 0x80604020;
    config.render_config.render_cursor = false;
    config.shell = "/bin/sh";
    config.args = {"-c", "exit 0"};
    core::TerminalSession session(config, std::move(surface), std::make_unique<TestLoop>(), nullptr);
    REQUIRE(session.start());
    const uint32_t pad_px = output->pixels.data()[0];
    const uint32_t grid_px = output->pixels.data()[
        config.padding_y * (output->pixels.stride() / 4) + config.padding_x];
    CHECK(pad_px == 0x80302010u);
    CHECK(grid_px == pad_px);
}

TEST_CASE("DECSET 2026 ESU flushes incrementally without forcing a full redraw") {
    const auto font = session_test_font();
    if (font.empty()) { WARN("No test font found"); return; }
    auto loop = std::make_unique<TestLoop>();
    auto* events = loop.get();
    auto surface = std::make_unique<TestSurface>();
    auto* output = surface.get();
    core::SessionConfig config;
    config.font_path = font;
    config.cols = 20;
    config.rows = 10;
    config.shell = "/bin/sh";
    config.args = {"-c", "printf '\\033[?2026hHI\\033[?2026l'; read line"};
    core::TerminalSession session(config, std::move(surface), std::move(loop), nullptr);
    REQUIRE(session.start());
    const size_t initial_full = session.render_stats().full_redraws;
    const int initial_frames = output->frames;
    for (int i = 0; i < 200 && output->frames == initial_frames; ++i)
        events->pump();
    REQUIRE(output->frames > initial_frames);
    CHECK(session.render_stats().full_redraws == initial_full);
    CHECK(output->last_damage.y2 < output->height);
}

TEST_CASE("HiDPI scale change preserves registered fallback fonts and TERM propagates") {
    const auto font = session_test_font();
    if (font.empty()) { WARN("No test font found"); return; }
    auto surface = std::make_unique<TestSurface>();
    auto* output = surface.get();
    auto loop = std::make_unique<TestLoop>();
    auto* events = loop.get();
    core::SessionConfig config;
    config.font_path = font;
    config.term = "foot-extra";
    config.cols = 16;
    config.rows = 2;
    config.shell = "/bin/sh";
    config.args = {"-c", "printf '%s' \"$TERM\"; read line"};
    core::TerminalSession session(config, std::move(surface), std::move(loop), nullptr);
    REQUIRE(session.start());
    for (int i = 0; i < 200 && session.engine().grid().cell(9, 0).codepoint != U'a'; ++i)
        events->pump();
    std::string term_out;
    for (int x = 0; x < 10; ++x)
        term_out.push_back(static_cast<char>(session.engine().grid().cell(x, 0).codepoint));
    CHECK(term_out == "foot-extra");

    session.add_fallback_font(font, "FallbackAlias");
    // Open the fallback by resolving a character missing from Monaco or force via set_dpi:
    session.handle_surface_resize(output->width * 2, output->height * 2, 2.0);
    // Registering another copy of the same path+ps succeeds as duplicate check,
    // proving the fallback entry was preserved across DPI change.
    const auto before_attempts = session.font_engine().face_load_attempt_count();
    session.add_fallback_font(font, "FallbackAlias");
    CHECK(session.font_engine().face_load_attempt_count() == before_attempts);
}
