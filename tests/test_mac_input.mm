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

TEST_CASE("Cocoa Command chords are swallowed and Cmd+V pastes the pasteboard") {
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSPasteboard* board = [NSPasteboard generalPasteboard];
        NSString* previous = [board stringForType:NSPasteboardTypeString];
        [board clearContents];
        REQUIRE([board setString:@"clipboard-text" forType:NSPasteboardTypeString]);

        auto surface = std::make_unique<gritty::platform::macos::CocoaLayerSurface>(80, 25);
        std::string pasted, committed;
        int key_events = 0;
        surface->set_key_callback([&](const gritty::input::KeyEvent&) { ++key_events; });
        surface->set_text_commit_callback([&](std::string_view text) { committed.assign(text); });
        surface->set_paste_callback([&](std::string_view text) { pasted.assign(text); });
        auto* view = [[GrittyView alloc] initWithSurface:surface.get()];
        REQUIRE(view != nil);
        auto key = [](NSString* chars, NSString* unmodified, NSEventModifierFlags flags, unsigned short code) {
            return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:flags
                timestamp:0 windowNumber:0 context:nil characters:chars
                charactersIgnoringModifiers:unmodified isARepeat:NO keyCode:code];
        };

        // CapsLock/NumericPad flags must not stop Cmd+V, and neither key event
        // nor insertText: may leak a bare 'v' into the PTY.
        [view keyDown:key(@"v", @"v",
            NSEventModifierFlagCommand | NSEventModifierFlagCapsLock | NSEventModifierFlagNumericPad, 9)];
        CHECK(pasted == "clipboard-text");
        CHECK(key_events == 0);
        CHECK(committed.empty());
        [view keyUp:key(@"v", @"v", NSEventModifierFlagCommand, 9)];
        CHECK(key_events == 0);

        [view keyDown:key(@"x", @"x", NSEventModifierFlagCommand, 7)];
        CHECK(pasted == "clipboard-text");
        CHECK(key_events == 0);
        CHECK(committed.empty());

        // Ctrl/Option shortcuts still travel through the normal key path.
        [view keyDown:key(@"\x02", @"b", NSEventModifierFlagControl, 11)];
        CHECK(key_events == 1);
        [view keyDown:key(@"\x01", @"a", NSEventModifierFlagControl, 0)];
        CHECK(key_events == 2);
        [view keyDown:key(@"\u222b", @"b", NSEventModifierFlagOption, 11)];
        CHECK(key_events == 3);

        [view detachSurface];
        [view release];
        [board clearContents];
        if (previous) [board setString:previous forType:NSPasteboardTypeString];
    }
}

TEST_CASE("Cocoa surface records OSC 22 pointer shapes") {
    @autoreleasepool {
        gritty::platform::macos::CocoaLayerSurface surface(80, 25);
        using Shape = gritty::platform::MouseCursorShape;
        CHECK(surface.cursor_shape() == Shape::Default);
        surface.set_cursor_shape(Shape::Text);
        CHECK(surface.cursor_shape() == Shape::Text);
        surface.set_cursor_shape(Shape::NotAllowed);
        CHECK(surface.cursor_shape() == Shape::NotAllowed);
        surface.set_cursor_shape(Shape::Hidden);
        CHECK(surface.cursor_shape() == Shape::Hidden);
        surface.set_cursor_shape(Shape::Default);
        CHECK(surface.cursor_shape() == Shape::Default);
        // Bare CLI test binary has no bundle ID: notifications must no-op.
        surface.send_notification("title", "body");
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
