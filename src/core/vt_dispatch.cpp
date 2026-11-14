#include "core/vt_parser.hpp"
#include <algorithm>
#include <cstdio>

namespace gritty::core {

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
            std::snprintf(reply, sizeof(reply), "\x1b[?%uu", engine_.kitty_keyboard_flags());
            send_response(reply);
        } else engine_.restore_cursor();
        return;
    }
    if (has_greater_ && final == 'm' && intermediate_char_ == 0) {
        if (get_param(0, 0) == 4) engine_.set_modify_other_keys(get_param(1, 0));
        return;
    }
    if (has_greater_ || has_less_ || has_equal_) return;
    if (is_private_) {
        if (intermediate_char_ == 0 && (final == 'h' || final == 'l')) handle_decset(final == 'h');
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

void VtParser::handle_decset(bool set) {
    const auto tracking = [&](MouseTrackingMode mode) {
        if (set) engine_.set_mouse_tracking(mode);
        else if (engine_.mouse_tracking() == mode) engine_.set_mouse_tracking(MouseTrackingMode::None);
    };
    const auto format = [&](MouseFormat mode) {
        if (set) engine_.set_mouse_format(mode);
        else if (engine_.mouse_format() == mode) engine_.set_mouse_format(MouseFormat::Normal);
    };
    for (int i = 0; i < param_count_; ++i) {
        const int code = params_[i].value;
        if (!is_private_) {
            if (code == 4) engine_.set_insert_mode(set);
            continue;
        }
        switch (code) {
            case 1: engine_.set_cursor_key_mode(set); break;
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
            case 1015: format(MouseFormat::Urxvt); break;
            case 1016: format(MouseFormat::SgrPixels); break;
            case 2004: engine_.set_bracketed_paste(set); break;
            case 2026:
                engine_.set_synchronized_updates(set);
                if (sync_handler_) sync_handler_(set);
                break;
            default: break;
        }
    }
}

int VtParser::dec_mode_status(int mode) const noexcept {
    bool set;
    switch (mode) {
        case 1: set = engine_.cursor_key_mode(); break;
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
        case 1015: set = engine_.mouse_format() == MouseFormat::Urxvt; break;
        case 1016: set = engine_.mouse_format() == MouseFormat::SgrPixels; break;
        case 2004: set = engine_.bracketed_paste(); break;
        case 2026: set = engine_.synchronized_updates(); break;
        default: return 0; // Do not advertise capabilities we have not implemented.
    }
    return set ? 1 : 2;
}

void VtParser::handle_query(uint8_t final) {
    char reply[128];
    reply[0] = '\0';
    if (final == 'p' && intermediate_char_ == '$' && is_private_) {
        std::snprintf(reply, sizeof(reply), "\x1b[?%d;%d$y", get_param(0, 0), dec_mode_status(get_param(0, 0)));
    } else if (intermediate_char_ || has_less_ || has_equal_) return;
    else if (has_greater_) {
        if (get_param(0, 0) != 0) return;
        if (final == 'c') send_response("\x1b[>1;100;0c");
        // Never impersonate foot: tmux would enable its unsupported features.
        else if (final == 'q') send_response("\x1bP>|gritty(0.1.0)\x1b\\");
        return;
    } else if (final == 'n') {
        const int query = get_param(0, 0);
        if (query == 5 && !is_private_) send_response("\x1b[0n");
        else if (query == 6) {
            const int row = engine_.cursor_row() - (engine_.origin_mode() ? engine_.margin_top() : 0) + 1;
            std::snprintf(reply, sizeof(reply), "\x1b[%s%d;%dR", is_private_ ? "?" : "", row, engine_.cursor_col() + 1);
        }
    } else if (!is_private_ && final == 'c' && get_param(0, 0) == 0) {
        // VT220 with ANSI color, but no rectangular editing/Sixel/clipboard claims.
        send_response("\x1b[?62;22c");
    } else if (!is_private_ && final == 't' && get_param(0, 0) == 18) {
        std::snprintf(reply, sizeof(reply), "\x1b[8;%d;%dt", engine_.rows(), engine_.cols());
    }
    if (reply[0]) send_response(reply);
}

} // namespace gritty::core
