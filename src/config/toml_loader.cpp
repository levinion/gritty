#include "config/toml_loader.hpp"

#include "config/tomlc17.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace gritty::config {

namespace {

class ParseResult final {
public:
    explicit ParseResult(toml_result_t result) noexcept : result_(result) {}
    ~ParseResult() noexcept { toml_free(result_); }

    ParseResult(const ParseResult&) = delete;
    ParseResult& operator=(const ParseResult&) = delete;

    [[nodiscard]] bool ok() const noexcept { return result_.ok; }
    [[nodiscard]] toml_datum_t root() const noexcept { return result_.toptab; }
    [[nodiscard]] const char* error() const noexcept { return result_.errmsg; }

private:
    toml_result_t result_;
};

toml_datum_t find_key(toml_datum_t table, std::string_view key) noexcept {
    if (table.type != TOML_TABLE || key.size() > std::numeric_limits<int>::max())
        return toml_datum_t{};
    const std::string NUL_terminated(key);
    return toml_get(table, NUL_terminated.c_str());
}

std::optional<std::string_view> as_string(toml_datum_t datum) noexcept {
    if (datum.type != TOML_STRING || !datum.u.str.ptr) return std::nullopt;
    return std::string_view(datum.u.str.ptr, static_cast<size_t>(datum.u.str.len));
}

bool as_integer(toml_datum_t datum, int64_t& out) noexcept {
    if (datum.type != TOML_INT64) return false;
    out = datum.u.int64;
    return true;
}

bool as_number(toml_datum_t datum, double& out) noexcept {
    if (datum.type == TOML_FP64) out = datum.u.fp64;
    else if (datum.type == TOML_INT64) out = static_cast<double>(datum.u.int64);
    else return false;
    return true;
}

bool as_bool(toml_datum_t datum, bool& out) noexcept {
    if (datum.type != TOML_BOOLEAN) return false;
    out = datum.u.boolean;
    return true;
}

std::string lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
        out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    return out;
}

std::string target_platform(std::string_view override) {
    if (!override.empty()) return lower(override);
#if defined(PLATFORM_MACOS)
    return "macos";
#elif defined(PLATFORM_LINUX)
    return "linux";
#else
    return {};
#endif
}

ConfigError parse_error(const char* message) {
    ConfigError result{.message = message ? message : "TOML parse error"};
    const std::string_view text(result.message);
    constexpr std::string_view prefix = "(line ";
    if (text.starts_with(prefix)) {
        const char* first = text.data() + prefix.size();
        const char* last = first;
        while (last < text.data() + text.size() && *last >= '0' && *last <= '9') ++last;
        int line = 0;
        const auto [end, ec] = std::from_chars(first, last, line);
        if (ec == std::errc{} && end == last) result.line = line;
    }
    return result;
}

void apply_colors(AppConfig& cfg, toml_datum_t table, bool& invalid_alpha) {
    if (const auto value = as_string(find_key(table, "foreground"))) {
        if (auto color = TomlLoader::parse_color(*value)) cfg.colors.foreground = *color;
    }
    if (const auto value = as_string(find_key(table, "background"))) {
        if (auto color = TomlLoader::parse_color(*value)) cfg.colors.background = *color;
    }
    if (const auto value = as_string(find_key(table, "cursor"))) {
        if (auto color = TomlLoader::parse_color(*value)) cfg.colors.cursor = *color;
    }
    if (const auto value = as_string(find_key(table, "selection_fg"))) {
        cfg.colors.selection_fg = TomlLoader::parse_color(*value);
    }
    if (const auto value = as_string(find_key(table, "selection_bg"))) {
        cfg.colors.selection_bg = TomlLoader::parse_color(*value);
    }

    for (int i = 0; i < 8; ++i) {
        const std::string regular = "regular" + std::to_string(i);
        const std::string bright = "bright" + std::to_string(i);
        if (const auto value = as_string(find_key(table, regular))) {
            if (auto color = TomlLoader::parse_color(*value)) {
                cfg.colors.palette[static_cast<size_t>(i)] = *color & 0x00ffffff;
                cfg.colors.palette_customized = true;
            }
        }
        if (const auto value = as_string(find_key(table, bright))) {
            if (auto color = TomlLoader::parse_color(*value)) {
                cfg.colors.palette[static_cast<size_t>(i + 8)] = *color & 0x00ffffff;
                cfg.colors.palette_customized = true;
            }
        }
    }

    const toml_datum_t alpha = find_key(table, "alpha");
    if (alpha.type == TOML_UNKNOWN) return;
    double parsed = 0;
    if (!as_number(alpha, parsed) || !std::isfinite(parsed) ||
        parsed < 0.0 || parsed > 1.0) {
        invalid_alpha = true;
        return;
    }
    cfg.colors.alpha = parsed;
}

