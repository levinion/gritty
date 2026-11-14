#pragma once

#include <unordered_map>
#include <vector>
#include <atomic>
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
    void run() override;
    void stop() override;
    void wake() override;

    // Process pending events once (with optional timeout in ms)
    int poll_once(int timeout_ms = -1);

private:
    util::UniqueFd kq_fd_;
    util::UniqueFd wake_pipe_read_;
    util::UniqueFd wake_pipe_write_;
    std::atomic<bool> running_{false};
    std::unordered_map<int, FdCallback> callbacks_;
};

} // namespace gritty::platform::macos
