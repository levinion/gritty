#include "config/app_config.hpp"

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

std::string_view trim(std::string_view text) noexcept {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

std::string unquote(std::string_view text) {
    text = trim(text);
    if (text.size() >= 2 &&
        ((text.front() == '"' && text.back() == '"') ||
         (text.front() == '\'' && text.back() == '\'')))
        return std::string(text.substr(1, text.size() - 2));
    return std::string(text);
}

void lower_into(std::string& out, std::string_view text) {
    out.clear();
    out.reserve(text.size());
    for (const char c : text)
        out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
}

bool parse_integer(std::string_view text, int64_t& out) noexcept {
    const char* first = text.data();
    const char* last = first + text.size();
    if (first != last && *first == '+') ++first;
    if (first == last) return false;
    const auto [end, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && end == last;
}

bool parse_double(std::string_view text, double& out) noexcept {
    const char* first = text.data();
    const char* last = first + text.size();
    if (first != last && *first == '+') ++first;
    if (first == last) return false;
    const auto [end, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && end == last && std::isfinite(out);
}

bool parse_bool(std::string_view text, bool& out) noexcept {
    const std::string value = std::string(trim(text));
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
    const auto text = trim(value);
    auto equals = [text](std::string_view expected) noexcept {
        if (text.size() != expected.size()) return false;
        for (size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            const char lowered = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            if (lowered != expected[i]) return false;
        }
        return true;
    };
    if (equals("disabled")) return Osc52Mode::Disabled;
    if (equals("copy-only") || equals("copyonly")) return Osc52Mode::CopyOnly;
    if (equals("paste-only") || equals("pasteonly")) return Osc52Mode::PasteOnly;
    if (equals("enabled")) return Osc52Mode::Enabled;
    return std::nullopt;
}

std::optional<platform::WindowDecorations> parse_window_decorations(
    std::string_view value
) noexcept {
    return platform::parse_window_decorations(value);
}

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

std::expected<void, std::string> AppConfig::apply_override(
    std::string_view expr,
    size_t* cols_out,
    size_t* rows_out
) {
    const size_t equals = expr.find('=');
    if (equals == std::string_view::npos || equals == 0)
        return invalid(expr, {});

    const std::string key = std::string(trim(expr.substr(0, equals)));
    const std::string value = unquote(expr.substr(equals + 1));
    std::string lowered;
    lower_into(lowered, key);

    if (lowered == "main.shell") { main.shell = value; return {}; }
    if (lowered == "main.term") { main.term = value; return {}; }
    if (lowered == "main.title") { main.title = value; return {}; }
    if (lowered == "main.app_id") { main.app_id = value; return {}; }
    if (lowered == "font.family") { font.family = value; return {}; }

    bool flag = false;
    if (lowered == "main.osc52") {
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

    if (lowered == "cursor.style") {
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
    if (lowered == "cursor.color") {
        const auto color = TomlLoader::parse_color(value);
        if (!color) return invalid(key, value);
        cursor.color = *color;
        return {};
    }

    if (lowered == "colors.alpha") {
        if (!parse_double(value, real) || real < 0.0 || real > 1.0) return invalid(key, value);
        colors.alpha = real;
        return {};
    }
    if (lowered == "colors.foreground" || lowered == "colors.background" ||
        lowered == "colors.cursor") {
        const auto color = TomlLoader::parse_color(value);
        if (!color) return invalid(key, value);
        if (lowered == "colors.foreground") colors.foreground = *color;
        else if (lowered == "colors.background") colors.background = *color;
        else colors.cursor = *color;
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

    if (lowered == "window.dimensions.columns" || lowered == "window.dimensions.lines") {
        if (!parse_integer(value, integer) || integer < 1 || integer > 1000)
            return invalid(key, value);
        size_t* out = lowered == "window.dimensions.columns" ? cols_out : rows_out;
        if (out) *out = static_cast<size_t>(integer);
        return {};
    }

    return invalid(key, value);
}

ConfigStore::ConfigStore() : config_(std::make_shared<AppConfig>()) {}

ConfigStore::ConfigStore(std::shared_ptr<const AppConfig> initial)
    : config_(std::move(initial)) {}

} // namespace gritty::config