void apply_section(AppConfig& cfg, std::string_view section, toml_datum_t table,
                   bool& invalid_alpha, bool& invalid_decorations) {
    if (section == "main") {
        if (const auto value = as_string(find_key(table, "shell"))) cfg.main.shell = *value;
        if (const auto value = as_string(find_key(table, "term"))) cfg.main.term = *value;
        if (const auto value = as_string(find_key(table, "title"))) cfg.main.title = *value;
        return;
    }
    if (section == "font") {
        if (const auto value = as_string(find_key(table, "family"))) cfg.font.family = *value;
        double size = 0;
        if (as_number(find_key(table, "size"), size) && size > 0 && std::isfinite(size))
            cfg.font.size = size;
        int64_t dpi = 0;
        if (as_integer(find_key(table, "dpi"), dpi) && dpi > 0 &&
            static_cast<uint64_t>(dpi) <= std::numeric_limits<unsigned int>::max())
            cfg.font.dpi = static_cast<unsigned int>(dpi);
        if (const auto value = as_string(find_key(table, "antialias"))) {
            const std::string mode = lower(*value);
            if (mode == "grayscale") cfg.font.antialias = render::AntialiasMode::Grayscale;
            else if (mode == "subpixel" || mode == "lcd") cfg.font.antialias = render::AntialiasMode::SubpixelLcd;
            else if (mode == "none") cfg.font.antialias = render::AntialiasMode::None;
        }
        return;
    }
    if (section == "cursor") {
        if (const auto value = as_string(find_key(table, "style"))) {
            const std::string style = lower(*value);
            if (style == "block") cfg.cursor.style = core::CursorShape::Block;
            else if (style == "beam") cfg.cursor.style = core::CursorShape::Beam;
            else if (style == "underline") cfg.cursor.style = core::CursorShape::Underline;
        }
        bool blink = false;
        if (as_bool(find_key(table, "blink"), blink)) cfg.cursor.blink = blink;
        if (const auto value = as_string(find_key(table, "color"))) {
            if (auto color = TomlLoader::parse_color(*value)) cfg.cursor.color = *color;
        }
        return;
    }
    if (section == "colors") {
        apply_colors(cfg, table, invalid_alpha);
        return;
    }
    if (section == "scrollback") {
        int64_t lines = 0;
        if (as_integer(find_key(table, "lines"), lines) && lines >= 0)
            cfg.scrollback.lines = static_cast<size_t>(lines);
        int64_t multiplier = 0;
        if (as_integer(find_key(table, "multiplier"), multiplier) && multiplier > 0 &&
            multiplier <= std::numeric_limits<int>::max())
            cfg.scrollback.multiplier = static_cast<int>(multiplier);
        return;
    }
    if (section == "window") {
        if (const auto value = as_string(find_key(table, "decorations"))) {
            const auto decorations = parse_window_decorations(*value);
            if (!decorations) invalid_decorations = true;
            else cfg.window.decorations = *decorations;
        }
    }
}

