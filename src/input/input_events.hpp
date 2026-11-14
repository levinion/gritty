#pragma once

#include <cstdint>
#include <string>

namespace squirt::input {

enum class KeyAction : uint8_t {
    Press,
    Release,
    Repeat,
};

struct Modifiers {
    bool shift{false};
    bool alt{false};
    bool ctrl{false};
    bool meta{false}; // Command on macOS / Super on Linux

    [[nodiscard]] constexpr uint32_t to_kitty_mods() const noexcept {
        uint32_t m = 0;
        if (shift) m |= 1;
        if (alt)   m |= 2;
        if (ctrl)  m |= 4;
        if (meta)  m |= 8;
        return m;
    }
};

enum class KeyCode : uint32_t {
    Unknown = 0,
    Escape = 27,
    Return = 13,
    Tab = 9,
    Backspace = 8,
    Delete = 127,

    // Arrow keys
    Up = 1000,
    Down,
    Left,
    Right,

    // Navigation
    Home,
    End,
    PageUp,
    PageDown,
    Insert,

    // Function keys
    F1 = 1100,
    F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12
};

struct KeyEvent {
    KeyCode key{KeyCode::Unknown};
    char32_t codepoint{0};
    Modifiers mods{};
    KeyAction action{KeyAction::Press};
    std::string text;
};

enum class MouseButton : uint8_t {
    None = 0,
    Left = 1,
    Middle = 2,
    Right = 3,
    WheelUp = 4,
    WheelDown = 5,
};

enum class MouseAction : uint8_t {
    Press,
    Release,
    Move,
};

struct MouseEvent {
    MouseButton button{MouseButton::None};
    MouseAction action{MouseAction::Press};
    int col{0};
    int row{0};
    int pixel_x{0};
    int pixel_y{0};
    Modifiers mods{};
};

} // namespace squirt::input
