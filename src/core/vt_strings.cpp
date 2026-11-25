#include "core/vt_parser.hpp"
#include "util/base64.hpp"
#include "util/format.hpp"
#include <array>
#include <charconv>
#include <cstring>
#include <optional>
#include <unistd.h>

namespace gritty::core {

namespace {

// Strict standard base64 (OSC 52 payloads are unpadded or '='-padded, no
// whitespace), bounded so a malicious stream cannot allocate without limit.
std::optional<std::string> decode_osc52_base64(std::string_view input, size_t max_bytes) {
    auto decoded = util::decode_base64(input, util::Base64Mode::Strict, max_bytes);
    if (!decoded) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(decoded->data()), decoded->size());
}

std::string hex_decode_ascii(std::string_view hex) {
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

// XTGETTCAP capabilities tmux (and other TUIs) use to enable terminal features.
// Booleans reply without a value; strings reply as hex-encoded terminfo values.
struct TermcapEntry {
    std::string_view name;
    std::string_view value_hex; // Empty means "boolean capability".
};

constexpr TermcapEntry kTermcaps[] = {
    {"RGB", ""},        // Direct colour (SGR 38;2 / 48;2)
    {"Tc", ""},         // True colour, alternate name
    {"Sync", ""},       // DECSET 2026 synchronized updates
    // Sxl is deliberately absent: only Kitty graphics is advertised.
    {"Ms", "1b5d35323b25703125733b257032257307"},                       // OSC 52 ; Pc ; data BEL
    {"Smulx", "1b5b343a25703125646d"},                                  // CSI 4 : Ps m (extended underline)
    {"Setulc", "1b5b35383a323a3a257031257b36353533367d252f25643a257031257b3235367d252f257b3235357d252625643a257031257b3235357d252625646d"}, // CSI 58:2::r:g:b m
};

[[nodiscard]] bool equals_ignore_case(std::string_view text, std::string_view expected) noexcept {
    if (text.size() != expected.size()) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const char lowered = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        if (lowered != expected[i]) return false;
    }
    return true;
}

[[nodiscard]] std::string_view trim_ascii(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

[[nodiscard]] platform::MouseCursorShape parse_cursor_shape(std::string_view name) noexcept {
    name = trim_ascii(name);
    using Shape = platform::MouseCursorShape;
    if (equals_ignore_case(name, "text") || equals_ignore_case(name, "xterm") ||
        equals_ignore_case(name, "ibeam")) return Shape::Text;
    if (equals_ignore_case(name, "pointer") || equals_ignore_case(name, "hand2") ||
        equals_ignore_case(name, "hand")) return Shape::Pointer;
    if (equals_ignore_case(name, "ew-resize") || equals_ignore_case(name, "col-resize") ||
        equals_ignore_case(name, "sb_h_double_arrow")) return Shape::EwResize;
    if (equals_ignore_case(name, "ns-resize") || equals_ignore_case(name, "row-resize") ||
        equals_ignore_case(name, "sb_v_double_arrow")) return Shape::NsResize;
    if (equals_ignore_case(name, "nesw-resize")) return Shape::NeswResize;
    if (equals_ignore_case(name, "nwse-resize")) return Shape::NwseResize;
    if (equals_ignore_case(name, "crosshair")) return Shape::Crosshair;
    if (equals_ignore_case(name, "move")) return Shape::Move;
    if (equals_ignore_case(name, "not-allowed")) return Shape::NotAllowed;
    if (equals_ignore_case(name, "none")) return Shape::Hidden;
    return Shape::Default; // Empty and unknown names fall back.
}

[[nodiscard]] bool is_local_host(std::string_view host) noexcept {
    if (host.empty() || equals_ignore_case(host, "localhost")) return true;
    char buffer[256]{};
    if (::gethostname(buffer, sizeof(buffer) - 1) != 0) return false;
    const std::string_view local(buffer, std::strlen(buffer));
    if (equals_ignore_case(host, local)) return true;
    const auto dot = local.find('.');
    return dot != std::string_view::npos && equals_ignore_case(host, local.substr(0, dot));
}

[[nodiscard]] int hex_nibble(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// OSC 7: file://<host>/<absolute path>. Reject remote hosts, NUL bytes and
// malformed percent escapes before touching the engine's cwd.
[[nodiscard]] std::optional<std::string> parse_file_url(std::string_view value) {
    constexpr std::string_view scheme = "file://";
    if (value.size() < scheme.size()) return std::nullopt;
    for (size_t i = 0; i < scheme.size(); ++i) {
        const char c = value[i];
        if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) != scheme[i])
            return std::nullopt;
    }
    value.remove_prefix(scheme.size());
    const auto slash = value.find('/');
    if (slash == std::string_view::npos || !is_local_host(value.substr(0, slash))) return std::nullopt;

    std::string path;
    path.reserve(value.size() - slash);
    for (size_t i = slash; i < value.size(); ++i) {
        const char c = value[i];
        if (c == '%') {
            if (i + 2 >= value.size()) return std::nullopt;
            const int hi = hex_nibble(value[i + 1]);
            const int lo = hex_nibble(value[i + 2]);
            if (hi < 0 || lo < 0) return std::nullopt;
            path.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else {
            path.push_back(c);
        }
    }
    if (path.empty() || path.front() != '/' || path.find('\0') != std::string::npos)
        return std::nullopt;
    return path;
}

void append_decimal(std::string& out, uint32_t value) {
    char digits[16]{};
    const auto [end, ec] = std::to_chars(digits, digits + sizeof(digits), value);
    if (ec == std::errc{}) out.append(digits, end);
}

void append_channel(std::string& out, uint32_t channel8) {
    const uint32_t wide = channel8 * 257; // X11 8-bit -> 16-bit expansion.
    char digits[4]{
        static_cast<char>("0123456789abcdef"[(wide >> 12) & 15]),
        static_cast<char>("0123456789abcdef"[(wide >> 8) & 15]),
        static_cast<char>("0123456789abcdef"[(wide >> 4) & 15]),
        static_cast<char>("0123456789abcdef"[wide & 15]),
    };
    out.append(digits, sizeof(digits));
}

[[nodiscard]] std::string color_reply(int command, uint32_t argb, bool with_st, int index = -1) {
    std::string out = "\x1b]";
    append_decimal(out, static_cast<uint32_t>(command));
    out.push_back(';');
    if (index >= 0) {
        append_decimal(out, static_cast<uint32_t>(index));
        out.push_back(';');
    }
    out += "rgb:";
    append_channel(out, (argb >> 16) & 255);
    out.push_back('/');
    append_channel(out, (argb >> 8) & 255);
    out.push_back('/');
    append_channel(out, argb & 255);
    if (with_st) out += "\x1b\\";
    else out.push_back('\x07');
    return out;
}

[[nodiscard]] bool parse_index(std::string_view text, int& out) noexcept {
    if (text.empty()) return false;
    int value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || end != text.data() + text.size() || value < 0 || value > 255)
        return false;
    out = value;
    return true;
}

} // namespace