void apply_tables(AppConfig& cfg, toml_datum_t root, bool& invalid_alpha,
                  bool& invalid_decorations) {
    constexpr std::string_view sections[] = {
        "main", "font", "cursor", "colors", "scrollback", "window"
    };
    for (const auto section : sections) {
        const toml_datum_t table = find_key(root, section);
        if (table.type == TOML_TABLE)
            apply_section(cfg, section, table, invalid_alpha, invalid_decorations);
    }
}

void apply_platform(AppConfig& cfg, toml_datum_t root, std::string_view platform,
                    bool& invalid_alpha, bool& invalid_decorations) {
    // Accept both [platform.macos.font] and legacy [macos.font] forms.
    toml_datum_t platform_root = find_key(root, "platform");
    const bool prefixed = platform_root.type == TOML_TABLE;
    if (!prefixed) platform_root = root;
    const toml_datum_t os = find_key(platform_root, platform);
    if (os.type != TOML_TABLE) return;

    constexpr std::string_view sections[] = {
        "main", "font", "cursor", "colors", "scrollback", "window"
    };
    for (const auto section : sections) {
        const toml_datum_t table = find_key(os, section);
        if (table.type == TOML_TABLE)
            apply_section(cfg, section, table, invalid_alpha, invalid_decorations);
    }
    if (prefixed) {
        // [platform.macos] font = { family = "...", size = 14.0 }
        apply_tables(cfg, os, invalid_alpha, invalid_decorations);
    }
}

std::expected<std::shared_ptr<AppConfig>, ConfigError> load(
    std::string_view text,
    std::string_view platform_override
) {
    const std::string source(text);
    ParseResult parsed(toml_parse(source.c_str(), static_cast<int>(source.size())));
    if (!parsed.ok()) return std::unexpected(parse_error(parsed.error()));

    auto cfg = std::make_shared<AppConfig>();
    bool invalid_alpha = false;
    bool invalid_decorations = false;
    apply_tables(*cfg, parsed.root(), invalid_alpha, invalid_decorations);
    apply_platform(*cfg, parsed.root(), target_platform(platform_override),
                   invalid_alpha, invalid_decorations);
    if (invalid_alpha)
        return std::unexpected(ConfigError{
            .message = "colors.alpha must be a finite number between 0 and 1"});
    if (invalid_decorations)
        return std::unexpected(ConfigError{
            .message = "window.decorations must be full, none, transparent, or buttonless"});
    return cfg;
}

} // namespace

std::optional<uint32_t> TomlLoader::parse_color(std::string_view str) noexcept {
    str = std::string_view(str.data(), str.size());
    while (!str.empty() && (str.front() == ' ' || str.front() == '\t')) str.remove_prefix(1);
    while (!str.empty() && (str.back() == ' ' || str.back() == '\t')) str.remove_suffix(1);
    if (str.empty()) return std::nullopt;

    if (str.front() == '#') str.remove_prefix(1);
    else if (str.starts_with("0x") || str.starts_with("0X")) str.remove_prefix(2);

    if (str.size() != 6 && str.size() != 8) return std::nullopt;
    uint32_t value = 0;
    const auto [end, ec] = std::from_chars(str.data(), str.data() + str.size(), value, 16);
    if (ec != std::errc{} || end != str.data() + str.size()) return std::nullopt;
    return str.size() == 6 ? (0xff000000u | value) : value;
}

std::expected<std::shared_ptr<AppConfig>, ConfigError> TomlLoader::load_from_string(
    std::string_view toml_str,
    std::string_view platform_override
) {
    return load(toml_str, platform_override);
}

std::expected<std::shared_ptr<AppConfig>, ConfigError> TomlLoader::load_from_file(
    std::string_view file_path,
    std::string_view platform_override
) {
    const std::string path(file_path);
    std::ifstream file(path);
    if (!file.is_open())
        return std::unexpected(ConfigError{
            .message = "Failed to open configuration file: " + std::string(file_path)});
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return load(buffer.str(), platform_override);
}

} // namespace gritty::config
