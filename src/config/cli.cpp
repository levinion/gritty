#include "config/cli.hpp"

#include "config/parse_helpers.hpp"
#include "util/version.hpp"

#include <cstdint>
#include <iterator>
#include <string>
#include <utility>

namespace gritty::config {

namespace {

constexpr int kMaxGeometry = 1000;
constexpr double kMinFontSize = 1.0;
constexpr double kMaxFontSize = 200.0;

struct Argument {
    std::string_view name;
    std::string_view inline_value;
    bool has_inline_value{false};
};

[[nodiscard]] Argument split_argument(std::string_view arg) noexcept {
    const size_t equals = arg.find('=');
    if (equals == std::string_view::npos) return {arg, {}, false};
    return {arg.substr(0, equals), arg.substr(equals + 1), true};
}

[[nodiscard]] std::expected<std::string_view, std::string> take_value(
    const Argument& arg,
    int& index,
    int argc,
    const char* const* argv
) {
    if (arg.has_inline_value) return arg.inline_value;
    if (index + 1 >= argc)
        return std::unexpected(std::string("Missing value for ") + std::string(arg.name));
    return std::string_view(argv[++index]);
}

[[nodiscard]] bool parse_geometry(std::string_view text, size_t& out) noexcept {
    int64_t parsed = 0;
    if (!detail::parse_integer(text, parsed) || parsed < 1 || parsed > kMaxGeometry) return false;
    out = static_cast<size_t>(parsed);
    return true;
}

[[nodiscard]] bool parse_font_size(std::string_view text, double& out) noexcept {
    double value = 0.0;
    if (!detail::parse_double(text, value) || value < kMinFontSize || value > kMaxFontSize) {
        return false;
    }
    out = value;
    return true;
}

[[nodiscard]] std::unexpected<std::string> invalid_geometry(std::string_view name) {
    return std::unexpected(std::string("Invalid ") + std::string(name) +
                           " (expected 1..1000)");
}

} // namespace

std::expected<CliOptions, std::string> parse_cli(int argc, const char* const* argv) {
    CliOptions options;
    if (argc <= 0 || argv == nullptr) return options;

    for (int i = 1; i < argc; ++i) {
        const std::string_view raw(argv[i]);
        if (raw.empty()) {
            options.command = std::string(raw);
            return options;
        }
        if (raw[0] != '-') {
            options.command = std::string(raw);
            for (int j = i + 1; j < argc; ++j) options.command_args.emplace_back(argv[j]);
            return options;
        }

        // -okey=value is the compact form; -o and --option also accept a
        // separate next argument.
        if (raw.starts_with("-o") && !raw.starts_with("--") && raw.size() > 2) {
            const std::string expr(raw.substr(2));
            const size_t equals = expr.find('=');
            if (equals == std::string::npos || equals == 0)
                return std::unexpected(std::string("Invalid option override: ") + expr);
            options.config_overrides.push_back(expr);
            continue;
        }

        const Argument arg = split_argument(raw);

        if (arg.name == "--") {
            if (arg.has_inline_value)
                return std::unexpected("Unexpected value for --");
            if (i + 1 < argc) {
                options.command = argv[i + 1];
                for (int j = i + 2; j < argc; ++j) options.command_args.emplace_back(argv[j]);
            }
            return options;
        }

        if (arg.name == "-e" || arg.name == "--exec" || arg.name == "--command") {
            auto value = take_value(arg, i, argc, argv);
            if (!value) return std::unexpected(std::move(value.error()));
            options.command = std::string(*value);
            for (int j = i + 1; j < argc; ++j) options.command_args.emplace_back(argv[j]);
            return options;
        }

        if (arg.name == "-h" || arg.name == "--help") {
            if (arg.has_inline_value) return std::unexpected("Unexpected value for --help");
            options.action = CliAction::Help;
            return options;
        }
        if (arg.name == "-V" || arg.name == "--version" || arg.name == "-v") {
            if (arg.has_inline_value) return std::unexpected("Unexpected value for --version");
            options.action = CliAction::Version;
            return options;
        }
        if (arg.name == "--hold") {
            if (arg.has_inline_value) return std::unexpected("Unexpected value for --hold");
            options.hold = true;
            continue;
        }
        if (arg.name == "--standalone") {
            if (arg.has_inline_value) return std::unexpected("Unexpected value for --standalone");
            options.standalone = true;
            continue;
        }

        if (arg.name == "-o" || arg.name == "--option") {
            auto value = take_value(arg, i, argc, argv);
            if (!value) return std::unexpected(std::move(value.error()));
            const std::string expr(*value);
            const size_t equals = expr.find('=');
            if (equals == std::string::npos || equals == 0)
                return std::unexpected(std::string("Invalid option override: ") + expr);
            options.config_overrides.push_back(expr);
            continue;
        }

        if (arg.name == "-c" || arg.name == "--config" || arg.name == "--config-file") {
            auto value = take_value(arg, i, argc, argv);
            if (!value) return std::unexpected(std::move(value.error()));
            options.config_path = std::string(*value);
            continue;
        }
        if (arg.name == "--working-directory" || arg.name == "--cwd") {
            auto value = take_value(arg, i, argc, argv);
            if (!value) return std::unexpected(std::move(value.error()));
            options.working_directory = std::string(*value);
            continue;
        }
        if (arg.name == "-T" || arg.name == "--title" || arg.name == "-t") {
            auto value = take_value(arg, i, argc, argv);
            if (!value) return std::unexpected(std::move(value.error()));
            options.title = std::string(*value);
            continue;
        }
        if (arg.name == "--class") {
            auto value = take_value(arg, i, argc, argv);
            if (!value) return std::unexpected(std::move(value.error()));
            const size_t comma = value->find(',');
            options.app_id = std::string(comma == std::string_view::npos
                ? *value : value->substr(0, comma));
            continue;
        }
        if (arg.name == "-f" || arg.name == "--font") {
            auto value = take_value(arg, i, argc, argv);
            if (!value) return std::unexpected(std::move(value.error()));
            options.font_family = std::string(*value);
            continue;
        }
        if (arg.name == "-s" || arg.name == "--size") {
            auto value = take_value(arg, i, argc, argv);
            if (!value) return std::unexpected(std::move(value.error()));
            if (!parse_font_size(*value, options.font_size))
                return std::unexpected("Invalid font size (expected 1..200 points)");
            continue;
        }
        if (arg.name == "--cols" || arg.name == "--rows") {
            auto value = take_value(arg, i, argc, argv);
            if (!value) return std::unexpected(std::move(value.error()));
            size_t parsed = 0;
            if (!parse_geometry(*value, parsed)) return invalid_geometry(arg.name);
            if (arg.name == "--cols") options.cols = parsed;
            else options.rows = parsed;
            continue;
        }

        return std::unexpected(std::string("Unknown option: ") + std::string(raw));
    }
    return options;
}

std::string format_cli_help(std::string_view prog) {
    std::string out;
    out.reserve(1024);
    out += "Usage: ";
    out.append(prog);
    out += " [options] [command [args...]]\n\n";
    out += "Options:\n";
    out += "  -h, --help                 Show this help\n";
    out += "  -V, --version              Print version (-v is accepted)\n";
    out += "  --config-file <path>       Configuration file (-c, --config)\n";
    out += "  -o <key=value>             Override a config value; repeatable\n";
    out += "  --working-directory <dir>  Child working directory (--cwd)\n";
    out += "  -T <title>                 Window title (--title, -t)\n";
    out += "  --class <general[,instance]> Set the application ID\n";
    out += "  -f <family>                Font family (--font)\n";
    out += "  -s <pt>                    Font size in points (--size)\n";
    out += "  --cols <n>                 Initial columns (1..1000)\n";
    out += "  --rows <n>                 Initial rows (1..1000)\n";
    out += "  --hold                     Keep the window open after the child exits\n";
    out += "  --standalone               Start an isolated instance without IPC\n";
    out += "  -e <cmd> [args...]         Execute command (--command, --exec, --)\n";
    return out;
}

std::string format_cli_version() {
    return "Gritty " + std::string(util::kVersion) + " (foot parity + Kitty protocols)\n";
}

ShellCommand split_shell_command(std::string_view cmd) {
    ShellCommand result;
    std::vector<std::string> tokens;
    std::string current;
    bool in_token = false;
    bool in_single = false;
    bool in_double = false;
    bool escaped = false;

    for (const char ch : cmd) {
        if (escaped) {
            current.push_back(ch);
            escaped = false;
            in_token = true;
            continue;
        }
        if (in_single) {
            if (ch == '\'') in_single = false;
            else current.push_back(ch);
            in_token = true;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            in_token = true;
            continue;
        }
        if (in_double) {
            if (ch == '"') in_double = false;
            else current.push_back(ch);
            in_token = true;
            continue;
        }
        if (ch == '\'') {
            in_single = true;
            in_token = true;
            continue;
        }
        if (ch == '"') {
            in_double = true;
            in_token = true;
            continue;
        }
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            if (in_token) {
                tokens.push_back(std::move(current));
                current.clear();
                in_token = false;
            }
            continue;
        }
        current.push_back(ch);
        in_token = true;
    }
    if (escaped) current.push_back('\\');
    if (in_token) tokens.push_back(std::move(current));
    if (!tokens.empty()) {
        result.executable = std::move(tokens.front());
        result.args.assign(std::make_move_iterator(tokens.begin() + 1),
                           std::make_move_iterator(tokens.end()));
    }
    return result;
}

std::string resolve_working_directory(
    std::string_view explicit_cwd,
    std::string_view inherited_cwd,
    std::string_view home_dir
) {
    if (!explicit_cwd.empty()) return std::string(explicit_cwd);
    if (inherited_cwd == "/" && !home_dir.empty()) return std::string(home_dir);
    return {};
}

} // namespace gritty::config
