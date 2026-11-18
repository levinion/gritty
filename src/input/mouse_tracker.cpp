#include "input/input_mapper.hpp"
#include "util/format.hpp"
#include <algorithm>
#include <utf8proc.h>

namespace gritty::input {

std::string InputMapper::map_mouse(const MouseEvent& event, const core::TerminalEngine& engine) {
    const auto tracking = engine.mouse_tracking();
    if (tracking == core::MouseTrackingMode::None || event.col < 0 || event.row < 0) return {};
    const bool release = event.action == MouseAction::Release;
    const bool wheel = event.button == MouseButton::WheelUp || event.button == MouseButton::WheelDown;
    if (release && (wheel || tracking == core::MouseTrackingMode::X10)) return {};
    if (event.action == MouseAction::Move) {
        // foot terminal.c: click-only mode never reports motion; drag mode
        // reports it only while a button is held.
        if (tracking == core::MouseTrackingMode::Normal || tracking == core::MouseTrackingMode::X10 ||
            (tracking == core::MouseTrackingMode::ButtonEvent && event.button == MouseButton::None)) return {};
    } else if (event.button == MouseButton::None) return {};

    int button = 3;
    switch (event.button) {
        case MouseButton::Left: button = 0; break;
        case MouseButton::Middle: button = 1; break;
        case MouseButton::Right: button = 2; break;
        case MouseButton::WheelUp: button = 64; break;
        case MouseButton::WheelDown: button = 65; break;
        default: break;
    }
    if (event.action == MouseAction::Move) button += 32;
    button += 4 * event.mods.shift + 8 * event.mods.alt + 16 * event.mods.ctrl;
    char sequence[96];
    switch (engine.mouse_format()) {
        case core::MouseFormat::Sgr:
        case core::MouseFormat::SgrPixels: {
            const bool pixels = engine.mouse_format() == core::MouseFormat::SgrPixels;
            const int x = pixels ? std::max(0, event.pixel_x) : event.col;
            const int y = pixels ? std::max(0, event.pixel_y) : event.row;
            const auto text = util::format_to_buffer(
                sequence, "\x1b[<{};{};{}{}", button, x + 1, y + 1, release ? 'm' : 'M');
            return std::string(text);
        }
        case core::MouseFormat::Urxvt: {
            const auto text = util::format_to_buffer(sequence, "\x1b[{};{};{}M",
                32 + (release ? 3 : button), event.col + 1, event.row + 1);
            return std::string(text);
        }
        case core::MouseFormat::Normal:
            if (event.col > 222 || event.row > 222) return {};
            return std::string("\x1b[M") + static_cast<char>(32 + (release ? 3 : button)) +
                static_cast<char>(33 + event.col) + static_cast<char>(33 + event.row);
        case core::MouseFormat::Utf8: {
            // UTF-8 coordinates are a legacy extension beyond foot's formats.
            if (event.col > 2014 || event.row > 2014) return {};
            std::string result = "\x1b[M";
            for (int cp : {32 + (release ? 3 : button), 33 + event.col, 33 + event.row}) {
                uint8_t bytes[4];
                const auto count = utf8proc_encode_char(cp, bytes);
                result.append(reinterpret_cast<char*>(bytes), count);
            }
            return result;
        }
    }
    return {};
}

} // namespace gritty::input
