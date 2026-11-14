#include <doctest/doctest.h>
#include <chrono>
#include <filesystem>
#include <thread>
#include <poll.h>
#include "core/terminal_session.hpp"

namespace {
using namespace gritty;

std::string session_test_font() {
    for (const auto* path : {"/System/Library/Fonts/Monaco.ttf",
                            "/System/Library/Fonts/Menlo.ttc",
                            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"}) {
        if (std::filesystem::exists(path)) return path;
    }
    return {};
}

class TestSurface final : public platform::IWindowSurface {
public:
    void resize(int w, int h, double s) override {
        width = w;
        height = h;
        scaling = s;
        pixels.reset(pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, nullptr, 0));
    }
    pixman_image_t* get_back_buffer() override { return pixels.get(); }
    void present(const util::PixmanRegion& damage) override {
        ++frames;
        last_damage = damage.extents();
    }
    void set_title(const std::string& value) override { title = value; }
    void set_transparent(bool value) override { transparent = value; }
    bool transparent{false};
    int pixel_width() const noexcept override { return width; }
    int pixel_height() const noexcept override { return height; }
    double scale() const noexcept override { return scaling; }
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
    void remove_fd(int) override { cb = {}; }
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
    bool stopped{false};
};
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
    CHECK(events->timer_delay == 1000);
    CHECK(output->frames == frames);
    auto timeout = std::move(events->timer);
    REQUIRE(static_cast<bool>(timeout));
    timeout();
    CHECK_FALSE(session.engine().synchronized_updates());
    CHECK(output->frames == frames + 1);
    CHECK(session.engine().grid().cell(0, 0).codepoint == U'S');
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
    REQUIRE(scaled.load_font(font, config.font_size, 192));
    const auto metrics = scaled.metrics();
    output->resize(10 * metrics.cell_width, 3 * metrics.cell_height, 2.0);
    session.handle_surface_resize(output->width, output->height, 2.0);
    CHECK(session.engine().cols() == 10);
    CHECK(session.engine().rows() == 3);
    CHECK(metrics.cell_width > width / 10);

    const int before_sync = output->frames;
    session.engine().set_synchronized_updates(true);
    session.render_frame(true);
    CHECK(output->frames == before_sync);
    session.engine().set_synchronized_updates(false);
    session.render_frame();
    CHECK(output->frames == before_sync + 1);
}
