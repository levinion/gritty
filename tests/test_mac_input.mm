#include <doctest/doctest.h>
#import <Cocoa/Cocoa.h>
#include "platform/macos/mac_input.hpp"
#include "input/input_mapper.hpp"

using namespace gritty;

TEST_CASE("Cocoa key normalization preserves tmux Ctrl-B, NUL and Unicode") {
    @autoreleasepool {
        auto make_key = [](NSString* text, NSString* unmodified, NSEventModifierFlags flags, unsigned short code) {
            return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:flags
                timestamp:0 windowNumber:0 context:nil characters:text charactersIgnoringModifiers:unmodified
                isARepeat:NO keyCode:code];
        };
        core::TerminalEngine engine(80, 24);
        auto event = platform::macos::MacInput::translate_key_event(
            make_key(@"\x02", @"b", NSEventModifierFlagControl, 11));
        CHECK(event.codepoint == U'b');
        CHECK(input::InputMapper::map_key(event, engine) == "\x02");
        engine.set_modify_other_keys(2);
        CHECK(input::InputMapper::map_key(event, engine) == "\x1b[27;5;98~");
        engine.set_modify_other_keys(0);
        const unichar nul = 0;
        event = platform::macos::MacInput::translate_key_event(
            make_key([NSString stringWithCharacters:&nul length:1], @" ", NSEventModifierFlagControl, 49));
        CHECK(event.text.size() == 1);
        CHECK(input::InputMapper::map_key(event, engine) == std::string(1, '\0'));
        event = platform::macos::MacInput::translate_key_event(make_key(@"😀", @"😀", 0, 0));
        CHECK(event.codepoint == U'😀');
        event = platform::macos::MacInput::translate_key_event(make_key(@"∫", @"b", NSEventModifierFlagOption, 11));
        CHECK(input::InputMapper::map_key(event, engine) == "\x1b" "b");
        event = platform::macos::MacInput::translate_key_event(make_key(@"\uf700", @"\uf700", 0, 126));
        engine.push_kitty_keyboard_flags(1);
        CHECK(input::InputMapper::map_key(event, engine) == "\x1b[A");
    }
}

TEST_CASE("Cocoa drag events preserve the held mouse button for tmux") {
    @autoreleasepool {
        NSEvent* event = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDragged location:NSMakePoint(20, 30)
            modifierFlags:0 timestamp:0 windowNumber:0 context:nil eventNumber:0 clickCount:1 pressure:1];
        const auto translated = platform::macos::MacInput::translate_mouse_event(event, 10, 20, 100);
        CHECK(translated.button == input::MouseButton::Left);
        CHECK(translated.action == input::MouseAction::Move);
        CHECK(translated.pixel_x == 20);
        CHECK(translated.pixel_y == 70);
    }
}
