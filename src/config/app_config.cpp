#include "config/app_config.hpp"
#include <algorithm>
#include <cmath>

namespace gritty::config {

AppConfig::AppConfig() {
#if defined(PLATFORM_MACOS)
    font.family = "Monaco";
#else
    font.family = "monospace";
#endif

    // Default ANSI colors
    static constexpr std::array<uint32_t, 16> kAnsi = {
        0x000000, 0xcc0403, 0x19cb00, 0xcecb00,
        0x0d73cc, 0xcb1ed1, 0x0dcdcd, 0xdddddd,
        0x767676, 0xf2201f, 0x23fd00, 0xfffd00,
        0x1a8fff, 0xfd28ff, 0x14ffff, 0xffffff
    };
    for (size_t i = 0; i < 16; ++i) {
        colors.palette[i] = kAnsi[i];
    }
}

uint32_t ColorsConfig::background_argb() const noexcept {
    const double opacity = std::isfinite(alpha) ? std::clamp(alpha, 0.0, 1.0) : 1.0;
    const auto effective_alpha = static_cast<uint32_t>(std::lround((background >> 24) * opacity));
    return (background & 0xffffff) | (effective_alpha << 24);
}

ConfigStore::ConfigStore() : config_(std::make_shared<AppConfig>()) {}

ConfigStore::ConfigStore(std::shared_ptr<const AppConfig> initial)
    : config_(std::move(initial)) {}

} // namespace gritty::config