namespace {
// foot osc.c parse_legacy_color/parse_rgb: components occupy the high bits,
// rather than scaling a single hex digit (f -> f0, not ff). Bound to 16 bits
// per component and reject malformed input instead of partially consuming it.
std::optional<uint32_t> parse_dynamic_rgb(std::string_view value) {
    std::array<std::string_view, 3> parts;
    if (value.starts_with('#')) {
        value.remove_prefix(1);
        if (value.size() < 3 || value.size() > 12 || value.size() % 3 != 0) return {};
        const auto digits = value.size() / 3;
        for (size_t i = 0; i < parts.size(); ++i) parts[i] = value.substr(i * digits, digits);
    } else if (value.starts_with("rgb:")) {
        value.remove_prefix(4);
        for (size_t i = 0; i < parts.size(); ++i) {
            const auto slash = value.find('/');
            if ((i < 2) == (slash == std::string_view::npos)) return {};
            parts[i] = value.substr(0, slash);
            if (i < 2) value.remove_prefix(slash + 1);
        }
    } else return {};
    uint32_t rgb = 0;
    for (const auto part : parts) {
        if (part.empty() || part.size() > 4) return {};
        uint32_t channel = 0;
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), channel, 16);
        if (error != std::errc() || end != part.data() + part.size()) return {};
        channel = part.size() == 1 ? channel << 4 : channel >> (4 * (part.size() - 2));
        rgb = (rgb << 8) | channel;
    }
    return rgb;
}
}

void VtParser::update_default_color(int command, uint32_t rgb) {
    auto& color = command == 10 ? default_fg_ : default_bg_;
    // Ordinary OSC color changes do not override the configured window alpha.
    const auto updated = (color & 0xff000000) | (rgb & 0xffffff);
    if (updated == color) return;
    color = updated;
    if (default_color_handler_) default_color_handler_(default_fg_, default_bg_);
}

void VtParser::update_cursor_color(uint32_t argb) {
    if (cursor_color_ == argb) return;
    cursor_color_ = argb;
    if (cursor_color_handler_) cursor_color_handler_(argb);
}

