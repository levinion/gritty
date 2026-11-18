#include "input/input_mapper.hpp"
#include "util/format.hpp"
#include <utility>
#include <utf8proc.h>

namespace gritty::input {
namespace {

int xterm_mod_param(const Modifiers& mods) noexcept {
    return 1 + mods.shift + 2 * mods.alt + 4 * mods.ctrl + 8 * mods.meta;
}

std::pair<uint32_t, char> kitty_key(const KeyEvent& event) {
    switch (event.key) {
        case KeyCode::Escape: return {27, 'u'};
        case KeyCode::Return: return {13, 'u'};
        case KeyCode::Tab: return {9, 'u'};
        case KeyCode::Backspace: return {127, 'u'};
        case KeyCode::Up: return {1, 'A'};
        case KeyCode::Down: return {1, 'B'};
        case KeyCode::Right: return {1, 'C'};
        case KeyCode::Left: return {1, 'D'};
        case KeyCode::Home: return {1, 'H'};
        case KeyCode::End: return {1, 'F'};
        case KeyCode::Insert: return {2, '~'};
        case KeyCode::Delete: return {3, '~'};
        case KeyCode::PageUp: return {5, '~'};
        case KeyCode::PageDown: return {6, '~'};
        default: break;
    }
    if (event.key >= KeyCode::F1 && event.key <= KeyCode::F12) {
        const auto index = static_cast<int>(event.key) - static_cast<int>(KeyCode::F1);
        constexpr uint32_t codes[] = {1, 1, 1, 1, 15, 17, 18, 19, 20, 21, 23, 24};
        return {codes[index], index < 4 ? static_cast<char>('P' + index) : '~'};
    }
    return {event.base_codepoint ? event.base_codepoint : event.codepoint, 'u'};
}

bool printable_text(std::string_view text) {
    if (text.empty()) return false;
    while (!text.empty()) {
        utf8proc_int32_t cp;
        const auto n = utf8proc_iterate(reinterpret_cast<const uint8_t*>(text.data()), text.size(), &cp);
        if (n <= 0 || cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) return false;
        text.remove_prefix(n);
    }
    return true;
}

} // namespace

std::string InputMapper::map_key(const KeyEvent& ev, const core::TerminalEngine& engine) {
    const auto flags = engine.kitty_keyboard_flags();
    const bool released = ev.action == KeyAction::Release;
    if (released && !(flags & 2)) return "";

    // foot input.c: disambiguation does NOT imply report-all. Plain text and
    // unmodified Enter/Tab/Backspace keep their legacy form unless flag 8 is set.
    if (flags && ((flags & (1 | 8)) || ev.action != KeyAction::Press)) {
        const bool plain = ev.key == KeyCode::Unknown && !ev.mods.ctrl && !ev.mods.alt &&
            !ev.mods.meta && printable_text(ev.text);
        const bool basic = !ev.mods.ctrl && !ev.mods.alt && !ev.mods.meta && !ev.mods.shift &&
            (ev.key == KeyCode::Return || ev.key == KeyCode::Tab || ev.key == KeyCode::Backspace);
        if (!(flags & 8) && plain && !released) return ev.text;
        if (!(flags & 8) && basic) {
            if (released) return "";
            return ev.key == KeyCode::Return ? "\r" : ev.key == KeyCode::Tab ? "\t" : "\x7f";
        }
        auto [key, final] = kitty_key(ev);
        if (key == 0) return "";
        const auto mods = ev.mods.to_kitty_mods() + 1;
        std::string bytes = "\x1b[";
        if (final == 'u' || final == '~') {
            bytes += std::to_string(key);
            if ((flags & 4) && ev.key == KeyCode::Unknown && ev.codepoint && ev.codepoint != key)
                bytes += ":" + std::to_string(ev.codepoint);
        } else if (mods > 1 || (flags & 2)) bytes += "1";
        const bool associated = (flags & 16) && plain && !released && (final == 'u' || final == '~');
        const bool emit_modifiers = mods > 1 || (flags & 2);
        if (emit_modifiers) {
            bytes += ";" + std::to_string(mods);
            if (flags & 2) bytes += ev.action == KeyAction::Press ? ":1" : released ? ":3" : ":2";
        }
        if (associated) {
            if (!emit_modifiers) bytes += ';';
            std::string_view text = ev.text;
            char separator = ';';
            while (!text.empty()) {
                utf8proc_int32_t cp;
                const auto n = utf8proc_iterate(reinterpret_cast<const uint8_t*>(text.data()), text.size(), &cp);
                if (n <= 0) break;
                bytes += separator;
                bytes += std::to_string(cp);
                separator = ':';
                text.remove_prefix(n);
            }
        }
        bytes += final;
        return bytes;
    }
    if (released) return "";

    // XTerm modifyOtherKeys mode 2 is separate from Kitty CSI-u.
    // Like foot, Shift-only text needs encoding for Latin letters, not punctuation.
    const auto mods = xterm_mod_param(ev.mods);
    if (engine.modify_other_keys() == 2 && mods > 1) {
        uint32_t cp = ev.codepoint;
        if (ev.key == KeyCode::Return) cp = 13;
        else if (ev.key == KeyCode::Tab) cp = 9;
        else if (ev.key == KeyCode::Backspace) cp = 127;
        else if (ev.key == KeyCode::Escape) cp = 27;
        else if (ev.key != KeyCode::Unknown) cp = 0;
        const bool encode = ev.mods.ctrl || ev.mods.alt || ev.mods.meta ||
            (ev.mods.shift && ((cp >= 'A' && cp <= 'Z') || ev.key != KeyCode::Unknown));
        if (cp && encode) {
            char sequence[64];
            const auto text = util::format_to_buffer(sequence, "\x1b[27;{};{}~", mods, cp);
            return std::string(text);
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
        std::string_view text;
        if (has_mod) {
            text = util::format_to_buffer(buf, "\x1b[1;{}{}", mod_param, suffix);
        } else if (engine.cursor_key_mode()) { // DECCKM application mode
            text = util::format_to_buffer(buf, "\x1bO{}", suffix);
        } else {
            text = util::format_to_buffer(buf, "\x1b[{}", suffix);
        }
        return std::string(text);
    }

    // Function keys
    if (ev.key >= KeyCode::F1 && ev.key <= KeyCode::F12) {
        int fnum = static_cast<int>(ev.key) - static_cast<int>(KeyCode::F1) + 1;
        char buf[32];
        std::string_view text;
        if (fnum <= 4) {
            const char suffix = static_cast<char>('P' + fnum - 1);
            if (has_mod) text = util::format_to_buffer(buf, "\x1b[1;{}{}", mod_param, suffix);
            else text = util::format_to_buffer(buf, "\x1bO{}", suffix);
        } else {
            static const int vt_codes[] = {11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 23, 24};
            int code = vt_codes[fnum - 1];
            if (has_mod) text = util::format_to_buffer(buf, "\x1b[{};{}~", code, mod_param);
            else text = util::format_to_buffer(buf, "\x1b[{}~", code);
        }
        return std::string(text);
    }

    // Navigation keys
    switch (ev.key) {
        case KeyCode::Home:
            return has_mod ? ("\x1b[1;" + std::to_string(mod_param) + "H") : engine.cursor_key_mode() ? "\x1bOH" : "\x1b[H";
        case KeyCode::End:
            return has_mod ? ("\x1b[1;" + std::to_string(mod_param) + "F") : engine.cursor_key_mode() ? "\x1bOF" : "\x1b[F";
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

    // Remaining ASCII control chords, including Ctrl+Space and Ctrl+[.
    if (ev.mods.ctrl) {
        const auto cp = ev.codepoint;
        char control;
        if (cp == ' ' || cp == '@' || cp == '2') control = 0;
        else if (cp >= '[' && cp <= '_') control = static_cast<char>(cp - '@');
        else if (cp == '?' || cp == '8') control = 127;
        else return ev.text;
        return (ev.mods.alt ? std::string("\x1b") : std::string{}) + std::string(1, control);
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

} // namespace gritty::input
