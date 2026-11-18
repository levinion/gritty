#include "core/vt_parser.hpp"
#include <algorithm>

namespace gritty::core {

// State transitions follow foot vt.c (DEC's ANSI parser), with bounded strings
// for this callback-based implementation. See upstream foot vt.c and tmux source tests.
VtParser::VtParser(TerminalEngine& engine) : engine_(engine) {
    string_buffer_.reserve(65536);
    reset();
}

void VtParser::clear_sequence() noexcept {
    is_private_ = has_greater_ = has_less_ = has_equal_ = false;
    intermediate_char_ = 0;
    param_count_ = current_param_idx_ = 0;
    current_subparam_idx_ = -1;
    for (auto& parameter : params_) parameter = Parameter{};
}

void VtParser::reset() noexcept {
    state_ = ParserState::Ground;
    clear_sequence();
    string_type_ = StringType::None;
    string_buffer_.clear();
    string_overflow_ = false;
    dcs_tmux_escape_pending_ = false;
    utf8_state_ = utf8_codepoint_ = utf8_minimum_ = 0;
}

void VtParser::feed(std::string_view data) {
    feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

void VtParser::feed(const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; ++i) process_byte(data[i]);
}

void VtParser::send_response(std::string_view response) {
    if (response_cb_) response_cb_(response);
}

int VtParser::get_param(int idx, int fallback) const noexcept {
    // foot vt_param_get: zero, just like omission, selects the default.
    return idx < param_count_ && params_[idx].value > 0 ? params_[idx].value : fallback;
}

void VtParser::append_string(uint8_t byte) {
    // OSC stays bounded: it is used for titles/hyperlinks/clipboard and has no
    // large-payload use case. DCS/APC carry Sixel and Kitty images, so allow the
    // same 32 MiB working budget as the renderer's image cache.
    const size_t limit = string_type_ == StringType::Osc
        ? 65536 : 32u * 1024u * 1024u;
    if (string_buffer_.size() < limit) string_buffer_.push_back(static_cast<char>(byte));
    else string_overflow_ = true;
}

void VtParser::finish_string() {
    if (!string_overflow_) {
        if (state_ == ParserState::OscString) handle_osc();
        else if (state_ == ParserState::DcsString) handle_dcs();
        else if (state_ == ParserState::ApcString && apc_handler_) apc_handler_(string_buffer_);
    }
    string_buffer_.clear();
    string_type_ = StringType::None;
    string_overflow_ = false;
    dcs_tmux_escape_pending_ = false;
    // A 32 MiB image must not pin its heap capacity for the rest of the session.
    if (string_buffer_.capacity() > 256 * 1024) {
        std::string empty;
        string_buffer_.swap(empty); // Portable shrink-to-fit across libstdc++/libc++.
    }
}

void VtParser::print_codepoint(char32_t cp) {
    engine_.print_char(cp);
}

void VtParser::process_byte(uint8_t byte) {
    // CAN/SUB cancel sequences in *every* state, including strings.
    if (byte == 0x18 || byte == 0x1a) {
        reset();
        return;
    }
    if (dcs_tmux_escape_pending_ && state_ == ParserState::DcsString) {
        dcs_tmux_escape_pending_ = false;
        if (byte == 0x1b) {
            // ESC ESC inside tmux passthrough represents one literal ESC.
            append_string(0x1b);
        } else if (byte == '\\') {
            // ESC \ is the outer ST; consume it without adding it to payload.
            finish_string();
            state_ = ParserState::Ground;
        } else {
            finish_string();
            clear_sequence();
            state_ = ParserState::Escape;
            process_byte(byte);
        }
        return;
    }
    if (byte == 0x1b) {
        if (state_ == ParserState::DcsString && string_buffer_.starts_with("tmux;")) {
            dcs_tmux_escape_pending_ = true;
            return;
        }
        finish_string();
        clear_sequence();
        utf8_state_ = 0;
        state_ = ParserState::Escape;
        return;
    }
    if (state_ == ParserState::OscString || state_ == ParserState::DcsString ||
        state_ == ParserState::ApcString || state_ == ParserState::IgnoredString) {
        // BEL terminates OSC only. DCS/APC are terminated by ST, not BEL.
        if (byte == 0x07 && state_ == ParserState::OscString) {
            finish_string();
            state_ = ParserState::Ground;
        } else if (state_ != ParserState::IgnoredString) append_string(byte);
        return;
    }
    if (byte == 0x7f) return; // DEL is ignored, including within ESC and CSI.
    if (byte < 0x20) {
        utf8_state_ = 0;
        handle_c0(byte);
        return;
    }

    switch (state_) {
        case ParserState::Ground: {
            if (utf8_state_ && (byte & 0xc0) == 0x80) {
                utf8_codepoint_ = (utf8_codepoint_ << 6) | (byte & 0x3f);
                if (--utf8_state_ == 0) {
                    const auto cp = utf8_codepoint_;
                    print_codepoint(cp >= utf8_minimum_ && cp <= 0x10ffff &&
                        !(cp >= 0xd800 && cp <= 0xdfff) ? cp : U'\ufffd');
                }
                break;
            }
            if (utf8_state_) {
                utf8_state_ = 0;
                print_codepoint(U'\ufffd');
            }
            if (byte < 0x80) print_codepoint(byte);
            else if (byte >= 0xc2 && byte <= 0xdf) {
                utf8_codepoint_ = byte & 0x1f;
                utf8_state_ = 1;
                utf8_minimum_ = 0x80;
            } else if (byte >= 0xe0 && byte <= 0xef) {
                utf8_codepoint_ = byte & 0x0f;
                utf8_state_ = 2;
                utf8_minimum_ = 0x800;
            } else if (byte >= 0xf0 && byte <= 0xf4) {
                utf8_codepoint_ = byte & 7;
                utf8_state_ = 3;
                utf8_minimum_ = 0x10000;
            }
            break;
        }
        case ParserState::Escape:
        case ParserState::EscapeIntermediate:
            if (byte >= 0x20 && byte <= 0x2f) {
                // Only single-intermediate ESC sequences are currently supported.
                if (intermediate_char_ == 0) intermediate_char_ = byte;
                else intermediate_char_ = 0xff;
                state_ = ParserState::EscapeIntermediate;
            } else if (byte >= 0x30 && byte <= 0x7e) handle_escape(byte);
            else state_ = ParserState::Ground;
            break;
        case ParserState::CsiEntry:
        case ParserState::CsiParam:
        case ParserState::CsiIntermediate:
            parse_csi_param_byte(byte);
            break;
        case ParserState::CsiIgnore:
            if (byte >= 0x40 && byte <= 0x7e) state_ = ParserState::Ground;
            break;
        case ParserState::DcsEntry:
        case ParserState::DcsParam:
        case ParserState::DcsIntermediate:
            if (byte >= 0x40 && byte <= 0x7e) {
                append_string(byte);
                state_ = ParserState::DcsString;
            } else if (byte >= 0x20 && byte <= 0x3f) {
                append_string(byte);
            } else state_ = ParserState::DcsIgnore;
            break;
        default:
            break;
    }
}

void VtParser::handle_c0(uint8_t byte) {
    switch (byte) {
        case 0x08: engine_.move_cursor_backward(1); break;
        case 0x09: engine_.tab_forward(1); break;
        case 0x0a: case 0x0b: case 0x0c: engine_.line_feed(); break;
        case 0x0d: engine_.carriage_return(); break;
        case 0x0e: engine_.select_charset(1); break;
        case 0x0f: engine_.select_charset(0); break;
        default: break;
    }
}

void VtParser::handle_escape(uint8_t byte) {
    state_ = ParserState::Ground;
    if (intermediate_char_) {
        if (intermediate_char_ >= '(' && intermediate_char_ <= '+' && (byte == '0' || byte == 'B'))
            engine_.designate_charset(intermediate_char_ - '(', byte == '0');
        return;
    }
    switch (byte) {
        case '[': clear_sequence(); state_ = ParserState::CsiEntry; break;
        case ']': state_ = ParserState::OscString; string_type_ = StringType::Osc; break;
        case 'P': state_ = ParserState::DcsEntry; string_type_ = StringType::Dcs; break;
        case '_': state_ = ParserState::ApcString; string_type_ = StringType::Apc; break;
        case 'X': case '^': state_ = ParserState::IgnoredString; break;
        case '7': engine_.save_cursor(); break;
        case '8': engine_.restore_cursor(); break;
        case 'M': engine_.reverse_index(); break;
        case 'D': engine_.line_feed(); break;
        case 'E': engine_.carriage_return(); engine_.line_feed(); break;
        case 'H': engine_.set_tab_stop(); break;
        case 'c':
            engine_.reset();
            update_default_color(10, configured_fg_);
            update_default_color(11, configured_bg_);
            if (sync_handler_) sync_handler_(false);
            break;
        default: break;
    }
}

void VtParser::parse_csi_param_byte(uint8_t byte) {
    if (state_ == ParserState::CsiEntry && byte >= '<' && byte <= '?') {
        is_private_ = byte == '?';
        has_greater_ = byte == '>';
        has_less_ = byte == '<';
        has_equal_ = byte == '=';
        state_ = ParserState::CsiParam;
        return;
    }
    if (byte >= 0x40 && byte <= 0x7e) {
        handle_csi(byte);
        state_ = ParserState::Ground;
        return;
    }
    if (byte >= 0x20 && byte <= 0x2f) {
        if (intermediate_char_) { state_ = ParserState::CsiIgnore; return; }
        intermediate_char_ = byte;
        state_ = ParserState::CsiIntermediate;
        return;
    }
    if (state_ == ParserState::CsiIntermediate) { state_ = ParserState::CsiIgnore; return; }
    state_ = ParserState::CsiParam;
    if (byte >= '0' && byte <= '9') {
        int& value = current_subparam_idx_ >= 0
            ? params_[current_param_idx_].subparams[current_subparam_idx_]
            : params_[current_param_idx_].value;
        // Saturate before multiplication to avoid signed overflow and unbounded work.
        value = std::min(65535, std::max(0, value) * 10 + byte - '0');
        param_count_ = current_param_idx_ + 1;
    } else if (byte == ';') {
        if (++current_param_idx_ >= 32) { state_ = ParserState::CsiIgnore; return; }
        current_subparam_idx_ = -1;
        param_count_ = current_param_idx_ + 1;
    } else if (byte == ':') {
        if (++current_subparam_idx_ >= 8) { state_ = ParserState::CsiIgnore; return; }
        params_[current_param_idx_].subparam_count = current_subparam_idx_ + 1;
        param_count_ = current_param_idx_ + 1;
    } else state_ = ParserState::CsiIgnore;
}

} // namespace gritty::core
