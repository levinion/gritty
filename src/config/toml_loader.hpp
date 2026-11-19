#pragma once

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "config/app_config.hpp"

namespace gritty::config {

struct ConfigError {
    std::string message;
    int line{0};
    int column{0};
};

class TomlLoader final {
public:
    static std::expected<std::shared_ptr<AppConfig>, ConfigError> load_from_string(
        std::string_view toml_str,
        std::string_view platform_override = ""
    );

    static std::expected<std::shared_ptr<AppConfig>, ConfigError> load_from_file(
        std::string_view file_path,
        std::string_view platform_override = ""
    );

    static std::optional<uint32_t> parse_color(std::string_view color_str) noexcept;
};

} // namespace gritty::config
