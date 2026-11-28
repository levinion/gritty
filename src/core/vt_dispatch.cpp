#include "core/vt_parser.hpp"
#include "util/format.hpp"
#include "util/version.hpp"
#include <algorithm>

namespace gritty::core {

namespace {
constexpr int kSavedPrivateModes[] = {
    1, 5, 6, 7, 12, 25, 47, 1000, 1002, 1003, 1004, 1005, 1006, 1007,
    1015, 1016, 1047, 1049, 2004, 2026, 2027, 2031, 2048
};
} // namespace

// Prefixes/intermediates are separate dispatch namespaces, as in foot csi.c.
// In particular, CSI > 4 ; 2 m must never be interpreted as SGR 4;2.
void VtParser::handle_csi(uint8_t final) {
    if (final == 'c' || final == 'n' || final == 't' || final == 'p' ||
        (final == 'q' && has_greater_)) {
        handle_query(final);
        return;
    }
    if (final == 'u' && intermediate_char_ == 0) {
        if (has_greater_) engine_.push_kitty_keyboard_flags(get_param(0, 0));
        else if (has_less_) engine_.pop_kitty_keyboard_flags(get_param(0, 1));
        else if (has_equal_) engine_.set_kitty_keyboard_flags(get_param(0, 0), get_param(1, 1));
        else if (is_private_) {
            char reply[32];
            const auto text = util::format_to_buffer(
                reply, "\x1b[?{}u", engine_.kitty_keyboard_flags());
            if (!text.empty()) send_response(text);
        } else engine_.restore_cursor();
        return;
    }
    if (has_greater_ && final == 'm' && intermediate_char_ == 0) {
        if (get_param(0, 0) == 4) engine_.set_modify_other_keys(get_param(1, 0));
        return;
    }
    if (has_greater_ || has_less_ || has_equal_) return;
    if (is_private_) {
        if (intermediate_char_ == 0) {
            if (final == 'h' || final == 'l') handle_decset(final == 'h');
            else if (final == 's') {
                if (param_count_ == 0) {
                    for (const int code : kSavedPrivateModes) {
                        engine_.save_dec_mode(code, dec_mode_status(code) == 1);
                    }
                } else {
                    for (int i = 0; i < param_count_; ++i) {
                        const int code = params_[i].value;
                        const int status = dec_mode_status(code);
                        if (status != 0) engine_.save_dec_mode(code, status == 1);
                    }
                }
            } else if (final == 'r') {
                if (param_count_ == 0) {
                    for (const int code : kSavedPrivateModes) {
                        if (const auto saved = engine_.saved_dec_mode(code)) {
                            apply_dec_mode(code, *saved);
                        }
                    }
                } else {
                    for (int i = 0; i < param_count_; ++i) {
                        const int code = params_[i].value;
                        if (const auto saved = engine_.saved_dec_mode(code)) {
                            apply_dec_mode(code, *saved);
                        }
                    }
                }
            }
        }
        return;
    }
    if (intermediate_char_) {
        if (intermediate_char_ == ' ' && final == 'q') {
            const auto shape = get_param(0, 1);
            if (shape <= 6) engine_.set_cursor_shape(
                shape <= 2 ? CursorShape::Block : shape <= 4 ? CursorShape::Underline : CursorShape::Beam,
                shape % 2 != 0);
        }
        return;
    }
    switch (final) {
        case 'H': case 'f': engine_.set_cursor_pos(get_param(1, 1) - 1, get_param(0, 1) - 1); break;
        case 'A': engine_.move_cursor_up(get_param(0, 1)); break;
        case 'B': case 'e': engine_.move_cursor_down(get_param(0, 1)); break;
        case 'C': case 'a': engine_.move_cursor_forward(get_param(0, 1)); break;
        case 'D': engine_.move_cursor_backward(get_param(0, 1)); break;
        case 'E': engine_.carriage_return(); engine_.move_cursor_down(get_param(0, 1)); break;
        case 'F': engine_.carriage_return(); engine_.move_cursor_up(get_param(0, 1)); break;
        case 'G': case '`': engine_.set_cursor_col(get_param(0, 1) - 1); break;
        case 'd': engine_.set_cursor_row(get_param(0, 1) - 1); break;
        case 'J': engine_.erase_in_display(get_param(0, 0)); break;
        case 'K': engine_.erase_in_line(get_param(0, 0)); break;
        case 'L': engine_.insert_lines(get_param(0, 1)); break;
        case 'M': engine_.delete_lines(get_param(0, 1)); break;
        case 'P': engine_.delete_chars(get_param(0, 1)); break;
        case '@': engine_.insert_chars(get_param(0, 1)); break;
        case 'X': engine_.erase_chars(get_param(0, 1)); break;
        case 'S': engine_.scroll_up(get_param(0, 1)); break;
        case 'T': engine_.scroll_down(get_param(0, 1)); break;
        case 'r': engine_.set_margins(get_param(0, 1) - 1, get_param(1, engine_.rows()) - 1); break;
        case 'g': engine_.clear_tab_stop(get_param(0, 0)); break;
        case 'I': engine_.tab_forward(std::min(get_param(0, 1), engine_.cols())); break;
        case 'Z': engine_.tab_backward(std::min(get_param(0, 1), engine_.cols())); break;
        case 'h': handle_decset(true); break;
        case 'l': handle_decset(false); break;
        case 'm': handle_sgr(); break;
        case 's': engine_.save_cursor(); break;
        case 'b': engine_.repeat_last_char(get_param(0, 1)); break;
        default: break;
    }
}

void VtParser::apply_dec_mode(int code, bool set) {
    const auto tracking = [&](MouseTrackingMode mode) {
        if (set) engine_.set_mouse_tracking(mode);
        else if (engine_.mouse_tracking() == mode) engine_.set_mouse_tracking(MouseTrackingMode::None);
    };
    const auto format = [&](MouseFormat mode) {
        if (set) engine_.set_mouse_format(mode);
        else if (engine_.mouse_format() == mode) engine_.set_mouse_format(MouseFormat::Normal);
    };
    switch (code) {
        case 1: engine_.set_cursor_key_mode(set); break;
        case 5: engine_.set_reverse_video(set); break;
        case 6: engine_.set_origin_mode(set); break;
        case 7: engine_.set_auto_wrap(set); break;
        case 12: engine_.set_cursor_shape(engine_.cursor_shape(), set); break;
        case 25: engine_.set_cursor_visible(set); break;
        case 47: case 1047: case 1049: engine_.set_alt_screen(set, code); break;
        case 1048: if (set) engine_.save_cursor(); else engine_.restore_cursor(); break;
        case 1000: tracking(MouseTrackingMode::Normal); break;
        case 1002: tracking(MouseTrackingMode::ButtonEvent); break;
        case 1003: tracking(MouseTrackingMode::AnyEvent); break;
        case 1005: format(MouseFormat::Utf8); break;
        case 1006: format(MouseFormat::Sgr); break;
        case 1007: engine_.set_alternate_scroll(set); break;
        case 1015: format(MouseFormat::Urxvt); break;
        case 1016: format(MouseFormat::SgrPixels); break;
        case 1004: engine_.set_focus_events(set); break;
        case 2004: engine_.set_bracketed_paste(set); break;
        case 2026:
            engine_.set_synchronized_updates(set);
            if (sync_handler_) sync_handler_(set);
            break;
        case 2027: engine_.set_grapheme_clustering(set); break;
        case 2031: engine_.set_color_scheme_updates(set); break;
        case 2048:
            engine_.set_in_band_resize(set);
            if (set) send_in_band_resize_report();
            break;
        default: break;
    }
}

void VtParser::handle_decset(bool set) {
    for (int i = 0; i < param_count_; ++i) {
        const int code = params_[i].value;
        if (!is_private_) {
            if (code == 4) engine_.set_insert_mode(set);
            continue;
        }
        apply_dec_mode(code, set);
    }
}

int VtParser::dec_mode_status(int mode) const noexcept {
    bool set;
    switch (mode) {
        case 1: set = engine_.cursor_key_mode(); break;
        case 5: set = engine_.reverse_video(); break;
        case 6: set = engine_.origin_mode(); break;
        case 7: set = engine_.auto_wrap(); break;
        case 12: set = engine_.cursor_blink(); break;
        case 25: set = engine_.cursor_visible(); break;
        case 47: case 1047: case 1049: set = engine_.is_alt_screen(); break;
        case 1000: set = engine_.mouse_tracking() == MouseTrackingMode::Normal; break;
        case 1002: set = engine_.mouse_tracking() == MouseTrackingMode::ButtonEvent; break;
        case 1003: set = engine_.mouse_tracking() == MouseTrackingMode::AnyEvent; break;
        case 1005: set = engine_.mouse_format() == MouseFormat::Utf8; break;
        case 1006: set = engine_.mouse_format() == MouseFormat::Sgr; break;
        case 1007: set = engine_.alternate_scroll(); break;
        case 1015: set = engine_.mouse_format() == MouseFormat::Urxvt; break;
        case 1016: set = engine_.mouse_format() == MouseFormat::SgrPixels; break;
        case 1004: set = engine_.focus_events(); break;
        case 2004: set = engine_.bracketed_paste(); break;
        case 2026: set = engine_.synchronized_updates(); break;
        case 2027: set = engine_.grapheme_clustering(); break;
        case 2031: set = engine_.color_scheme_updates(); break;
        case 2048: set = engine_.in_band_resize(); break;
        default: return 0; // Do not advertise capabilities we have not implemented.
    }
    return set ? 1 : 2;
}

void VtParser::handle_query(uint8_t final) {
    std::string_view reply;
    char reply_buffer[128];
    if (final == 'p' && intermediate_char_ == '$' && is_private_) {
        reply = util::format_to_buffer(reply_buffer, "\x1b[?{};{}$y",
            get_param(0, 0), dec_mode_status(get_param(0, 0)));
    } else if (intermediate_char_ || has_less_ || has_equal_) return;
    else if (has_greater_) {
        if (get_param(0, 0) != 0) return;
        if (final == 'c') send_response("\x1b[>1;100;0c");
        // XTVERSION: advertise the Meson project version so tmux can detect
        // gritty without extra configuration.
        else if (final == 'q') {
            const auto text = util::format_to_buffer(
                reply_buffer, "\x1bP>|gritty({})\x1b\\", util::kVersion);
            if (!text.empty()) send_response(text);
        }
        return;
    } else if (final == 'n') {
        const int query = get_param(0, 0);
        if (query == 5 && !is_private_) send_response("\x1b[0n");
        else if (query == 6) {
            const int row = engine_.cursor_row() - (engine_.origin_mode() ? engine_.margin_top() : 0) + 1;
            reply = util::format_to_buffer(reply_buffer, "\x1b[{}{};{}R",
                is_private_ ? std::string_view("?") : std::string_view(),
                row, engine_.cursor_col() + 1);
        } else if (query == 996 && is_private_) {
            send_color_scheme_report();
        }
    } else if (!is_private_ && final == 'c' && get_param(0, 0) == 0) {
        // VT220 + ANSI color only. Image support is advertised via Kitty queries.
        send_response("\x1b[?62;22c");
    } else if (!is_private_ && final == 't') {
        const int op = get_param(0, 0);
        if (op == 14 && engine_.cell_width() > 0 && engine_.cell_height() > 0) {
            reply = util::format_to_buffer(reply_buffer, "\x1b[4;{};{}t",
                engine_.rows() * engine_.cell_height(),
                engine_.cols() * engine_.cell_width());
        } else if (op == 16 && engine_.cell_width() > 0 && engine_.cell_height() > 0) {
            reply = util::format_to_buffer(reply_buffer, "\x1b[6;{};{}t",
                engine_.cell_height(), engine_.cell_width());
        } else if (op == 18) {
            reply = util::format_to_buffer(reply_buffer, "\x1b[8;{};{}t",
                engine_.rows(), engine_.cols());
        } else if (op == 22) {
            const int which = get_param(1, 0);
            if (which == 0 || which == 2) engine_.push_window_title();
        } else if (op == 23) {
            const int which = get_param(1, 0);
            if (which == 0 || which == 2) engine_.pop_window_title();
        }
    }
    if (!reply.empty()) send_response(reply);
}

} // namespace gritty::core