void VtParser::handle_osc() {
    const std::string_view sequence = string_buffer_;
    const auto semicolon = sequence.find(';');
    const auto cmd_str = sequence.substr(0, semicolon);
    const auto payload = semicolon == std::string_view::npos ? std::string_view{} : sequence.substr(semicolon + 1);

    int cmd = -1;
    auto [ptr, ec] = std::from_chars(cmd_str.data(), cmd_str.data() + cmd_str.size(), cmd);
    if (ec != std::errc() || ptr != cmd_str.data() + cmd_str.size()) return;

    if (osc_handler_) osc_handler_(cmd, payload);

    switch (cmd) {
        case 0:
        case 2:
            engine_.set_window_title(payload);
            break;
        case 7: {
            if (const auto path = parse_file_url(payload)) engine_.set_cwd(*path);
            break;
        }
        case 4: {
            // OSC 4 ; idx ; ? (repeatable), or OSC 4 ; idx ; colour.
            size_t pos = 0;
            while (pos < payload.size()) {
                const auto next = payload.find(';', pos);
                const auto token = payload.substr(pos, next == std::string_view::npos
                    ? std::string_view::npos : next - pos);
                int index = -1;
                if (token == "?" || !parse_index(token, index)) {
                    if (next == std::string_view::npos) break;
                    pos = next + 1;
                    continue;
                }
                const size_t value_begin = next == std::string_view::npos ? payload.size() : next + 1;
                const auto value_end = payload.find(';', value_begin);
                const auto value = payload.substr(value_begin, value_end == std::string_view::npos
                    ? std::string_view::npos : value_end - value_begin);
                if (value == "?") {
                    send_response(color_reply(4, engine_.palette_color(static_cast<uint8_t>(index)),
                                              osc_reply_with_st_, index));
                } else if (const auto rgb = parse_dynamic_rgb(value)) {
                    engine_.set_palette_color(static_cast<uint8_t>(index), *rgb);
                }
                if (value_end == std::string_view::npos) break;
                pos = value_end + 1;
            }
            break;
        }
        case 10:
        case 11:
        case 12: {
            if (payload == "?") {
                const auto color = cmd == 10 ? default_fg_ : cmd == 11 ? default_bg_ : cursor_color_;
                send_response(color_reply(cmd, color, osc_reply_with_st_));
            } else if (const auto rgb = parse_dynamic_rgb(payload)) {
                if (cmd == 12) update_cursor_color((cursor_color_ & 0xff000000) | *rgb);
                else update_default_color(cmd, *rgb);
            }
            break;
        }
        case 104: {
            if (payload.empty() || payload == ";") {
                engine_.reset_palette();
            } else {
                size_t pos = 0;
                while (pos < payload.size()) {
                    const auto end = payload.find(';', pos);
                    const auto token = payload.substr(pos, end == std::string_view::npos
                        ? std::string_view::npos : end - pos);
                    int index = -1;
                    if (parse_index(token, index))
                        engine_.reset_palette_color(static_cast<uint8_t>(index));
                    if (end == std::string_view::npos) break;
                    pos = end + 1;
                }
            }
            break;
        }
        case 110:
        case 111:
            update_default_color(cmd - 100, cmd == 110 ? configured_fg_ : configured_bg_);
            break;
        case 112:
            update_cursor_color(configured_cursor_);
            break;
        case 22: {
            const auto shape = parse_cursor_shape(payload);
            engine_.set_mouse_cursor_shape(shape);
            if (mouse_cursor_shape_handler_) mouse_cursor_shape_handler_(shape);
            break;
        }
        case 52: {
            // OSC 52 ; target ; payload. Policy denials are silent: never emit
            // a diagnostic back to the PTY (foot osc.c behaviour).
            const auto target_end = payload.find(';');
            if (target_end == std::string_view::npos) break;
            const auto target = payload.substr(0, target_end);
            const auto data = payload.substr(target_end + 1);
            const bool allow_write = osc52_mode_ == config::Osc52Mode::CopyOnly ||
                osc52_mode_ == config::Osc52Mode::Enabled;
            const bool allow_read = osc52_mode_ == config::Osc52Mode::PasteOnly ||
                osc52_mode_ == config::Osc52Mode::Enabled;
            if (data == "?") {
                if (!allow_read || !clipboard_read_handler_) break;
                std::string reply = "\x1b]52;";
                // An empty target selector defaults to the system clipboard.
                const auto reply_target = target.empty() ? std::string_view("c") : target;
                reply.append(reply_target);
                reply.push_back(';');
                reply += util::encode_base64(clipboard_read_handler_());
                if (osc_reply_with_st_) reply += "\x1b\\";
                else reply.push_back('\x07');
                send_response(reply);
                break;
            }
            if (data == "!") { // Explicit clipboard clear.
                if (allow_write && clipboard_handler_) clipboard_handler_("");
                break;
            }
            if (data.empty() || !allow_write || !clipboard_handler_) break;
            if (auto decoded = decode_osc52_base64(data, 1024 * 1024)) {
                if (!decoded->empty()) clipboard_handler_(*decoded);
            }
            break;
        }
        case 777: {
            // OSC 777 ; notify ; title ; body (body may contain ';').
            constexpr std::string_view prefix = "notify;";
            if (!payload.starts_with(prefix) || !notification_handler_) break;
            const auto rest = payload.substr(prefix.size());
            const auto split = rest.find(';');
            if (split == std::string_view::npos) break;
            notification_handler_(rest.substr(0, split), rest.substr(split + 1));
            break;
        }
        case 8: {
            // OSC 8 ; params ; url
            const auto second_semi = payload.find(';');
            if (second_semi != std::string_view::npos) {
                const std::string_view url = payload.substr(second_semi + 1);
                if (url.empty()) engine_.clear_current_hyperlink();
                else engine_.set_current_hyperlink(url);
            }
            break;
        }
        default:
            break; // Unknown OSC payloads are already consumed by the parser.
    }
}

