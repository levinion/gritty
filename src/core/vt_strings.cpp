#include "core/vt_parser.hpp"
#include <charconv>
#include <cstdio>

namespace gritty::core {

void VtParser::handle_osc() {
    size_t semicolon = string_buffer_.find(';');
    if (semicolon == std::string::npos) return;

    std::string_view cmd_str(string_buffer_.data(), semicolon);
    std::string_view payload(string_buffer_.data() + semicolon + 1, string_buffer_.size() - semicolon - 1);

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
            }
            break;
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
                send_response("\x1bP0+r");
                send_response(name);
                send_response("\x1b\\");
            }
            if (end == std::string_view::npos) break;
            names.remove_prefix(end + 1);
        }
    }
    if (dcs_handler_) dcs_handler_(data);
}

} // namespace gritty::core
