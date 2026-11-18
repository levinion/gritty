#include "core/vt_parser.hpp"
#include <array>
#include <charconv>
#include <cstdio>
#include <optional>

namespace gritty::core {

namespace {

// Strict standard base64 (OSC 52 payloads are unpadded or '='-padded, no
// whitespace), bounded so a malicious stream cannot allocate without limit.
std::optional<std::string> decode_osc52_base64(std::string_view input, size_t max_bytes) {
    auto value_of = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    out.reserve(std::min(input.size() * 3 / 4, max_bytes));
    uint32_t accumulator = 0;
    int bits = 0;
    for (const char c : input) {
        if (c == '=') break; // Padding: the remainder carries no data.
        const int value = value_of(c);
        if (value < 0) return std::nullopt;
        accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((accumulator >> bits) & 0xFF));
            if (out.size() > max_bytes) return std::nullopt;
        }
    }
    return out;
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
    {"Sxl", ""},        // Sixel graphics (tmux enables its image cache for this).
    {"Ms", "1b5d35323b25703125733b257032257307"},                       // OSC 52 ; Pc ; data BEL
    {"Smulx", "1b5b343a25703125646d"},                                  // CSI 4 : Ps m (extended underline)
    {"Setulc", "1b5b35383a323a3a25703125643a25703225643a25703325646d"}, // CSI 58:2::r:g:b m
};

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

void VtParser::handle_osc() {
    const std::string_view sequence = string_buffer_;
    const auto semicolon = sequence.find(';');
    const auto cmd_str = sequence.substr(0, semicolon);
    const auto payload = semicolon == std::string_view::npos ? std::string_view{} : sequence.substr(semicolon + 1);

    int cmd = -1;
    auto [ptr, ec] = std::from_chars(cmd_str.data(), cmd_str.data() + cmd_str.size(), cmd);
    if (ec != std::errc() || ptr != cmd_str.data() + cmd_str.size()) return;

    if (osc_handler_) {
        osc_handler_(cmd, payload);
    }

    switch (cmd) {
        case 0:
        case 2:
            engine_.set_window_title(payload);
            break;
        case 7:
            engine_.set_cwd(payload);
            break;
        case 10:
        case 11:
            if (payload == "?") {
                const auto color = cmd == 10 ? default_fg_ : default_bg_;
                char reply[80];
                std::snprintf(reply, sizeof(reply), "\x1b]%d;rgb:%04x/%04x/%04x\x1b\\",
                    cmd, ((color >> 16) & 255) * 257, ((color >> 8) & 255) * 257, (color & 255) * 257);
                send_response(reply);
            } else if (const auto rgb = parse_dynamic_rgb(payload)) {
                update_default_color(cmd, *rgb);
            }
            break;
        case 110:
        case 111:
            update_default_color(cmd - 100, cmd == 110 ? configured_fg_ : configured_bg_);
            break;
        case 52: {
            // OSC 52 ; Pc ; base64(ST) — clipboard write, used by tmux's
            // set-clipboard. Queries ("?") are intentionally not answered.
            const auto selection_end = payload.find(';');
            if (selection_end == std::string_view::npos || !clipboard_handler_) break;
            const auto data = payload.substr(selection_end + 1);
            if (data.empty() || data == "?") break;
            if (auto decoded = decode_osc52_base64(data, 1024 * 1024)) {
                if (!decoded->empty()) clipboard_handler_(*decoded);
            }
            break;
        }
        case 8: {
            // OSC 8 ; params ; url
            size_t second_semi = payload.find(';');
            if (second_semi != std::string::npos) {
                std::string_view url = payload.substr(second_semi + 1);
                if (url.empty()) {
                    engine_.clear_current_hyperlink();
                } else {
                    engine_.set_current_hyperlink(url);
                }
            }
            break;
        }
        default:
            break;
    }
}

void VtParser::handle_dcs() {
    const std::string_view data = string_buffer_;
    if (data.starts_with("tmux;")) {
        // tmux doubles ESC inside a DCS passthrough. Feed the unwrapped inner
        // stream to a bounded nested parser so APC G and DCS q dispatches are
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
            inner.sync_handler_ = sync_handler_;
            inner.default_color_handler_ = default_color_handler_;
            inner.feed(data.substr(5));
        }
        return;
    }
    if (data.starts_with("$q")) {
        const auto query = data.substr(2);
        char reply[80];
        if (query == "r") {
            std::snprintf(reply, sizeof(reply), "\x1bP1$r%d;%dr\x1b\\", engine_.margin_top() + 1, engine_.margin_bottom() + 1);
            send_response(reply);
        } else if (query == " q") {
            const auto shape = engine_.cursor_shape();
            const int mode = (shape == CursorShape::Block ? 2 : shape == CursorShape::Underline ? 4 : 6) - engine_.cursor_blink();
            std::snprintf(reply, sizeof(reply), "\x1bP1$r%d q\x1b\\", mode);
            send_response(reply);
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