void VtParser::handle_dcs() {
    const std::string_view data = string_buffer_;
    if (data.starts_with("tmux;")) {
        // tmux doubles ESC inside a DCS passthrough. Feed the unwrapped inner
        // stream to a bounded nested parser so APC G and DCS queries are
        // identical to their non-passthrough forms.
        if (passthrough_depth_ < 2) {
            VtParser inner(engine_);
            inner.passthrough_depth_ = passthrough_depth_ + 1;
            inner.default_fg_ = default_fg_;
            inner.default_bg_ = default_bg_;
            inner.configured_fg_ = configured_fg_;
            inner.configured_bg_ = configured_bg_;
            inner.apc_handler_ = apc_handler_;
            inner.dcs_handler_ = dcs_handler_;
            inner.osc_handler_ = osc_handler_;
            inner.response_cb_ = response_cb_;
            inner.clipboard_handler_ = clipboard_handler_;
            inner.clipboard_read_handler_ = clipboard_read_handler_;
            inner.osc52_mode_ = osc52_mode_;
            inner.notification_handler_ = notification_handler_;
            inner.cursor_color_handler_ = cursor_color_handler_;
            inner.mouse_cursor_shape_handler_ = mouse_cursor_shape_handler_;
            inner.cursor_color_ = cursor_color_;
            inner.configured_cursor_ = configured_cursor_;
            inner.sync_handler_ = sync_handler_;
            inner.default_color_handler_ = default_color_handler_;
            inner.feed(data.substr(5));
            default_fg_ = inner.default_fg_;
            default_bg_ = inner.default_bg_;
            cursor_color_ = inner.cursor_color_;
        }
        return;
    }
    if (data.starts_with("$q")) {
        const auto query = data.substr(2);
        char reply[80];
        if (query == "r") {
            const auto text = util::format_to_buffer(reply, "\x1bP1$r{};{}r\x1b\\",
                engine_.margin_top() + 1, engine_.margin_bottom() + 1);
            if (!text.empty()) send_response(text);
        } else if (query == " q") {
            const auto shape = engine_.cursor_shape();
            const int mode = (shape == CursorShape::Block ? 2 : shape == CursorShape::Underline ? 4 : 6) - engine_.cursor_blink();
            const auto text = util::format_to_buffer(reply, "\x1bP1$r{} q\x1b\\", mode);
            if (!text.empty()) send_response(text);
        } else {
            // A negative reply is preferable to falsely claiming an unsupported status.
            send_response("\x1bP0$r\x1b\\");
        }
    } else if (data.starts_with("+q")) {
        auto names = data.substr(2);
        while (!names.empty()) {
            const auto end = names.find(';');
            const auto name = names.substr(0, end);
            if (!name.empty() && name.size() % 2 == 0 &&
                name.find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos) {
                const std::string capability = hex_decode_ascii(name);
                const TermcapEntry* match = nullptr;
                for (const auto& entry : kTermcaps) {
                    if (entry.name == capability) { match = &entry; break; }
                }
                std::string reply = match ? "\x1bP1+r" : "\x1bP0+r";
                reply.append(name);
                if (match && !match->value_hex.empty()) {
                    reply.push_back('=');
                    reply.append(match->value_hex);
                }
                reply.append("\x1b\\");
                send_response(reply);
            }
            if (end == std::string_view::npos) break;
            names.remove_prefix(end + 1);
        }
    }
    if (dcs_handler_) dcs_handler_(data);
}

} // namespace gritty::core
