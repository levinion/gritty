#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace gritty::config {

enum class CliAction : uint8_t {
    Run,
    Help,
    Version,
};

struct CliOptions {
    CliAction action{CliAction::Run};
    std::string config_path;
    std::string working_directory;
    std::string title;
    std::string app_id;
    std::string font_family;
    double font_size{0.0};
    size_t cols{0};
    size_t rows{0};
    bool hold{false};
    std::vector<std::string> config_overrides;
    std::string command;
    std::vector<std::string> command_args;
};

[[nodiscard]] std::expected<CliOptions, std::string> parse_cli(
    int argc,
    const char* const* argv
);

[[nodiscard]] std::string format_cli_help(std::string_view prog);
[[nodiscard]] std::string format_cli_version();

} // namespace gritty::config
