#include "config/ini_loader.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace gritty::config {

namespace {

namespace fs = std::filesystem;

constexpr int kMaxIncludeDepth = 8;

std::string_view trim(std::string_view text) noexcept {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

char lower_ascii(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lowercase(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) out.push_back(lower_ascii(c));
    return out;
}

bool equals_ascii_case(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i)
        if (lower_ascii(lhs[i]) != lower_ascii(rhs[i])) return false;
    return true;
}

enum class SectionKind : uint8_t {
    None,
    Main,
    Font,
    Cursor,
    Colors,
    Scrollback,
};

struct Section {
    SectionKind kind{SectionKind::None};
    std::string platform;
};

enum class Pass : uint8_t { Base, Platform };

struct Context {
    std::shared_ptr<AppConfig> config;
    std::string target_platform;
    Pass pass{Pass::Base};
    int depth{0};
    fs::path base_dir;
    std::vector<std::string> include_stack;
};

[[nodiscard]] ConfigError error(std::string message, int line, int column) {
    return ConfigError{std::move(message), line, column};
}

SectionKind section_kind(std::string_view name) noexcept {
    if (name == "main") return SectionKind::Main;
    if (name == "font") return SectionKind::Font;
    if (name == "cursor") return SectionKind::Cursor;
    if (name == "colors") return SectionKind::Colors;
    if (name == "scrollback") return SectionKind::Scrollback;
    return SectionKind::None;
}

[[nodiscard]] std::optional<ConfigError> parse_section(
    std::string_view line,
    int line_number,
    Section& section
) {
    const size_t close = line.find(']');
    if (close == std::string_view::npos)
        return error("unterminated section header", line_number, 1);
    if (!trim(line.substr(close + 1)).empty())
        return error("unexpected characters after section header", line_number,
                     static_cast<int>(close + 2));

    const std::string_view name = trim(line.substr(1, close - 1));
    if (name.empty()) return error("empty section header", line_number, 2);

    const size_t first_dot = name.find('.');
    const std::string_view first = name.substr(0, first_dot);

    if (equals_ascii_case(first, "platform")) {
        if (first_dot == std::string_view::npos)
            return error("platform section requires an OS name", line_number, 2);
        const std::string_view rest = name.substr(first_dot + 1);
        const size_t second_dot = rest.find('.');
        section.platform = lowercase(rest.substr(0, second_dot));
        if (second_dot == std::string_view::npos) {
            section.kind = SectionKind::None;
            return std::nullopt;
        }
        section.kind = section_kind(lowercase(rest.substr(second_dot + 1)));
        return std::nullopt;
    }

    if (equals_ascii_case(first, "macos") || equals_ascii_case(first, "linux")) {
        section.platform = lowercase(first);
        if (first_dot == std::string_view::npos) {
            section.kind = SectionKind::None;
            return std::nullopt;
        }
        section.kind = section_kind(lowercase(name.substr(first_dot + 1)));
        return std::nullopt;
    }

    section.platform.clear();
    section.kind = section_kind(lowercase(name));
    return std::nullopt;
}

[[nodiscard]] std::optional<ConfigError> decode_value(
    std::string_view raw,
    int line_number,
    int column,
    std::string& out
) {
    const std::string_view value = trim(raw);
    if (value.size() < 2 || (value.front() != '"' && value.front() != '\'')) {
        out.assign(value);
        return std::nullopt;
    }
    const char quote = value.front();
    if (value.back() != quote)
        return error("unterminated quoted value", line_number, column);

    const std::string_view body = value.substr(1, value.size() - 2);
    if (quote == '\'') {
        out.assign(body);
        return std::nullopt;
    }

    out.clear();
    out.reserve(body.size());
    for (size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (++i >= body.size())
            return error("invalid escape at end of value", line_number, column);
        switch (body[i]) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (i + 4 >= body.size())
                    return error("incomplete unicode escape", line_number, column);
                uint32_t cp = 0;
                for (int n = 0; n < 4; ++n) {
                    const char hex = body[++i];
                    uint32_t digit = 0;
                    if (hex >= '0' && hex <= '9') digit = static_cast<uint32_t>(hex - '0');
                    else if (hex >= 'a' && hex <= 'f') digit = static_cast<uint32_t>(hex - 'a' + 10);
                    else if (hex >= 'A' && hex <= 'F') digit = static_cast<uint32_t>(hex - 'A' + 10);
                    else return error("invalid unicode escape", line_number, column);
                    cp = (cp << 4) | digit;
                }
                if (cp <= 0x7f) out.push_back(static_cast<char>(cp));
                else if (cp <= 0x7ff) {
                    out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
                } else {
                    out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
                }
                break;
            }
            default:
                return error("unknown escape sequence", line_number, column);
        }
    }
    return std::nullopt;
}

