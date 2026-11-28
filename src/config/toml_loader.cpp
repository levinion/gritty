#include "config/toml_loader.hpp"

#include "config/tomlc17.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
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

struct ValidationErrors {
    std::string first_error;
    void set(std::string_view msg) {
        if (first_error.empty()) first_error.assign(msg);
    }
};

void apply_ansi_group(AppConfig& cfg, toml_datum_t table, size_t base_idx) {
    if (table.type != TOML_TABLE) return;
    constexpr std::array<std::string_view, 8> names{
        "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"
    };
    for (size_t i = 0; i < names.size(); ++i) {
        if (const auto value = as_string(find_key(table, names[i]))) {
            if (auto color = TomlLoader::parse_color(*value)) {
                cfg.colors.palette[base_idx + i] = *color & 0x00ffffff;
                cfg.colors.palette_customized = true;
            }
        }
    }
}

void apply_colors(AppConfig& cfg, toml_datum_t table, bool cursor_color_explicit,
                  ValidationErrors& errors) {
    if (const auto value = as_string(find_key(table, "foreground"))) {
        if (auto color = TomlLoader::parse_color(*value)) cfg.colors.foreground = *color;
    }
    if (const auto value = as_string(find_key(table, "background"))) {
        if (auto color = TomlLoader::parse_color(*value)) cfg.colors.background = *color;
    }
    const toml_datum_t primary = find_key(table, "primary");
    if (primary.type == TOML_TABLE) {
        if (const auto value = as_string(find_key(primary, "foreground"))) {
            if (auto color = TomlLoader::parse_color(*value)) cfg.colors.foreground = *color;
        }
        if (const auto value = as_string(find_key(primary, "background"))) {
            if (auto color = TomlLoader::parse_color(*value)) cfg.colors.background = *color;
        }
    }

    const toml_datum_t cursor_key = find_key(table, "cursor");
    if (const auto value = as_string(cursor_key)) {
        if (auto color = TomlLoader::parse_color(*value)) {
            cfg.colors.cursor = *color;
            if (!cursor_color_explicit) cfg.cursor.color = *color;
        }
    } else if (cursor_key.type == TOML_TABLE) {
        if (const auto value = as_string(find_key(cursor_key, "cursor"))) {
            if (auto color = TomlLoader::parse_color(*value)) {
                cfg.colors.cursor = *color;
                if (!cursor_color_explicit) cfg.cursor.color = *color;
            }
        }
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
    apply_ansi_group(cfg, find_key(table, "normal"), 0);
    apply_ansi_group(cfg, find_key(table, "bright"), 8);

    const toml_datum_t alpha = find_key(table, "alpha");
    if (alpha.type == TOML_UNKNOWN) return;
    double parsed = 0;
    if (!as_number(alpha, parsed) || !std::isfinite(parsed) ||
        parsed < 0.0 || parsed > 1.0) {
        errors.set("colors.alpha must be a finite number between 0 and 1");
        return;
    }
    cfg.colors.alpha = parsed;
}

void apply_padding(AppConfig& cfg, toml_datum_t table, ValidationErrors& errors) {
    const toml_datum_t padding = find_key(table, "padding");
    if (padding.type == TOML_UNKNOWN) return;

    int64_t x = cfg.window.padding.x;
    int64_t y = cfg.window.padding.y;
    bool valid = true;
    if (padding.type == TOML_INT64) {
        x = y = padding.u.int64;
    } else if (padding.type == TOML_TABLE) {
        const toml_datum_t dx = find_key(padding, "x");
        const toml_datum_t dy = find_key(padding, "y");
        if (dx.type == TOML_UNKNOWN && dy.type == TOML_UNKNOWN) {
            valid = false;
        } else {
            if (dx.type != TOML_UNKNOWN && !as_integer(dx, x)) valid = false;
            if (dy.type != TOML_UNKNOWN && !as_integer(dy, y)) valid = false;
        }
    } else {
        valid = false;
    }
    if (x < 0 || x > 512 || y < 0 || y > 512) valid = false;
    if (!valid) {
        errors.set("window.padding must be an integer between 0 and 512");
        return;
    }
    cfg.window.padding.x = static_cast<int>(x);
    cfg.window.padding.y = static_cast<int>(y);
}

void apply_section(AppConfig& cfg, std::string_view section, toml_datum_t table,
                   bool& cursor_color_explicit, ValidationErrors& errors) {
    if (section == "general") {
        const toml_datum_t wd = find_key(table, "working_directory");
        if (wd.type != TOML_UNKNOWN) {
            if (const auto value = as_string(wd)) {
                cfg.main.working_directory = (lower(*value) == "none") ? std::string{} : std::string(*value);
            } else {
                errors.set("general.working_directory must be a string");
            }
        }
        const toml_datum_t cq = find_key(table, "confirm_quit");
        if (cq.type != TOML_UNKNOWN) {
            const auto value = as_string(cq);
            const auto mode = value ? parse_quit_confirmation(*value) : std::nullopt;
            if (!mode) errors.set("general.confirm_quit must be always, needed, or never");
            else cfg.window.confirm_quit = *mode;
        }
        return;
    }
    if (section == "env") {
        for (int i = 0; i < table.u.tab.size; ++i) {
            const std::string_view key(table.u.tab.key[i], static_cast<size_t>(table.u.tab.len[i]));
            const auto val = as_string(table.u.tab.value[i]);
            if (!val) {
                errors.set("env entries must be strings");
                return;
            }
            if (key == "TERM") cfg.main.term = *val;
            else cfg.main.set_env(key, *val);
        }
        return;
    }
    if (section == "terminal") {
        const toml_datum_t osc52_datum = find_key(table, "osc52");
        if (osc52_datum.type != TOML_UNKNOWN) {
            const auto value = as_string(osc52_datum);
            const auto mode = value ? parse_osc52_mode(*value) : std::nullopt;
            if (!mode) errors.set("terminal.osc52 must be Disabled, OnlyCopy, OnlyPaste, or CopyPaste");
            else cfg.main.osc52 = *mode;
        }
        const toml_datum_t shell = find_key(table, "shell");
        if (shell.type != TOML_UNKNOWN) {
            if (const auto s = as_string(shell)) {
                cfg.main.shell = *s;
                cfg.main.shell_args.clear();
            } else if (shell.type == TOML_TABLE) {
                const toml_datum_t prog = find_key(shell, "program");
                if (prog.type != TOML_UNKNOWN) {
                    if (const auto p = as_string(prog)) cfg.main.shell = *p;
                    else errors.set("terminal.shell.program must be a string");
                }
                const toml_datum_t args = find_key(shell, "args");
                if (args.type != TOML_UNKNOWN) {
                    if (args.type != TOML_ARRAY) {
                        errors.set("terminal.shell.args must be an array of strings");
                    } else {
                        std::vector<std::string> parsed_args;
                        parsed_args.reserve(static_cast<size_t>(args.u.arr.size));
                        bool ok = true;
                        for (int i = 0; i < args.u.arr.size; ++i) {
                            const auto elem = as_string(args.u.arr.elem[i]);
                            if (!elem) { ok = false; break; }
                            parsed_args.emplace_back(*elem);
                        }
                        if (!ok) errors.set("terminal.shell.args must be an array of strings");
                        else cfg.main.shell_args = std::move(parsed_args);
                    }
                }
            } else {
                errors.set("terminal.shell must be a string or table");
            }
        }
        return;
    }
    if (section == "main") {
        if (const auto value = as_string(find_key(table, "shell"))) {
            cfg.main.shell = *value;
            cfg.main.shell_args.clear();
        }
        if (const auto value = as_string(find_key(table, "term"))) cfg.main.term = *value;
        if (const auto value = as_string(find_key(table, "title"))) cfg.main.title = *value;
        if (const auto value = as_string(find_key(table, "app_id"))) cfg.main.app_id = *value;
        const toml_datum_t osc52_datum = find_key(table, "osc52");
        if (osc52_datum.type != TOML_UNKNOWN) {
            const auto value = as_string(osc52_datum);
            const auto mode = value ? parse_osc52_mode(*value) : std::nullopt;
            if (!mode) errors.set("main.osc52 must be disabled, copy-only, paste-only, or enabled");
            else cfg.main.osc52 = *mode;
        }
        // Legacy boolean spelling: true enables both directions.
        bool osc52_read = false;
        if (as_bool(find_key(table, "osc52_read"), osc52_read))
            cfg.main.osc52 = osc52_read ? Osc52Mode::Enabled : Osc52Mode::CopyOnly;
        return;
    }
    if (section == "font") {
        if (const auto value = as_string(find_key(table, "family"))) cfg.font.family = *value;
        const toml_datum_t normal = find_key(table, "normal");
        if (normal.type == TOML_TABLE) {
            if (const auto value = as_string(find_key(normal, "family"))) cfg.font.family = *value;
        }
        const toml_datum_t size_datum = find_key(table, "size");
        if (size_datum.type != TOML_UNKNOWN) {
            double size = 0;
            if (!as_number(size_datum, size) || !std::isfinite(size) || size < 1.0 || size > 200.0)
                errors.set("font.size must be a finite number between 1 and 200");
            else cfg.font.size = size;
        }
        const toml_datum_t dpi_datum = find_key(table, "dpi");
        if (dpi_datum.type != TOML_UNKNOWN) {
            int64_t dpi = 0;
            if (!as_integer(dpi_datum, dpi) || dpi <= 0 ||
                static_cast<uint64_t>(dpi) > std::numeric_limits<unsigned int>::max())
                errors.set("font.dpi must be a positive integer");
            else cfg.font.dpi = static_cast<unsigned int>(dpi);
        }
        const toml_datum_t aa_datum = find_key(table, "antialias");
        if (aa_datum.type != TOML_UNKNOWN) {
            if (const auto value = as_string(aa_datum)) {
                const std::string mode = lower(*value);
                if (mode == "grayscale") cfg.font.antialias = render::AntialiasMode::Grayscale;
                else if (mode == "subpixel" || mode == "lcd") cfg.font.antialias = render::AntialiasMode::SubpixelLcd;
                else if (mode == "none") cfg.font.antialias = render::AntialiasMode::None;
                else errors.set("font.antialias must be grayscale, subpixel, lcd, or none");
            } else {
                errors.set("font.antialias must be grayscale, subpixel, lcd, or none");
            }
        }
        return;
    }
    if (section == "cursor") {
        const toml_datum_t style_datum = find_key(table, "style");
        if (style_datum.type != TOML_UNKNOWN) {
            auto apply_shape = [&](std::string_view sv) {
                const std::string shape = lower(sv);
                if (shape == "block") cfg.cursor.style = core::CursorShape::Block;
                else if (shape == "beam") cfg.cursor.style = core::CursorShape::Beam;
                else if (shape == "underline") cfg.cursor.style = core::CursorShape::Underline;
                else errors.set("cursor.style must be block, beam, or underline");
            };
            if (const auto value = as_string(style_datum)) {
                apply_shape(*value);
            } else if (style_datum.type == TOML_TABLE) {
                const toml_datum_t shape_d = find_key(style_datum, "shape");
                if (shape_d.type != TOML_UNKNOWN) {
                    if (const auto sv = as_string(shape_d)) apply_shape(*sv);
                    else errors.set("cursor.style.shape must be block, beam, or underline");
                }
                const toml_datum_t blink_d = find_key(style_datum, "blinking");
                if (blink_d.type != TOML_UNKNOWN) {
                    bool b = false;
                    if (const auto bv = as_string(blink_d)) {
                        const std::string mode = lower(*bv);
                        if (mode == "never" || mode == "off") cfg.cursor.blink = false;
                        else if (mode == "on" || mode == "always") cfg.cursor.blink = true;
                        else errors.set("cursor.style.blinking must be never, off, on, or always");
                    } else if (as_bool(blink_d, b)) {
                        cfg.cursor.blink = b;
                    } else {
                        errors.set("cursor.style.blinking must be never, off, on, or always");
                    }
                }
            } else {
                errors.set("cursor.style must be block, beam, or underline");
            }
        }
        bool blink = false;
        if (as_bool(find_key(table, "blink"), blink)) cfg.cursor.blink = blink;
        if (const auto value = as_string(find_key(table, "color"))) {
            if (auto color = TomlLoader::parse_color(*value)) {
                cfg.cursor.color = *color;
                cursor_color_explicit = true;
            }
        }
        return;
    }
    if (section == "colors") {
        apply_colors(cfg, table, cursor_color_explicit, errors);
        return;
    }
    if (section == "window") {
        if (const auto value = as_string(find_key(table, "title"))) cfg.main.title = *value;
        const toml_datum_t cls = find_key(table, "class");
        if (cls.type != TOML_UNKNOWN) {
            if (const auto s = as_string(cls)) {
                cfg.main.app_id = *s;
            } else if (cls.type == TOML_TABLE) {
                const toml_datum_t gen = find_key(cls, "general");
                if (gen.type != TOML_UNKNOWN) {
                    if (const auto gs = as_string(gen)) cfg.main.app_id = *gs;
                    else errors.set("window.class.general must be a string");
                }
            } else {
                errors.set("window.class must be a string or table");
            }
        }
        const toml_datum_t datum = find_key(table, "decorations");
        if (datum.type != TOML_UNKNOWN) {
            const auto value = as_string(datum);
            const auto decorations = value ? parse_window_decorations(*value) : std::nullopt;
            if (!decorations) errors.set("window.decorations must be full, none, transparent, or buttonless");
            else cfg.window.decorations = *decorations;
        }
        const toml_datum_t cq = find_key(table, "confirm_quit");
        if (cq.type != TOML_UNKNOWN) {
            const auto value = as_string(cq);
            const auto mode = value ? parse_quit_confirmation(*value) : std::nullopt;
            if (!mode) errors.set("window.confirm_quit must be always, needed, or never");
            else cfg.window.confirm_quit = *mode;
        }
        const toml_datum_t opacity = find_key(table, "opacity");
        if (opacity.type != TOML_UNKNOWN) {
            double parsed = 0;
            if (!as_number(opacity, parsed) || !std::isfinite(parsed) || parsed < 0.0 || parsed > 1.0)
                errors.set("window.opacity must be a finite number between 0 and 1");
            else cfg.colors.alpha = parsed;
        }
        apply_padding(cfg, table, errors);
        const toml_datum_t dims = find_key(table, "dimensions");
        if (dims.type != TOML_UNKNOWN) {
            if (dims.type != TOML_TABLE) {
                errors.set("window.dimensions must be a table");
            } else {
                const toml_datum_t cols_d = find_key(dims, "columns");
                const toml_datum_t lines_d = find_key(dims, "lines");
                if (cols_d.type != TOML_UNKNOWN) {
                    int64_t cols = 0;
                    if (!as_integer(cols_d, cols) || cols < 0 || cols > 1000)
                        errors.set("window.dimensions.columns must be an integer between 0 and 1000");
                    else cfg.window.columns = static_cast<size_t>(cols);
                }
                if (lines_d.type != TOML_UNKNOWN) {
                    int64_t lines = 0;
                    if (!as_integer(lines_d, lines) || lines < 0 || lines > 1000)
                        errors.set("window.dimensions.lines must be an integer between 0 and 1000");
                    else cfg.window.lines = static_cast<size_t>(lines);
                }
            }
        }
        const toml_datum_t pos_d = find_key(table, "position");
        if (pos_d.type != TOML_UNKNOWN) {
            if (const auto s = as_string(pos_d)) {
                if (lower(*s) == "none") cfg.window.position.reset();
                else errors.set("window.position must be 'None' or a table { x = int, y = int }");
            } else if (pos_d.type == TOML_TABLE) {
                const toml_datum_t x_d = find_key(pos_d, "x");
                const toml_datum_t y_d = find_key(pos_d, "y");
                int64_t px = cfg.window.position ? cfg.window.position->x : 0;
                int64_t py = cfg.window.position ? cfg.window.position->y : 0;
                bool valid = !(x_d.type == TOML_UNKNOWN && y_d.type == TOML_UNKNOWN);
                if (x_d.type != TOML_UNKNOWN && !as_integer(x_d, px)) valid = false;
                if (y_d.type != TOML_UNKNOWN && !as_integer(y_d, py)) valid = false;
                if (px < -32768 || px > 32768 || py < -32768 || py > 32768) valid = false;
                if (!valid) {
                    errors.set("window.position must be 'None' or a table { x = int, y = int }");
                } else {
                    cfg.window.position = platform::WindowPosition{
                        static_cast<int>(px), static_cast<int>(py)
                    };
                }
            } else {
                errors.set("window.position must be 'None' or a table { x = int, y = int }");
            }
        }
        const toml_datum_t startup_d = find_key(table, "startup_mode");
        if (startup_d.type != TOML_UNKNOWN) {
            const auto value = as_string(startup_d);
            const auto mode = value ? parse_window_startup_mode(*value) : std::nullopt;
            if (!mode) {
                errors.set("window.startup_mode must be Windowed, Maximized, Fullscreen, or SimpleFullscreen");
            } else {
                cfg.window.startup_mode = *mode;
            }
        }
    }
}

void apply_tables(AppConfig& cfg, toml_datum_t root, bool& cursor_color_explicit,
                  ValidationErrors& errors) {
    constexpr std::string_view sections[] = {
        "general", "env", "main", "terminal", "font", "cursor", "colors", "window"
    };
    for (const auto section : sections) {
        const toml_datum_t table = find_key(root, section);
        if (table.type == TOML_TABLE)
            apply_section(cfg, section, table, cursor_color_explicit, errors);
    }
}

void apply_platform(AppConfig& cfg, toml_datum_t root, std::string_view platform,
                    ValidationErrors& errors) {
    // Accept both [platform.macos.font] and legacy [macos.font] forms.
    toml_datum_t platform_root = find_key(root, "platform");
    const bool prefixed = platform_root.type == TOML_TABLE;
    if (!prefixed) platform_root = root;
    const toml_datum_t os = find_key(platform_root, platform);
    if (os.type != TOML_TABLE) return;

    bool platform_cursor_explicit = false;
    apply_tables(cfg, os, platform_cursor_explicit, errors);
}

std::expected<std::shared_ptr<AppConfig>, ConfigError> load(
    std::string_view text,
    std::string_view platform_override
) {
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(ConfigError{.message = "Configuration file is too large"});
    }
    const std::string source(text);
    ParseResult parsed(toml_parse(source.c_str(), static_cast<int>(source.size())));
    if (!parsed.ok()) return std::unexpected(parse_error(parsed.error()));

    auto cfg = std::make_shared<AppConfig>();
    bool cursor_color_explicit = false;
    ValidationErrors errors;
    apply_tables(*cfg, parsed.root(), cursor_color_explicit, errors);
    apply_platform(*cfg, parsed.root(), target_platform(platform_override), errors);
    if (!errors.first_error.empty()) {
        return std::unexpected(ConfigError{.message = std::move(errors.first_error)});
    }
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
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec)
        return std::unexpected(ConfigError{
            .message = "Failed to open configuration file: " + std::string(file_path)});
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return std::unexpected(ConfigError{
            .message = "Failed to open configuration file: " + std::string(file_path)});
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (file.bad())
        return std::unexpected(ConfigError{
            .message = "Failed to read configuration file: " + std::string(file_path)});
    return load(buffer.str(), platform_override);
}

} // namespace gritty::config
