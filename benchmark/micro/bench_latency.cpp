#include "bench.hpp"
#include "core/terminal_session.hpp"
#include "input/input_events.hpp"
#include "platform/pal_interfaces.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <poll.h>
#include <unistd.h>
#include <vector>

namespace {

using namespace gritty;
using namespace gritty::bench;

class HeadlessSurface final : public platform::IWindowSurface {
public:
    void resize(int pixel_width, int pixel_height, double) override {
        width_ = pixel_width; height_ = pixel_height;
        image_.reset(pixman_image_create_bits(PIXMAN_a8r8g8b8, width_, height_, nullptr, 0));
    }
    pixman_image_t* get_back_buffer() override { return image_.get(); }
    void present(const util::PixmanRegion&) override { ++presents_; }
    [[nodiscard]] int pixel_width() const noexcept override { return width_; }
    [[nodiscard]] int pixel_height() const noexcept override { return height_; }
    [[nodiscard]] double scale() const noexcept override { return 1.0; }
    [[nodiscard]] size_t presents() const noexcept { return presents_; }
private:
    util::PixmanImage image_;
    int width_{640}, height_{384};
    size_t presents_{0};
};

class PollLoop final : public platform::IEventLoop {
public:
    void add_fd(int fd, uint32_t events, FdCallback cb) override {
        fd_callbacks_[fd] = std::move(cb);
        fd_events_[fd] = events;
    }
    void remove_fd(int fd) override {
        fd_callbacks_.erase(fd);
        fd_events_.erase(fd);
    }
    void set_timer(TimerId id, int delay_ms, std::function<void()> callback) override {
        const auto key = static_cast<uintptr_t>(id);
        if (!callback || delay_ms <= 0) { timers_.erase(key); return; }
        timers_[key] = {std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms), std::move(callback)};
    }
    void set_timer(int delay_ms, std::function<void()> callback) override {
        set_timer(TimerId::Default, delay_ms, std::move(callback));
    }
    void run() override { while (!stopped_) step(10); }
    void stop() override { stopped_ = true; }
    void wake() override {}

    bool step(int timeout_ms) {
        fire_timers();
        std::vector<pollfd> fds;
        std::vector<int> ids;
        for (const auto& [fd, events] : fd_events_) {
            short flags = 0;
            if (events & EventReadable) flags |= POLLIN;
            if (events & EventWritable) flags |= POLLOUT;
            fds.push_back({fd, flags, 0});
            ids.push_back(fd);
        }
        if (fds.empty()) { ::usleep(static_cast<useconds_t>(timeout_ms) * 1000); return false; }
        const int n = ::poll(fds.data(), fds.size(), timeout_ms);
        if (n <= 0) return false;
        for (size_t i = 0; i < fds.size(); ++i) {
            auto callback = fd_callbacks_.find(ids[i]);
            if (callback == fd_callbacks_.end()) continue;
            uint32_t flags = 0;
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) flags |= EventReadable;
            if (fds[i].revents & POLLOUT) flags |= EventWritable;
            if (flags) callback->second(ids[i], flags);
        }
        fire_timers();
        return true;
    }

    void set_stopped() noexcept { stopped_ = true; }

private:
    struct Timer {
        std::chrono::steady_clock::time_point deadline;
        std::function<void()> callback;
    };
    void fire_timers() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = timers_.begin(); it != timers_.end();) {
            if (it->second.deadline > now) { ++it; continue; }
            auto callback = std::move(it->second.callback);
            it = timers_.erase(it);
            if (callback) callback();
        }
    }
    std::map<int, FdCallback> fd_callbacks_;
    std::map<int, uint32_t> fd_events_;
    std::map<uintptr_t, Timer> timers_;
    bool stopped_{false};
};

static void latency_pty_roundtrip_500() {
    const auto font_path = find_monospace_font();
    if (font_path.empty()) { std::cerr << "No font; skipping latency benchmark\n"; return; }
    core::SessionConfig config;
    config.font_path = font_path;
    config.cols = 80; config.rows = 24;
    config.shell = "/bin/sh";
    config.args = {"-c", "stty raw -echo; cat"};

    auto surface = std::make_unique<HeadlessSurface>();
    auto* output = surface.get();
    auto loop = std::make_unique<PollLoop>();
    auto* events = loop.get();
    core::TerminalSession session(config, std::move(surface), std::move(loop), nullptr);
    input::KeyEvent key;
    key.key = input::KeyCode::Unknown;
    key.codepoint = 'x';
    key.text = "x";

    size_t observed = 0;
    session.set_present_observer([&] { ++observed; });
    if (!session.start()) { std::cerr << "Session failed\n"; return; }

    constexpr size_t iterations = 500;
    Runner runner(0);
    runner.report("latency/pty_roundtrip_500", 1, iterations, [&](size_t) {
        const auto before = observed;
        session.handle_key_event(key);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
        while (observed == before && std::chrono::steady_clock::now() < deadline)
            events->step(1);
    });
    (void)output;
    events->set_stopped();
}
static Registrar latency_pty_roundtrip_500_reg("latency/pty_roundtrip_500", &latency_pty_roundtrip_500);

} // namespace

namespace gritty::bench {
void register_latency() {}
} // namespace gritty::bench
