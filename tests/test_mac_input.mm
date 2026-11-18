#include <doctest/doctest.h>
#import <Cocoa/Cocoa.h>
#include "platform/macos/mac_input.hpp"
#include "platform/macos/cocoa_view.hpp"
#include "platform/macos/cocoa_surface.hpp"
#include <memory>
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

TEST_CASE("NSTextInputClient commits IME text and tracks marked text") {
    @autoreleasepool {
        auto surface = std::make_unique<gritty::platform::macos::CocoaLayerSurface>(80, 25);
        std::string received;
        int key_events = 0;
        surface->set_key_callback([&](const gritty::input::KeyEvent&) { ++key_events; });
        surface->set_text_commit_callback([&](std::string_view text) { received.assign(text); });
        surface->set_bracketed_paste_provider([] { return true; });
        auto* view = [[GrittyView alloc] initWithSurface:surface.get()];
        REQUIRE(view != nil);

        [view insertText:@"ab" replacementRange:NSMakeRange(NSNotFound, 0)];
        CHECK(received == "\x1b[200~ab\x1b[201~");
        CHECK(key_events == 0); // IME text must not enter InputMapper/CSI-u.
        CHECK_FALSE([view hasMarkedText]);

        [view setMarkedText:@"にほん" selectedRange:NSMakeRange(2, 1) replacementRange:NSMakeRange(NSNotFound, 0)];
        CHECK([view hasMarkedText]);
        CHECK([view markedRange].length == 3);
        CHECK([view selectedRange].location == 2);
        [view unmarkText];
        CHECK_FALSE([view hasMarkedText]);

        [view doCommandBySelector:@selector(deleteBackward:)];
        CHECK(key_events == 1); // Correct selector spelling must emit Backspace.
        [view doCommandBySelector:@selector(deleteForward:)];
        CHECK(key_events == 2);

        [view detachSurface];
        [view release];
    }
}
