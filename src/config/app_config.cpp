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
