#include "config/toml_loader.hpp"
#include <toml++/toml.hpp>
#include <fstream>
#include <sstream>
#include <charconv>
#include <cmath>

namespace gritty::config {

namespace {

void deep_merge(toml::table& dest, const toml::table& src) {
    for (const auto& [k, v] : src) {
        if (v.is_table() && dest[k].is_table()) {
            deep_merge(*dest[k].as_table(), *v.as_table());
        } else {
            dest.insert_or_assign(k, v);
        }
    }
}

} // namespace

std::optional<uint32_t> TomlLoader::parse_color(std::string_view str) noexcept {
    if (str.empty()) return std::nullopt;

    if (str.starts_with('#')) {
        str.remove_prefix(1);
    } else if (str.starts_with("0x") || str.starts_with("0X")) {
        str.remove_prefix(2);
    }

    if (str.size() == 6) { // RRGGBB
        uint32_t val = 0;
        auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), val, 16);
        if (ec == std::errc() && ptr == str.data() + str.size()) {
            return 0xFF000000 | val;
        }
    } else if (str.size() == 8) { // AARRGGBB
        uint32_t val = 0;
        auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), val, 16);
        if (ec == std::errc() && ptr == str.data() + str.size()) {
            return val;
        }
    }

    return std::nullopt;
}

std::expected<std::shared_ptr<AppConfig>, ConfigError> TomlLoader::load_from_string(
    std::string_view toml_str,
    std::string_view platform_override
) {
    toml::table tbl;
    try {
        tbl = toml::parse(toml_str);
    } catch (const toml::parse_error& err) {
        return std::unexpected(ConfigError{
            .message = std::string(err.description()),
            .line = static_cast<int>(err.source().begin.line),
            .column = static_cast<int>(err.source().begin.column)
        });
    }

    std::string os_name = std::string(platform_override);
    if (os_name.empty()) {
#if defined(PLATFORM_MACOS)
        os_name = "macos";
#elif defined(PLATFORM_LINUX)
        os_name = "linux";
#endif
    }

    // Merge [platform.<os>] table over root
    if (auto* platform_tbl = tbl["platform"].as_table()) {
        if (auto* os_tbl = (*platform_tbl)[os_name].as_table()) {
            deep_merge(tbl, *os_tbl);
        }
    }

    auto cfg = std::make_shared<AppConfig>();

    // [main]
    if (auto* m = tbl["main"].as_table()) {
        if (auto s = (*m)["shell"].value<std::string>()) cfg->main.shell = *s;
        if (auto t = (*m)["term"].value<std::string>()) cfg->main.term = *t;
        if (auto title = (*m)["title"].value<std::string>()) cfg->main.title = *title;
    }

    // [font]
    if (auto* f = tbl["font"].as_table()) {
        if (auto fam = (*f)["family"].value<std::string>()) cfg->font.family = *fam;
        if (auto sz = (*f)["size"].value<double>()) {
            if (*sz > 0) cfg->font.size = *sz;
        }
        if (auto dpi = (*f)["dpi"].value<int64_t>()) {
            if (*dpi > 0) cfg->font.dpi = static_cast<unsigned int>(*dpi);
        }
        if (auto aa = (*f)["antialias"].value<std::string>()) {
            if (*aa == "grayscale") cfg->font.antialias = render::AntialiasMode::Grayscale;
            else if (*aa == "subpixel" || *aa == "lcd") cfg->font.antialias = render::AntialiasMode::SubpixelLcd;
            else if (*aa == "none") cfg->font.antialias = render::AntialiasMode::None;
        }
    }

    // [cursor]
    if (auto* c = tbl["cursor"].as_table()) {
        if (auto style = (*c)["style"].value<std::string>()) {
            if (*style == "block") cfg->cursor.style = core::CursorShape::Block;
            else if (*style == "beam") cfg->cursor.style = core::CursorShape::Beam;
            else if (*style == "underline") cfg->cursor.style = core::CursorShape::Underline;
        }
        if (auto blink = (*c)["blink"].value<bool>()) cfg->cursor.blink = *blink;
        if (auto color_str = (*c)["color"].value<std::string>()) {
            if (auto col = parse_color(*color_str)) cfg->cursor.color = *col;
        }
    }

    // [colors]
    if (auto* clr = tbl["colors"].as_table()) {
        if (const auto* node = clr->get("alpha")) {
            const auto alpha = node->value<double>();
            if (!alpha || !std::isfinite(*alpha) || *alpha < 0.0 || *alpha > 1.0)
                return std::unexpected(ConfigError{.message = "colors.alpha must be a finite number between 0 and 1"});
            cfg->colors.alpha = *alpha;
        }
        if (auto fg = (*clr)["foreground"].value<std::string>()) {
            if (auto val = parse_color(*fg)) cfg->colors.foreground = *val;
        }
        if (auto bg = (*clr)["background"].value<std::string>()) {
            if (auto val = parse_color(*bg)) cfg->colors.background = *val;
        }
        if (auto cur = (*clr)["cursor"].value<std::string>()) {
            if (auto val = parse_color(*cur)) cfg->colors.cursor = *val;
        }
        // Palette 0..15
        for (int i = 0; i < 8; ++i) {
            std::string reg_key = "regular" + std::to_string(i);
            std::string bgt_key = "bright" + std::to_string(i);
            if (auto reg = (*clr)[reg_key].value<std::string>()) {
                if (auto val = parse_color(*reg)) {
                    cfg->colors.palette[i] = *val & 0x00FFFFFF;
                    cfg->colors.palette_customized = true;
                }
            }
            if (auto bgt = (*clr)[bgt_key].value<std::string>()) {
                if (auto val = parse_color(*bgt)) {
                    cfg->colors.palette[i + 8] = *val & 0x00FFFFFF;
                    cfg->colors.palette_customized = true;
                }
            }
        }
    }

    // [scrollback]
    if (auto* sc = tbl["scrollback"].as_table()) {
        if (auto lines = (*sc)["lines"].value<int64_t>()) {
            if (*lines >= 0) cfg->scrollback.lines = static_cast<size_t>(*lines);
        }
        if (auto mul = (*sc)["multiplier"].value<int64_t>()) {
            if (*mul > 0) cfg->scrollback.multiplier = static_cast<int>(*mul);
        }
    }

    return cfg;
}

std::expected<std::shared_ptr<AppConfig>, ConfigError> TomlLoader::load_from_file(
    std::string_view file_path,
    std::string_view platform_override
) {
    std::string path_str(file_path);
    std::ifstream file(path_str);
    if (!file.is_open()) {
        return std::unexpected(ConfigError{
            .message = "Failed to open configuration file: " + std::string(file_path)
        });
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return load_from_string(buffer.str(), platform_override);
}

} // namespace gritty::config
