#pragma once

#include "config/app_config.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace gritty::core {

enum class QuitAction : uint8_t {
    QuitNow = 0,
    RequestConfirm = 1,
};

// State machine for double-Cmd+Q quit confirmation.
class QuitConfirmGuard final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit QuitConfirmGuard(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)
    ) noexcept
        : timeout_(timeout) {}

    [[nodiscard]] bool armed() const noexcept { return armed_; }
    [[nodiscard]] std::chrono::milliseconds timeout() const noexcept { return timeout_; }

    void disarm() noexcept {
        armed_ = false;
        armed_at_ = {};
    }

    // Returns true if an armed confirmation expired and was cleared.
    bool expire_if_needed(TimePoint now) noexcept {
        if (!armed_) return false;
        if (now >= armed_at_ && (now - armed_at_) < timeout_) return false;
        disarm();
        return true;
    }

    [[nodiscard]] QuitAction on_quit_requested(
        config::QuitConfirmation policy,
        size_t window_count,
        TimePoint now
    ) noexcept {
        expire_if_needed(now);
        const bool needs_confirm =
            (policy == config::QuitConfirmation::Always) ||
            (policy == config::QuitConfirmation::Needed && window_count > 1);
        if (!needs_confirm || armed_) {
            disarm();
            return QuitAction::QuitNow;
        }
        armed_ = true;
        armed_at_ = now;
        return QuitAction::RequestConfirm;
    }

private:
    std::chrono::milliseconds timeout_;
    bool armed_{false};
    TimePoint armed_at_{};
};

} // namespace gritty::core
