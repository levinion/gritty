#include "input/input_mapper.hpp"
#include <cstdio>

namespace gritty::input {

namespace {

int xterm_mod_param(const Modifiers& m) noexcept {
    // xterm modifier encoding: 1 + (shift*1) + (alt*2) + (ctrl*4) + (meta*8)
    int p = 1;
    if (m.shift) p += 1;
    if (m.alt)   p += 2;
    if (m.ctrl)  p += 4;
    if (m.meta)  p += 8;
    return p;
}

} // namespace

std::string InputMapper::map_key(const KeyEvent& ev, const core::TerminalEngine& engine) {
    if (ev.action == KeyAction::Release) {
        // Only Kitty protocol reports releases if requested
        uint32_t kflags = engine.kitty_keyboard_flags();
        if (!(kflags & 2)) return ""; // Event types not enabled
    }

    uint32_t kflags = engine.kitty_keyboard_flags();

    // 1. Kitty Keyboard Protocol progressive enhancement (CSI u)
    if (kflags != 0) {
        uint32_t codepoint = ev.codepoint;
        if (codepoint == 0) {
            switch (ev.key) {
                case KeyCode::Escape: codepoint = 27; break;
                case KeyCode::Return: codepoint = 13; break;
                case KeyCode::Tab: codepoint = 9; break;
                case KeyCode::Backspace: codepoint = 127; break;
                case KeyCode::Delete: codepoint = 57350; break;
                case KeyCode::Up: codepoint = 57352; break;
                case KeyCode::Down: codepoint = 57353; break;
                case KeyCode::Left: codepoint = 57354; break;
                case KeyCode::Right: codepoint = 57355; break;
                default: break;
            }
        }

        if (codepoint != 0) {
            uint32_t kmods = ev.mods.to_kitty_mods();
            int event_type = (ev.action == KeyAction::Press) ? 1 : ((ev.action == KeyAction::Repeat) ? 2 : 3);

            char buf[64];
            if (kflags & 2) { // Report event types
                std::snprintf(buf, sizeof(buf), "\x1b[%u;%u:%uu", codepoint, kmods + 1, event_type);
            } else if (kmods > 0) {
                std::snprintf(buf, sizeof(buf), "\x1b[%u;%uu", codepoint, kmods + 1);
            } else {
                std::snprintf(buf, sizeof(buf), "\x1b[%uu", codepoint);
            }
            return std::string(buf);
        }
    }

    // 2. Standard Legacy / XTerm mapping
    int mod_param = xterm_mod_param(ev.mods);
    bool has_mod = (mod_param > 1);

    // Arrow keys
    if (ev.key == KeyCode::Up || ev.key == KeyCode::Down ||
        ev.key == KeyCode::Right || ev.key == KeyCode::Left) {
        char suffix = 'A';
        if (ev.key == KeyCode::Up) suffix = 'A';
        else if (ev.key == KeyCode::Down) suffix = 'B';
        else if (ev.key == KeyCode::Right) suffix = 'C';
        else if (ev.key == KeyCode::Left) suffix = 'D';

        char buf[32];
        if (has_mod) {
            std::snprintf(buf, sizeof(buf), "\x1b[1;%d%c", mod_param, suffix);
        } else if (engine.cursor_key_mode()) { // DECCKM application mode
            std::snprintf(buf, sizeof(buf), "\x1bO%c", suffix);
        } else {
            std::snprintf(buf, sizeof(buf), "\x1b[%c", suffix);
        }
        return std::string(buf);
    }

    // Function keys
    if (ev.key >= KeyCode::F1 && ev.key <= KeyCode::F12) {
        int fnum = static_cast<int>(ev.key) - static_cast<int>(KeyCode::F1) + 1;
        char buf[32];
        if (fnum <= 4 && !has_mod) {
            std::snprintf(buf, sizeof(buf), "\x1bO%c", 'P' + (fnum - 1));
        } else {
            static const int vt_codes[] = {11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 23, 24};
            int code = vt_codes[fnum - 1];
            if (has_mod) {
                std::snprintf(buf, sizeof(buf), "\x1b[%d;%d~", code, mod_param);
            } else {
                std::snprintf(buf, sizeof(buf), "\x1b[%d~", code);
            }
        }
        return std::string(buf);
    }

    // Navigation keys
    switch (ev.key) {
        case KeyCode::Home:
            return has_mod ? ("\x1b[1;" + std::to_string(mod_param) + "H") : "\x1b[H";
        case KeyCode::End:
            return has_mod ? ("\x1b[1;" + std::to_string(mod_param) + "F") : "\x1b[F";
        case KeyCode::Insert:
            return has_mod ? ("\x1b[2;" + std::to_string(mod_param) + "~") : "\x1b[2~";
        case KeyCode::Delete:
            return has_mod ? ("\x1b[3;" + std::to_string(mod_param) + "~") : "\x1b[3~";
        case KeyCode::PageUp:
            return has_mod ? ("\x1b[5;" + std::to_string(mod_param) + "~") : "\x1b[5~";
        case KeyCode::PageDown:
            return has_mod ? ("\x1b[6;" + std::to_string(mod_param) + "~") : "\x1b[6~";
        case KeyCode::Escape:
            return "\x1b";
        case KeyCode::Return:
            return ev.mods.alt ? "\x1b\r" : "\r";
        case KeyCode::Tab:
            if (ev.mods.shift) return "\x1b[Z"; // Back-tab
            return ev.mods.alt ? "\x1b\t" : "\t";
        case KeyCode::Backspace:
            return ev.mods.alt ? "\x1b\x7f" : "\x7f";
        default:
            break;
    }

    // Ctrl+Letter
    if (ev.mods.ctrl && ev.codepoint >= 'a' && ev.codepoint <= 'z') {
        char c = static_cast<char>(ev.codepoint - 'a' + 1);
        return ev.mods.alt ? std::string("\x1b") + c : std::string(1, c);
    }
    if (ev.mods.ctrl && ev.codepoint >= 'A' && ev.codepoint <= 'Z') {
        char c = static_cast<char>(ev.codepoint - 'A' + 1);
        return ev.mods.alt ? std::string("\x1b") + c : std::string(1, c);
    }

    // Normal text
    if (!ev.text.empty()) {
        if (ev.mods.alt) {
            return "\x1b" + ev.text;
        }
        return ev.text;
    }

    return "";
}

std::string InputMapper::map_mouse(const MouseEvent& ev, const core::TerminalEngine& engine) {
    if (engine.mouse_tracking() == core::MouseTrackingMode::None) {
        return "";
    }

    // SGR (1006) encoding
    int button_code = 0;
    switch (ev.button) {
        case MouseButton::Left: button_code = 0; break;
        case MouseButton::Middle: button_code = 1; break;
        case MouseButton::Right: button_code = 2; break;
        case MouseButton::WheelUp: button_code = 64; break;
        case MouseButton::WheelDown: button_code = 65; break;
        default: button_code = 3; break;
    }

    if (ev.action == MouseAction::Move) {
        button_code += 32;
    }

    // Modifiers in mouse events
    if (ev.mods.shift) button_code += 4;
    if (ev.mods.alt)   button_code += 8;
    if (ev.mods.ctrl)  button_code += 16;

    char final_char = (ev.action == MouseAction::Release) ? 'm' : 'M';
    char buf[64];

    if (engine.mouse_format() == core::MouseFormat::SgrPixels) {
        std::snprintf(buf, sizeof(buf), "\x1b[<%d;%d;%d%c", button_code, ev.pixel_x + 1, ev.pixel_y + 1, final_char);
    } else { // Standard SGR 1006
        std::snprintf(buf, sizeof(buf), "\x1b[<%d;%d;%d%c", button_code, ev.col + 1, ev.row + 1, final_char);
    }

    return std::string(buf);
}

} // namespace gritty::input