std::string_view strip_separators(std::string_view text, std::string& storage) {
    storage.clear();
    storage.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '_') {
            if (i == 0 || i + 1 == text.size() ||
                !std::isdigit(static_cast<unsigned char>(text[i - 1])) ||
                !std::isdigit(static_cast<unsigned char>(text[i + 1])))
                return {};
            continue;
        }
        storage.push_back(text[i]);
    }
    return storage;
}

bool parse_integer(std::string_view text, int64_t& out) noexcept {
    std::string storage;
    const std::string_view cleaned = strip_separators(trim(text), storage);
    if (cleaned.empty()) return false;
    const char* first = cleaned.data();
    const char* last = first + cleaned.size();
    if (*first == '+') ++first;
    if (first == last) return false;
    const auto [end, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && end == last;
}

bool parse_double(std::string_view text, double& out) noexcept {
    std::string storage;
    const std::string_view cleaned = strip_separators(trim(text), storage);
    if (cleaned.empty()) return false;
    const char* first = cleaned.data();
    const char* last = first + cleaned.size();
    if (*first == '+') ++first;
    if (first == last) return false;
    const auto [end, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && end == last && std::isfinite(out);
}

bool parse_bool(std::string_view text, bool& out) noexcept {
    const std::string value = lowercase(trim(text));
    if (value == "true" || value == "yes" || value == "1") {
        out = true;
        return true;
    }
    if (value == "false" || value == "no" || value == "0") {
        out = false;
        return true;
    }
    return false;
}

bool apply_field(Context& context, Section section, std::string_view key,
                 const std::string& value, int line_number, ConfigError& field_error) {
    if (key == "include" || section.kind == SectionKind::None) return true;
    if (context.pass == Pass::Base && !section.platform.empty()) return true;
    if (context.pass == Pass::Platform && section.platform != context.target_platform) return true;

    AppConfig& cfg = *context.config;
    const bool bad_alpha = [&] {
        switch (section.kind) {
            case SectionKind::Main:
                if (key == "shell") cfg.main.shell = value;
                else if (key == "term") cfg.main.term = value;
                else if (key == "title") cfg.main.title = value;
                return false;

            case SectionKind::Font: {
                if (key == "family") cfg.font.family = value;
                else if (key == "size") {
                    double parsed = 0;
                    if (parse_double(value, parsed) && parsed > 0) cfg.font.size = parsed;
                } else if (key == "dpi") {
                    int64_t parsed = 0;
                    if (parse_integer(value, parsed) && parsed > 0 &&
                        parsed <= std::numeric_limits<unsigned int>::max())
                        cfg.font.dpi = static_cast<unsigned int>(parsed);
                } else if (key == "antialias") {
                    const std::string mode = lowercase(value);
                    if (mode == "grayscale") cfg.font.antialias = render::AntialiasMode::Grayscale;
                    else if (mode == "subpixel" || mode == "lcd") cfg.font.antialias = render::AntialiasMode::SubpixelLcd;
                    else if (mode == "none") cfg.font.antialias = render::AntialiasMode::None;
                }
                return false;
            }

            case SectionKind::Cursor: {
                if (key == "style") {
                    const std::string style = lowercase(value);
                    if (style == "block") cfg.cursor.style = core::CursorShape::Block;
                    else if (style == "beam") cfg.cursor.style = core::CursorShape::Beam;
                    else if (style == "underline") cfg.cursor.style = core::CursorShape::Underline;
                } else if (key == "blink") {
                    bool parsed = false;
                    if (parse_bool(value, parsed)) cfg.cursor.blink = parsed;
                } else if (key == "color") {
                    if (auto color = IniLoader::parse_color(value)) cfg.cursor.color = *color;
                }
                return false;
            }

            case SectionKind::Colors: {
                if (key == "alpha") {
                    double parsed = 0;
                    if (!parse_double(value, parsed) || parsed < 0.0 || parsed > 1.0) {
                        field_error = error("colors.alpha must be a finite number between 0 and 1",
                                            line_number, 1);
                        return true;
                    }
                    cfg.colors.alpha = parsed;
                    return false;
                }
                if (key == "foreground") {
                    if (auto color = IniLoader::parse_color(value)) cfg.colors.foreground = *color;
                } else if (key == "background") {
                    if (auto color = IniLoader::parse_color(value)) cfg.colors.background = *color;
                } else if (key == "cursor") {
                    if (auto color = IniLoader::parse_color(value)) cfg.colors.cursor = *color;
                } else if (key == "selection_fg") {
                    cfg.colors.selection_fg = IniLoader::parse_color(value);
                } else if (key == "selection_bg") {
                    cfg.colors.selection_bg = IniLoader::parse_color(value);
                } else if (key.starts_with("regular") || key.starts_with("bright")) {
                    const std::string_view digits = key.substr(key.starts_with("regular") ? 7 : 6);
                    int index = -1;
                    const auto [end, ec] = std::from_chars(digits.data(),
                        digits.data() + digits.size(), index);
                    if (ec == std::errc{} && end == digits.data() + digits.size() &&
                        index >= 0 && index < 8) {
                        if (auto color = IniLoader::parse_color(value)) {
                            const size_t slot = key.starts_with("regular")
                                ? static_cast<size_t>(index)
                                : static_cast<size_t>(index + 8);
                            cfg.colors.palette[slot] = *color & 0x00ffffff;
                            cfg.colors.palette_customized = true;
                        }
                    }
                }
                return false;
            }

            case SectionKind::Scrollback: {
                if (key == "lines") {
                    int64_t parsed = 0;
                    if (parse_integer(value, parsed) && parsed >= 0)
                        cfg.scrollback.lines = static_cast<size_t>(parsed);
                } else if (key == "multiplier") {
                    int64_t parsed = 0;
                    if (parse_integer(value, parsed) && parsed > 0 &&
                        parsed <= std::numeric_limits<int>::max())
                        cfg.scrollback.multiplier = static_cast<int>(parsed);
                }
                return false;
            }

            case SectionKind::None:
                return false;
        }
        return false;
    }();
    return !bad_alpha;
}

[[nodiscard]] std::optional<ConfigError> parse_text(
    std::string_view text,
    Context& context
) {
    Section section;
    size_t offset = 0;
    int line_number = 1;

    while (offset <= text.size()) {
        const size_t newline = text.find('\n', offset);
        const size_t end = newline == std::string_view::npos ? text.size() : newline;
        std::string_view line = text.substr(offset, end - offset);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        const std::string_view content = trim(line);
        if (!content.empty() && content.front() != '#' && content.front() != ';') {
            if (content.front() == '[') {
                if (auto failure = parse_section(content, line_number, section))
                    return failure;
            } else {
                const size_t equals = content.find('=');
                if (equals == std::string_view::npos)
                    return error("expected '=' in key/value line", line_number,
                                 static_cast<int>(content.size() + 1));

                const std::string key = lowercase(trim(content.substr(0, equals)));
                const std::string_view raw_value = content.substr(equals + 1);
                const size_t value_offset = content.find_first_not_of(" \t", equals + 1);
                const int value_column = static_cast<int>(value_offset == std::string_view::npos
                    ? content.size() + 1 : value_offset + 1);
                std::string value;
                if (auto failure = decode_value(raw_value, line_number, value_column, value))
                    return failure;

                if (key == "include") {
                    if (context.depth >= kMaxIncludeDepth)
                        return error("include depth exceeds 8", line_number, value_column);
                    fs::path path(value);
                    if (path.is_relative()) path = context.base_dir / path;
                    std::error_code ec;
                    const fs::path canonical = fs::weakly_canonical(path, ec);
                    if (!ec) path = canonical;
                    const std::string identity = path.lexically_normal().generic_string();
                    if (std::find(context.include_stack.begin(), context.include_stack.end(),
                                  identity) != context.include_stack.end())
                        return error("include cycle detected: " + identity, line_number, value_column);

                    std::ifstream file(path);
                    if (!file.is_open())
                        return error("failed to open include: " + identity, line_number, value_column);
                    const std::string included((std::istreambuf_iterator<char>(file)),
                                               std::istreambuf_iterator<char>());
                    Context nested = context;
                    ++nested.depth;
                    nested.base_dir = path.parent_path();
                    nested.include_stack.push_back(identity);
                    if (auto failure = parse_text(included, nested)) return failure;
                } else {
                    ConfigError field_error;
                    if (!apply_field(context, section, key, value, line_number, field_error))
                        return field_error;
                }
            }
        }

        if (newline == std::string_view::npos) break;
        offset = newline + 1;
        ++line_number;
    }
    return std::nullopt;
}

std::string target_platform(std::string_view override) {
    if (!override.empty()) return lowercase(override);
#if defined(PLATFORM_MACOS)
    return "macos";
#elif defined(PLATFORM_LINUX)
    return "linux";
#else
    return {};
#endif
}

} // namespace

std::optional<uint32_t> IniLoader::parse_color(std::string_view str) noexcept {
    str = trim(str);
    if (str.empty()) return std::nullopt;
    if (str.front() == '#') str.remove_prefix(1);
    else if (str.starts_with("0x") || str.starts_with("0X")) str.remove_prefix(2);

    uint32_t value = 0;
    if (str.size() != 6 && str.size() != 8) return std::nullopt;
    const auto [end, ec] = std::from_chars(str.data(), str.data() + str.size(), value, 16);
    if (ec != std::errc{} || end != str.data() + str.size()) return std::nullopt;
    return str.size() == 6 ? (0xff000000u | value) : value;
}

std::expected<std::shared_ptr<AppConfig>, ConfigError> IniLoader::load_from_string(
    std::string_view ini_str,
    std::string_view platform_override
) {
    auto config = std::make_shared<AppConfig>();
    Context base_context;
    base_context.config = config;
    base_context.target_platform = target_platform(platform_override);
    base_context.pass = Pass::Base;

    if (auto failure = parse_text(ini_str, base_context)) return std::unexpected(*failure);

    Context platform_context = base_context;
    platform_context.pass = Pass::Platform;
    if (auto failure = parse_text(ini_str, platform_context)) return std::unexpected(*failure);
    return config;
}

std::expected<std::shared_ptr<AppConfig>, ConfigError> IniLoader::load_from_file(
    std::string_view file_path,
    std::string_view platform_override
) {
    const fs::path path(file_path);
    std::ifstream file(path);
    if (!file.is_open())
        return std::unexpected(error("Failed to open configuration file: " +
                                     std::string(file_path), 0, 0));
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());

    auto config = std::make_shared<AppConfig>();
    Context base_context;
    base_context.config = config;
    base_context.target_platform = target_platform(platform_override);
    base_context.pass = Pass::Base;
    base_context.base_dir = path.parent_path();

    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(path, ec);
    if (!ec) base_context.include_stack.push_back(canonical.lexically_normal().generic_string());
    else base_context.include_stack.push_back(path.lexically_normal().generic_string());

    if (auto failure = parse_text(text, base_context)) return std::unexpected(*failure);

    Context platform_context = base_context;
    platform_context.pass = Pass::Platform;
    if (auto failure = parse_text(text, platform_context)) return std::unexpected(*failure);
    return config;
}

} // namespace gritty::config
