#include "platform/macos/mac_input.hpp"
#import <Cocoa/Cocoa.h>

namespace gritty::platform::macos {

input::Modifiers MacInput::translate_modifiers(unsigned long flags) {
    input::Modifiers mods;
    if (flags & NSEventModifierFlagShift)   mods.shift = true;
    if (flags & NSEventModifierFlagControl) mods.ctrl = true;
    if (flags & NSEventModifierFlagOption)  mods.alt = true;
    if (flags & NSEventModifierFlagCommand) mods.meta = true;
    return mods;
}

input::KeyEvent MacInput::translate_key_event(NSEvent* event) {
    input::KeyEvent ev;
    ev.mods = translate_modifiers([event modifierFlags]);
    ev.action = [event isARepeat] ? input::KeyAction::Repeat : input::KeyAction::Press;

    unsigned short key_code = [event keyCode];
    switch (key_code) {
        case 53:  ev.key = input::KeyCode::Escape; break;
        case 36:  ev.key = input::KeyCode::Return; break;
        case 48:  ev.key = input::KeyCode::Tab; break;
        case 51:  ev.key = input::KeyCode::Backspace; break;
        case 117: ev.key = input::KeyCode::Delete; break;
        case 126: ev.key = input::KeyCode::Up; break;
        case 125: ev.key = input::KeyCode::Down; break;
        case 123: ev.key = input::KeyCode::Left; break;
        case 124: ev.key = input::KeyCode::Right; break;
        case 115: ev.key = input::KeyCode::Home; break;
        case 119: ev.key = input::KeyCode::End; break;
        case 116: ev.key = input::KeyCode::PageUp; break;
        case 121: ev.key = input::KeyCode::PageDown; break;
        case 114: ev.key = input::KeyCode::Insert; break;
        case 122: ev.key = input::KeyCode::F1; break;
        case 120: ev.key = input::KeyCode::F2; break;
        case 99:  ev.key = input::KeyCode::F3; break;
        case 118: ev.key = input::KeyCode::F4; break;
        case 96:  ev.key = input::KeyCode::F5; break;
        case 97:  ev.key = input::KeyCode::F6; break;
        case 98:  ev.key = input::KeyCode::F7; break;
        case 100: ev.key = input::KeyCode::F8; break;
        case 101: ev.key = input::KeyCode::F9; break;
        case 109: ev.key = input::KeyCode::F10; break;
        case 103: ev.key = input::KeyCode::F11; break;
        case 111: ev.key = input::KeyCode::F12; break;
        default:  ev.key = input::KeyCode::Unknown; break;
    }

    NSString* chars = [event characters];
    if (chars && [chars length] > 0) {
        ev.text = [chars UTF8String];
        if ([chars length] == 1) {
            ev.codepoint = [chars characterAtIndex:0];
        }
    }

    return ev;
}

input::MouseEvent MacInput::translate_mouse_event(NSEvent* event, int cell_w, int cell_h, double view_height) {
    input::MouseEvent ev;
    ev.mods = translate_modifiers([event modifierFlags]);

    NSPoint loc = [event locationInWindow];
    // Cocoa coordinates have (0, 0) at bottom-left, invert Y for top-left
    int px = static_cast<int>(loc.x);
    int py = static_cast<int>(view_height - loc.y);

    ev.pixel_x = px;
    ev.pixel_y = py;
    ev.col = cell_w > 0 ? (px / cell_w) : 0;
    ev.row = cell_h > 0 ? (py / cell_h) : 0;

    NSEventType type = [event type];
    switch (type) {
        case NSEventTypeLeftMouseDown:
            ev.button = input::MouseButton::Left;
            ev.action = input::MouseAction::Press;
            break;
        case NSEventTypeLeftMouseUp:
            ev.button = input::MouseButton::Left;
            ev.action = input::MouseAction::Release;
            break;
        case NSEventTypeRightMouseDown:
            ev.button = input::MouseButton::Right;
            ev.action = input::MouseAction::Press;
            break;
        case NSEventTypeRightMouseUp:
            ev.button = input::MouseButton::Right;
            ev.action = input::MouseAction::Release;
            break;
        case NSEventTypeOtherMouseDown:
            ev.button = input::MouseButton::Middle;
            ev.action = input::MouseAction::Press;
            break;
        case NSEventTypeOtherMouseUp:
            ev.button = input::MouseButton::Middle;
            ev.action = input::MouseAction::Release;
            break;
        case NSEventTypeMouseMoved:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDragged:
            ev.button = input::MouseButton::None;
            ev.action = input::MouseAction::Move;
            break;
        case NSEventTypeScrollWheel: {
            CGFloat dy = [event scrollingDeltaY];
            ev.button = (dy > 0) ? input::MouseButton::WheelUp : input::MouseButton::WheelDown;
            ev.action = input::MouseAction::Press;
            break;
        }
        default:
            break;
    }

    return ev;
}

} // namespace gritty::platform::macos
