#include "core/vt_parser.hpp"
#include <charconv>
#include <cstdio>
#include <cstring>

namespace gritty::core {

VtParser::VtParser(TerminalEngine& engine) : engine_(engine) {
    reset();
}

void VtParser::reset() noexcept {
    state_ = ParserState::Ground;
    is_private_ = false;
    has_greater_ = false;
    has_less_ = false;
    has_equal_ = false;
    intermediate_char_ = 0;
    param_count_ = 0;
    current_param_idx_ = 0;
    current_subparam_idx_ = -1;
    string_type_ = StringType::None;
    string_buffer_.clear();
    utf8_state_ = 0;
    utf8_codepoint_ = 0;
}

void VtParser::feed(std::string_view data) {
    feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

void VtParser::feed(const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        process_byte(data[i]);
    }
}

void VtParser::send_response(std::string_view resp) {
    if (response_cb_) {
        response_cb_(resp);
    }
}

void VtParser::process_byte(uint8_t byte) {
    // Check for string terminators or transitions in string states
    if (state_ == ParserState::OscString || state_ == ParserState::DcsString || state_ == ParserState::ApcString) {
        if (byte == 0x07) { // BEL terminates string
            if (string_type_ == StringType::Osc) handle_osc();
            else if (string_type_ == StringType::Apc && apc_handler_) apc_handler_(string_buffer_);
            else if (string_type_ == StringType::Dcs && dcs_handler_) dcs_handler_(string_buffer_);
            state_ = ParserState::Ground;
            string_type_ = StringType::None;
            string_buffer_.clear();
            return;
        }
        if (byte == 0x1B) { // ESC might be start of ST (ESC \)
            state_ = ParserState::Escape;
            return;
        }
        string_buffer_.push_back(static_cast<char>(byte));
        return;
    }

    if (byte == 0x1B) {
        state_ = ParserState::Escape;
        string_type_ = StringType::None;
        is_private_ = false;
        has_greater_ = false;
        has_less_ = false;
        has_equal_ = false;
        intermediate_char_ = 0;
        param_count_ = 0;
        current_param_idx_ = 0;
        current_subparam_idx_ = -1;
        for (auto& p : params_) {
            p.value = -1;
            p.subparam_count = 0;
            for (auto& sp : p.subparams) sp = -1;
        }
        return;
    }

    // C0 controls (0x00 - 0x1F)
    if (byte < 0x20) {
        handle_c0(byte);
        return;
    }

    switch (state_) {
        case ParserState::Ground: {
            // UTF-8 decoding
            if (byte < 0x80) {
                engine_.print_char(byte);
            } else if ((byte & 0xE0) == 0xC0) {
                utf8_codepoint_ = byte & 0x1F;
                utf8_state_ = 1;
            } else if ((byte & 0xF0) == 0xE0) {
                utf8_codepoint_ = byte & 0x0F;
                utf8_state_ = 2;
            } else if ((byte & 0xF8) == 0xF0) {
                utf8_codepoint_ = byte & 0x07;
                utf8_state_ = 3;
            } else if ((byte & 0xC0) == 0x80) {
                if (utf8_state_ > 0) {
                    utf8_codepoint_ = (utf8_codepoint_ << 6) | (byte & 0x3F);
                    utf8_state_--;
                    if (utf8_state_ == 0) {
                        engine_.print_char(utf8_codepoint_);
                    }
                }
            } else {
                utf8_state_ = 0;
            }
            break;
        }

        case ParserState::Escape:
            handle_escape(byte);
            break;

        case ParserState::CsiEntry:
        case ParserState::CsiParam:
        case ParserState::CsiIntermediate:
            parse_csi_param_byte(byte);
            break;

        case ParserState::CsiIgnore:
            if (byte >= 0x40 && byte <= 0x7E) {
                state_ = ParserState::Ground;
            }
            break;

        default:
            break;
    }
}

void VtParser::handle_c0(uint8_t byte) {
    switch (byte) {
        case 0x07: // BEL
            break;
        case 0x08: // BS
            engine_.move_cursor_backward(1);
            break;
        case 0x09: // HT
            engine_.tab_forward(1);
            break;
        case 0x0A: // LF
        case 0x0B: // VT
        case 0x0C: // FF
            engine_.line_feed();
            break;
        case 0x0D: // CR
            engine_.carriage_return();
            break;
        default:
            break;
    }
}

void VtParser::handle_escape(uint8_t byte) {
    if (byte == '\\') { // ST (String Terminator)
        if (string_type_ == StringType::Osc) {
            handle_osc();
        } else if (string_type_ == StringType::Apc && apc_handler_) {
            apc_handler_(string_buffer_);
        } else if (string_type_ == StringType::Dcs && dcs_handler_) {
            dcs_handler_(string_buffer_);
        }
        string_buffer_.clear();
        string_type_ = StringType::None;
        state_ = ParserState::Ground;
        return;
    }

    switch (byte) {
        case '[':
            state_ = ParserState::CsiEntry;
            break;
        case ']':
            state_ = ParserState::OscString;
            string_type_ = StringType::Osc;
            string_buffer_.clear();
            break;
        case 'P':
            state_ = ParserState::DcsEntry;
            string_type_ = StringType::Dcs;
            string_buffer_.clear();
            break;
        case '_':
            state_ = ParserState::ApcString;
            string_type_ = StringType::Apc;
            string_buffer_.clear();
            break;
        case '7':
            engine_.save_cursor();
            state_ = ParserState::Ground;
            break;
        case '8':
            engine_.restore_cursor();
            state_ = ParserState::Ground;
            break;
        case 'M':
            engine_.reverse_index();
            state_ = ParserState::Ground;
            break;
        case 'D':
            engine_.line_feed();
            state_ = ParserState::Ground;
            break;
        case 'E':
            engine_.carriage_return();
            engine_.line_feed();
            state_ = ParserState::Ground;
            break;
        case 'H':
            engine_.set_tab_stop();
            state_ = ParserState::Ground;
            break;
        case 'c':
            engine_.reset_palette();
            engine_.sgr_reset();
            state_ = ParserState::Ground;
            break;
        default:
            state_ = ParserState::Ground;
            break;
    }
}

void VtParser::parse_csi_param_byte(uint8_t byte) {
    if (state_ == ParserState::CsiEntry) {
        if (byte == '?') {
            is_private_ = true;
            state_ = ParserState::CsiParam;
            return;
        } else if (byte == '>') {
            has_greater_ = true;
            state_ = ParserState::CsiParam;
            return;
        } else if (byte == '<') {
            has_less_ = true;
            state_ = ParserState::CsiParam;
            return;
        } else if (byte == '=') {
            has_equal_ = true;
            state_ = ParserState::CsiParam;
            return;
        }
        state_ = ParserState::CsiParam;
    }

    if (byte >= '0' && byte <= '9') {
        if (current_subparam_idx_ >= 0) {
            auto& sp = params_[current_param_idx_].subparams[current_subparam_idx_];
            sp = (sp < 0) ? (byte - '0') : (sp * 10 + (byte - '0'));
        } else {
            auto& p = params_[current_param_idx_].value;
            p = (p < 0) ? (byte - '0') : (p * 10 + (byte - '0'));
        }
        param_count_ = std::max(param_count_, current_param_idx_ + 1);
        return;
    }

    if (byte == ';') {
        current_param_idx_++;
        current_subparam_idx_ = -1;
        if (current_param_idx_ >= 32) current_param_idx_ = 31;
        param_count_ = std::max(param_count_, current_param_idx_ + 1);
        return;
    }

    if (byte == ':') {
        if (current_subparam_idx_ < 7) {
            current_subparam_idx_++;
            params_[current_param_idx_].subparam_count = current_subparam_idx_ + 1;
        }
        return;
    }

    if (byte >= 0x20 && byte <= 0x2F) {
        intermediate_char_ = byte;
        state_ = ParserState::CsiIntermediate;
        return;
    }

    if (byte >= 0x40 && byte <= 0x7E) {
        handle_csi(byte);
        state_ = ParserState::Ground;
        return;
    }

    // Invalid character
    state_ = ParserState::CsiIgnore;
}

void VtParser::handle_csi(uint8_t final_char) {
    auto get_param = [this](int idx, int def) -> int {
        if (idx < param_count_ && params_[idx].value >= 0) {
            return params_[idx].value;
        }
        return def;
    };

    switch (final_char) {
        case 'H': // CUP
        case 'f': {
            int row = get_param(0, 1) - 1;
            int col = get_param(1, 1) - 1;
            engine_.set_cursor_pos(col, row);
            break;
        }
        case 'A': // CUU
            engine_.move_cursor_up(get_param(0, 1));
            break;
        case 'B': // CUD
            engine_.move_cursor_down(get_param(0, 1));
            break;
        case 'C': // CUF
            engine_.move_cursor_forward(get_param(0, 1));
            break;
        case 'D': // CUB
            engine_.move_cursor_backward(get_param(0, 1));
            break;
        case 'E': // CNL
            engine_.carriage_return();
            engine_.move_cursor_down(get_param(0, 1));
            break;
        case 'F': // CPL
            engine_.carriage_return();
            engine_.move_cursor_up(get_param(0, 1));
            break;
        case 'G': // CHA
            engine_.set_cursor_col(get_param(0, 1) - 1);
            break;
        case 'd': // VPA
            engine_.set_cursor_row(get_param(0, 1) - 1);
            break;
        case 'J': // ED
            engine_.erase_in_display(get_param(0, 0));
            break;
        case 'K': // EL
            engine_.erase_in_line(get_param(0, 0));
            break;
        case 'L': // IL
            engine_.insert_lines(get_param(0, 1));
            break;
        case 'M': // DL
            engine_.delete_lines(get_param(0, 1));
            break;
        case 'P': // DCH
            engine_.delete_chars(get_param(0, 1));
            break;
        case '@': // ICH
            engine_.insert_chars(get_param(0, 1));
            break;
        case 'X': // ECH
            engine_.erase_chars(get_param(0, 1));
            break;
        case 'S': // SU
            engine_.grid().scroll_up(engine_.margin_top(), engine_.margin_bottom(), get_param(0, 1));
            break;
        case 'T': // SD
            engine_.grid().scroll_down(engine_.margin_top(), engine_.margin_bottom(), get_param(0, 1));
            break;
        case 'r': // DECSTBM
            if (param_count_ == 0) {
                engine_.set_margins(0, engine_.rows() - 1);
            } else {
                int top = get_param(0, 1) - 1;
                int bottom = get_param(1, engine_.rows()) - 1;
                engine_.set_margins(top, bottom);
            }
            break;
        case 'g': // TBC
            engine_.clear_tab_stop(get_param(0, 0));
            break;
        case 'I': // CHT
            engine_.tab_forward(get_param(0, 1));
            break;
        case 'Z': // CBT
            engine_.tab_backward(get_param(0, 1));
            break;
        case 'h': // SM / DECSET
            handle_decset(true);
            break;
        case 'l': // RM / DECRST
            handle_decset(false);
            break;
        case 'm': // SGR
            handle_sgr();
            break;
        case 'u': // Kitty Keyboard Protocol
            if (has_greater_) { // Push
                engine_.push_kitty_keyboard_flags(static_cast<uint32_t>(get_param(0, 0)));
            } else if (has_less_) { // Pop
                engine_.pop_kitty_keyboard_flags(static_cast<size_t>(get_param(0, 1)));
            } else if (has_equal_) { // Set
                uint32_t flags = static_cast<uint32_t>(get_param(0, 0));
                int mode = get_param(1, 1);
                engine_.set_kitty_keyboard_flags(flags, mode);
            } else if (is_private_) { // Query
                char buf[32];
                std::snprintf(buf, sizeof(buf), "\x1b[?%uu", engine_.kitty_keyboard_flags());
                send_response(buf);
            }
            break;
        case 'q':
            if (intermediate_char_ == ' ') { // DECSCUSR cursor shape
                int shape = get_param(0, 1);
                switch (shape) {
                    case 0:
                    case 1: engine_.set_cursor_shape(CursorShape::Block, true); break;
                    case 2: engine_.set_cursor_shape(CursorShape::Block, false); break;
                    case 3: engine_.set_cursor_shape(CursorShape::Underline, true); break;
                    case 4: engine_.set_cursor_shape(CursorShape::Underline, false); break;
                    case 5: engine_.set_cursor_shape(CursorShape::Beam, true); break;
                    case 6: engine_.set_cursor_shape(CursorShape::Beam, false); break;
                    default: break;
                }
            }
            break;
        default:
            break;
    }
}

void VtParser::handle_decset(bool set) {
    if (!is_private_) {
        // Standard ANSI modes
        for (int i = 0; i < param_count_; ++i) {
            if (params_[i].value == 4) {
                engine_.set_insert_mode(set);
            }
        }
        return;
    }

    for (int i = 0; i < param_count_; ++i) {
        int code = params_[i].value;
        switch (code) {
            case 1: // DECCKM
                engine_.set_cursor_key_mode(set);
                break;
            case 6: // DECOM
                engine_.set_origin_mode(set);
                break;
            case 7: // DECAWM
                engine_.set_auto_wrap(set);
                break;
            case 25: // DECTCEM
                engine_.set_cursor_visible(set);
                break;
            case 47:
            case 1047:
            case 1049: // Alternate Screen Buffer
                engine_.set_alt_screen(set);
                break;
            case 1000:
                engine_.set_mouse_tracking(set ? MouseTrackingMode::Normal : MouseTrackingMode::None);
                break;
            case 1002:
                engine_.set_mouse_tracking(set ? MouseTrackingMode::ButtonEvent : MouseTrackingMode::None);
                break;
            case 1003:
                engine_.set_mouse_tracking(set ? MouseTrackingMode::AnyEvent : MouseTrackingMode::None);
                break;
            case 1005:
                engine_.set_mouse_format(set ? MouseFormat::Utf8 : MouseFormat::Normal);
                break;
            case 1006:
                engine_.set_mouse_format(set ? MouseFormat::Sgr : MouseFormat::Normal);
                break;
            case 1015:
                engine_.set_mouse_format(set ? MouseFormat::Urxvt : MouseFormat::Normal);
                break;
            case 1016:
                engine_.set_mouse_format(set ? MouseFormat::SgrPixels : MouseFormat::Normal);
                break;
            case 2004: // Bracketed paste
                engine_.set_bracketed_paste(set);
                break;
            case 2026: // Synchronized updates
                engine_.set_synchronized_updates(set);
                break;
            default:
                break;
        }
    }
}

void VtParser::handle_sgr() {
    if (param_count_ == 0) {
        engine_.sgr_reset();
        return;
    }

    for (int i = 0; i < param_count_; ++i) {
        const auto& p = params_[i];
        int val = p.value < 0 ? 0 : p.value;

        switch (val) {
            case 0: engine_.sgr_reset(); break;
            case 1: engine_.sgr_bold(true); break;
            case 2: engine_.sgr_dim(true); break;
            case 3: engine_.sgr_italic(true); break;
            case 4:
                if (p.subparam_count > 0 && p.subparams[0] >= 0) {
                    engine_.sgr_underline(static_cast<UnderlineStyle>(p.subparams[0]));
                } else {
                    engine_.sgr_underline(UnderlineStyle::Single);
                }
                break;
            case 5: engine_.sgr_blink(true); break;
            case 7: engine_.sgr_inverse(true); break;
            case 8: engine_.sgr_invisible(true); break;
            case 9: engine_.sgr_strikethrough(true); break;
            case 21: engine_.sgr_underline(UnderlineStyle::Double); break;
            case 22:
                engine_.sgr_bold(false);
                engine_.sgr_dim(false);
                break;
            case 23: engine_.sgr_italic(false); break;
            case 24: engine_.sgr_underline(UnderlineStyle::None); break;
            case 25: engine_.sgr_blink(false); break;
            case 27: engine_.sgr_inverse(false); break;
            case 28: engine_.sgr_invisible(false); break;
            case 29: engine_.sgr_strikethrough(false); break;

            // Standard ANSI colors
            case 30: case 31: case 32: case 33:
            case 34: case 35: case 36: case 37:
                engine_.sgr_fg_color(Color::make_indexed(static_cast<uint8_t>(val - 30)));
                break;
            case 39:
                engine_.sgr_fg_color(Color::make_default());
                break;
            case 40: case 41: case 42: case 43:
            case 44: case 45: case 46: case 47:
                engine_.sgr_bg_color(Color::make_indexed(static_cast<uint8_t>(val - 40)));
                break;
            case 49:
                engine_.sgr_bg_color(Color::make_default());
                break;
            case 90: case 91: case 92: case 93:
            case 94: case 95: case 96: case 97:
                engine_.sgr_fg_color(Color::make_indexed(static_cast<uint8_t>(val - 90 + 8)));
                break;
            case 100: case 101: case 102: case 103:
            case 104: case 105: case 106: case 107:
                engine_.sgr_bg_color(Color::make_indexed(static_cast<uint8_t>(val - 100 + 8)));
                break;

            // Extended colors (38, 48, 58)
            case 38:
            case 48:
            case 58: {
                auto apply_color = [&](Color c) {
                    if (val == 38) engine_.sgr_fg_color(c);
                    else if (val == 48) engine_.sgr_bg_color(c);
                    else engine_.sgr_underline_color(c);
                };

                // Check colon subparameters first (e.g. 38:2::r:g:b or 38:2:cs:r:g:b or 38:2:r:g:b or 38:5:idx)
                if (p.subparam_count >= 1) {
                    int type = p.subparams[0];
                    if (type == 5 && p.subparam_count >= 2 && p.subparams[1] >= 0) {
                        apply_color(Color::make_indexed(static_cast<uint8_t>(p.subparams[1])));
                    } else if (type == 2) {
                        if (p.subparam_count >= 5 && p.subparams[1] <= 0) {
                            // Format: 38:2::r:g:b or 38:2:cs:r:g:b
                            int r = p.subparams[2];
                            int g = p.subparams[3];
                            int b = p.subparams[4];
                            apply_color(Color::make_rgb(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)));
                        } else if (p.subparam_count >= 4) {
                            // Format: 38:2:r:g:b
                            int r = p.subparams[1];
                            int g = p.subparams[2];
                            int b = p.subparams[3];
                            apply_color(Color::make_rgb(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)));
                        }
                    }
                } else if (i + 1 < param_count_) {
                    int type = params_[i + 1].value;
                    if (type == 5 && i + 2 < param_count_) { // 38;5;n
                        apply_color(Color::make_indexed(static_cast<uint8_t>(params_[i + 2].value)));
                        i += 2;
                    } else if (type == 2 && i + 4 < param_count_) { // 38;2;r;g;b
                        uint8_t r = static_cast<uint8_t>(params_[i + 2].value);
                        uint8_t g = static_cast<uint8_t>(params_[i + 3].value);
                        uint8_t b = static_cast<uint8_t>(params_[i + 4].value);
                        apply_color(Color::make_rgb(r, g, b));
                        i += 4;
                    }
                }
                break;
            }
            case 59:
                engine_.sgr_underline_color(Color::make_default());
                break;
            default:
                break;
        }
    }
}

void VtParser::handle_osc() {
    size_t semicolon = string_buffer_.find(';');
    if (semicolon == std::string::npos) return;

    std::string_view cmd_str(string_buffer_.data(), semicolon);
    std::string_view payload(string_buffer_.data() + semicolon + 1, string_buffer_.size() - semicolon - 1);

    int cmd = -1;
    auto [ptr, ec] = std::from_chars(cmd_str.data(), cmd_str.data() + cmd_str.size(), cmd);
    if (ec != std::errc()) return;

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

} // namespace gritty::core
