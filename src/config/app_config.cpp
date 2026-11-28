#include "config/app_config.hpp"

#include "config/parse_helpers.hpp"
#include "config/toml_loader.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

namespace gritty::config {

namespace {

using detail::equals_ignore_case;
using detail::lower_into;
using detail::parse_double;
using detail::parse_integer;

std::string unquote(std::string_view text) {
    text = detail::trim_ascii(text);
    if (text.size() >= 2 &&
        ((text.front() == '"' && text.back() == '"') ||
         (text.front() == '\'' && text.back() == '\'')))
        return std::string(text.substr(1, text.size() - 2));
    return std::string(text);
}

bool parse_bool(std::string_view text, bool& out) noexcept {
    const std::string_view value = detail::trim_ascii(text);
    if (value == "true" || value == "1") {
        out = true;
        return true;
    }
    if (value == "false" || value == "0") {
        out = false;
        return true;
    }
    return false;
}

std::unexpected<std::string> invalid(std::string_view key, std::string_view value) {
    return std::unexpected(std::string("Invalid option override: ") + std::string(key) +
                           "=" + std::string(value));
}

} // namespace

std::optional<Osc52Mode> parse_osc52_mode(std::string_view value) noexcept {
    const auto text = detail::trim_ascii(value);
    if (equals_ignore_case(text, "disabled")) return Osc52Mode::Disabled;
    if (equals_ignore_case(text, "copy-only") || equals_ignore_case(text, "copyonly") ||
        equals_ignore_case(text, "onlycopy")) return Osc52Mode::CopyOnly;
    if (equals_ignore_case(text, "paste-only") || equals_ignore_case(text, "pasteonly") ||
        equals_ignore_case(text, "onlypaste")) return Osc52Mode::PasteOnly;
    if (equals_ignore_case(text, "enabled") || equals_ignore_case(text, "copypaste"))
        return Osc52Mode::Enabled;
    return std::nullopt;
}

std::optional<QuitConfirmation> parse_quit_confirmation(std::string_view value) noexcept {
    const auto text = detail::trim_ascii(value);
    if (equals_ignore_case(text, "always")) return QuitConfirmation::Always;
    if (equals_ignore_case(text, "needed")) return QuitConfirmation::Needed;
    if (equals_ignore_case(text, "never")) return QuitConfirmation::Never;
    return std::nullopt;
}

void MainConfig::set_env(std::string_view key, std::string_view value) {
    if (key.empty()) return;
    const std::string entry = std::string(key) + "=" + std::string(value);
    for (auto& existing : env) {
        if (existing.size() > key.size() &&
            existing.compare(0, key.size(), key) == 0 &&
            existing[key.size()] == '=') {
            existing = entry;
            return;
        }
    }
    env.push_back(entry);
}

std::optional<platform::WindowDecorations> parse_window_decorations(
    std::string_view value
) noexcept {
    return platform::parse_window_decorations(detail::trim_ascii(value));
}

std::optional<platform::WindowStartupMode> parse_window_startup_mode(
    std::string_view value
) noexcept {
    return platform::parse_window_startup_mode(detail::trim_ascii(value));
}

AppConfig::AppConfig() {
#if defined(PLATFORM_MACOS)
    font.family = "Menlo";
#else
    font.family = "monospace";
#endif
}

uint32_t AppConfig::effective_background_argb() const noexcept {
    if (window.decorations == platform::WindowDecorations::Full)
        return (colors.background & 0x00ffffffu) | 0xff000000u;
    return colors.background_argb();
}

uint32_t ColorsConfig::background_argb() const noexcept {
    const double opacity = std::isfinite(alpha) ? std::clamp(alpha, 0.0, 1.0) : 1.0;
    const auto effective_alpha = static_cast<uint32_t>(std::lround((background >> 24) * opacity));
    return (background & 0xffffff) | (effective_alpha << 24);
}

std::expected<void, std::string> AppConfig::apply_override(
    std::string_view expr,
    size_t* cols_out,
    size_t* rows_out
) {
    const size_t equals = expr.find('=');
    if (equals == std::string_view::npos || equals == 0)
        return invalid(expr, {});

    const std::string key = std::string(detail::trim_ascii(expr.substr(0, equals)));
    const std::string value = unquote(expr.substr(equals + 1));
    std::string lowered;
    lower_into(lowered, key);

    if (lowered == "main.shell" || lowered == "terminal.shell" ||
        lowered == "terminal.shell.program") {
        main.shell = value;
        if (lowered != "terminal.shell.program") main.shell_args.clear();
        return {};
    }
    if (lowered == "general.working_directory") {
        std::string wd_lower;
        lower_into(wd_lower, value);
        main.working_directory = (wd_lower == "none") ? std::string{} : value;
        return {};
    }
    if (lowered.starts_with("env.") && key.size() > 4) {
        const std::string env_key = key.substr(4);
        if (env_key == "TERM") main.term = value;
        else main.set_env(env_key, value);
        return {};
    }
    if (lowered == "main.term") { main.term = value; return {}; }
    if (lowered == "main.title" || lowered == "window.title") { main.title = value; return {}; }
    if (lowered == "main.app_id" || lowered == "window.class" ||
        lowered == "window.class.general") { main.app_id = value; return {}; }
    if (lowered == "font.family" || lowered == "font.normal.family") { font.family = value; return {}; }

    bool flag = false;
    if (lowered == "main.osc52" || lowered == "terminal.osc52") {
        const auto mode = parse_osc52_mode(value);
        if (!mode) return invalid(key, value);
        main.osc52 = *mode;
        return {};
    }
    if (lowered == "main.osc52_read") {
        // Legacy boolean spelling: true enables both directions.
        if (!parse_bool(value, flag)) return invalid(key, value);
        main.osc52 = flag ? Osc52Mode::Enabled : Osc52Mode::CopyOnly;
        return {};
    }

    double real = 0.0;
    int64_t integer = 0;
    std::string mode;

    if (lowered == "font.size") {
        if (!parse_double(value, real) || real < 1.0 || real > 200.0) return invalid(key, value);
        font.size = real;
        return {};
    }
    if (lowered == "font.dpi") {
        if (!parse_integer(value, integer) || integer <= 0 ||
            static_cast<uint64_t>(integer) > std::numeric_limits<unsigned int>::max())
            return invalid(key, value);
        font.dpi = static_cast<unsigned int>(integer);
        return {};
    }
    if (lowered == "font.antialias") {
        lower_into(mode, value);
        if (mode == "grayscale") font.antialias = render::AntialiasMode::Grayscale;
        else if (mode == "subpixel" || mode == "lcd") font.antialias = render::AntialiasMode::SubpixelLcd;
        else if (mode == "none") font.antialias = render::AntialiasMode::None;
        else return invalid(key, value);
        return {};
    }

    if (lowered == "cursor.style" || lowered == "cursor.style.shape") {
        lower_into(mode, value);
        if (mode == "block") cursor.style = core::CursorShape::Block;
        else if (mode == "beam") cursor.style = core::CursorShape::Beam;
        else if (mode == "underline") cursor.style = core::CursorShape::Underline;
        else return invalid(key, value);
        return {};
    }
    if (lowered == "cursor.blink") {
        if (!parse_bool(value, flag)) return invalid(key, value);
        cursor.blink = flag;
        return {};
    }
    if (lowered == "cursor.style.blinking") {
        lower_into(mode, value);
        if (mode == "never" || mode == "off" || mode == "false" || mode == "0") cursor.blink = false;
        else if (mode == "on" || mode == "always" || mode == "true" || mode == "1") cursor.blink = true;
        else return invalid(key, value);
        return {};
    }
    if (lowered == "cursor.color") {
        const auto color = TomlLoader::parse_color(value);
        if (!color) return invalid(key, value);
        cursor.color = *color;
        return {};
    }

    if (lowered == "colors.alpha" || lowered == "window.opacity") {
        if (!parse_double(value, real) || real < 0.0 || real > 1.0) return invalid(key, value);
        colors.alpha = real;
        return {};
    }
    if (lowered == "colors.foreground" || lowered == "colors.primary.foreground" ||
        lowered == "colors.background" || lowered == "colors.primary.background" ||
        lowered == "colors.cursor" || lowered == "colors.cursor.cursor") {
        const auto color = TomlLoader::parse_color(value);
        if (!color) return invalid(key, value);
        if (lowered == "colors.foreground" || lowered == "colors.primary.foreground")
            colors.foreground = *color;
        else if (lowered == "colors.background" || lowered == "colors.primary.background")
            colors.background = *color;
        else {
            colors.cursor = *color;
            cursor.color = *color;
        }
        return {};
    }
    if (lowered.starts_with("colors.normal.") || lowered.starts_with("colors.bright.")) {
        const bool bright = lowered.starts_with("colors.bright.");
        const std::string_view name = std::string_view(lowered).substr(
            bright ? std::string_view("colors.bright.").size()
                   : std::string_view("colors.normal.").size());
        constexpr std::array<std::string_view, 8> names{
            "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"
        };
        int index = -1;
        for (int i = 0; i < 8; ++i) {
            if (name == names[static_cast<size_t>(i)]) { index = i; break; }
        }
        if (index < 0) return invalid(key, value);
        const auto color = TomlLoader::parse_color(value);
        if (!color) return invalid(key, value);
        colors.palette[static_cast<size_t>(bright ? index + 8 : index)] = *color & 0x00ffffff;
        colors.palette_customized = true;
        return {};
    }
    if (lowered.starts_with("colors.regular") || lowered.starts_with("colors.bright")) {
        const bool regular = lowered.starts_with("colors.regular");
        const std::string_view number = std::string_view(lowered).substr(
            regular ? std::string_view("colors.regular").size()
                    : std::string_view("colors.bright").size());
        int index = -1;
        const auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), index);
        if (ec != std::errc{} || end != number.data() + number.size() || index < 0 || index > 7)
            return invalid(key, value);
        const auto color = TomlLoader::parse_color(value);
        if (!color) return invalid(key, value);
        colors.palette[static_cast<size_t>(regular ? index : index + 8)] = *color & 0x00ffffff;
        colors.palette_customized = true;
        return {};
    }

    if (lowered == "window.decorations") {
        const auto decorations = parse_window_decorations(value);
        if (!decorations) return invalid(key, value);
        window.decorations = *decorations;
        return {};
    }
    if (lowered == "window.startup_mode") {
        const auto startup = parse_window_startup_mode(value);
        if (!startup) return invalid(key, value);
        window.startup_mode = *startup;
        return {};
    }
    if (lowered == "window.position") {
        lower_into(mode, value);
        if (mode == "none") {
            window.position.reset();
            return {};
        }
        return invalid(key, value);
    }
    if (lowered == "window.position.x" || lowered == "window.position.y") {
        if (!parse_integer(value, integer) || integer < -32768 || integer > 32768)
            return invalid(key, value);
        if (!window.position) window.position = platform::WindowPosition{};
        if (lowered == "window.position.x") window.position->x = static_cast<int>(integer);
        else window.position->y = static_cast<int>(integer);
        return {};
    }
    if (lowered == "window.confirm_quit" || lowered == "general.confirm_quit") {
        const auto confirm = parse_quit_confirmation(value);
        if (!confirm) return invalid(key, value);
        window.confirm_quit = *confirm;
        return {};
    }
    if (lowered == "window.padding" || lowered == "window.padding.x" ||
        lowered == "window.padding.y") {
        int64_t amount = 0;
        if (!parse_integer(value, amount) || amount < 0 || amount > 512)
            return invalid(key, value);
        if (lowered == "window.padding" || lowered == "window.padding.x")
            window.padding.x = static_cast<int>(amount);
        if (lowered == "window.padding" || lowered == "window.padding.y")
            window.padding.y = static_cast<int>(amount);
        return {};
    }

    if (lowered == "window.dimensions.columns" || lowered == "window.dimensions.lines") {
        if (!parse_integer(value, integer) || integer < 0 || integer > 1000)
            return invalid(key, value);
        if (lowered == "window.dimensions.columns") {
            window.columns = static_cast<size_t>(integer);
            if (cols_out) *cols_out = static_cast<size_t>(integer);
        } else {
            window.lines = static_cast<size_t>(integer);
            if (rows_out) *rows_out = static_cast<size_t>(integer);
        }
        return {};
    }

    return invalid(key, value);
}

} // namespace gritty::config
