#pragma once

#include <unordered_map>
#include <vector>
#include <atomic>
#include <CoreFoundation/CoreFoundation.h>
#include "platform/pal_interfaces.hpp"
#include "util/raii.hpp"

namespace gritty::platform::macos {

class KqueueRunLoop final : public IEventLoop {
public:
    KqueueRunLoop();
    ~KqueueRunLoop() override;

    KqueueRunLoop(const KqueueRunLoop&) = delete;
    KqueueRunLoop& operator=(const KqueueRunLoop&) = delete;

    void add_fd(int fd, uint32_t events, FdCallback cb) override;
    void remove_fd(int fd) override;
    void set_timer(int delay_ms, std::function<void()> callback) override;
    void set_timer(TimerId id, int delay_ms, std::function<void()> callback) override;
    void run() override;
    void stop() override;
    void wake() override;

    // Process pending events once (with optional timeout in ms)
    int poll_once(int timeout_ms = -1);

private:
    static void on_kq_ready(CFFileDescriptorRef fd, CFOptionFlags flags, void* context);

    util::UniqueFd kq_fd_;
    util::UniqueFd wake_pipe_read_;
    util::UniqueFd wake_pipe_write_;
    CFFileDescriptorRef cf_fd_{nullptr};
    CFRunLoopSourceRef cf_source_{nullptr};
    std::atomic<bool> running_{false};
    std::unordered_map<int, FdCallback> callbacks_;
    std::unordered_map<uintptr_t, std::function<void()>> timers_;
};

} // namespace gritty::platform::macos
