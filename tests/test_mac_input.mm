#include <doctest/doctest.h>
#import <Cocoa/Cocoa.h>
#include "platform/macos/mac_input.hpp"
#include "platform/macos/cocoa_view.hpp"
#include "platform/macos/cocoa_surface.hpp"
#include <cmath>
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

TEST_CASE("NSTextInputClient commits IME text, emits preedit, and handles repeat/cancel") {
    @autoreleasepool {
        auto surface = std::make_unique<gritty::platform::macos::CocoaLayerSurface>(80, 25);
        std::string received;
        std::string preedit;
        size_t preedit_sel_start = 0;
        size_t preedit_sel_end = 0;
        int preedit_calls = 0;
        int key_events = 0;
        surface->set_key_callback([&](const gritty::input::KeyEvent&) { ++key_events; });
        surface->set_text_commit_callback([&](std::string_view text) { received.assign(text); });
        surface->set_preedit_callback([&](std::string_view text, size_t sel_start, size_t sel_end) {
            preedit.assign(text);
            preedit_sel_start = sel_start;
            preedit_sel_end = sel_end;
            ++preedit_calls;
        });
        surface->set_bracketed_paste_provider([] { return true; });
        auto* view = [[GrittyView alloc] initWithSurface:surface.get()];
        REQUIRE(view != nil);

        // Multi-character IME commits must go straight to PTY without bracketed-paste wrappers.
        [view insertText:@"ab" replacementRange:NSMakeRange(NSNotFound, 0)];
        CHECK(received == "ab");
        CHECK(key_events == 0); // IME text must not enter InputMapper/CSI-u.
        CHECK_FALSE([view hasMarkedText]);

        [view setMarkedText:@"にほん" selectedRange:NSMakeRange(2, 1) replacementRange:NSMakeRange(NSNotFound, 0)];
        CHECK([view hasMarkedText]);
        CHECK([view markedRange].length == 3);
        CHECK([view selectedRange].location == 2);
        CHECK(preedit == "にほん");
        CHECK(preedit_sel_start == 6);
        CHECK(preedit_sel_end == 9);

        // Cancel while composing clears preedit without leaking Escape to PTY.
        [view doCommandBySelector:@selector(cancelOperation:)];
        CHECK_FALSE([view hasMarkedText]);
        CHECK(preedit.empty());
        CHECK(key_events == 0);

        // Setting empty marked text clears composition cleanly.
        [view setMarkedText:@"ni" selectedRange:NSMakeRange(2, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
        CHECK([view hasMarkedText]);
        [view setMarkedText:@"" selectedRange:NSMakeRange(0, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
        CHECK_FALSE([view hasMarkedText]);
        CHECK([view markedRange].location == NSNotFound);
        CHECK(preedit.empty());

        // unmarkText clears composition without committing unfinished text.
        [view setMarkedText:@"にほん" selectedRange:NSMakeRange(3, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
        received.clear();
        [view unmarkText];
        CHECK_FALSE([view hasMarkedText]);
        CHECK([view selectedRange].location == 0);
        CHECK([view selectedRange].length == 0);
        CHECK(received.empty());
        CHECK(preedit.empty());
        CHECK(preedit_calls > 0);

        // Repeated plain keys outside IME bypass interpretKeyEvents: directly to key_cb_.
        NSEvent* repeat_k = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
            modifierFlags:0 timestamp:0 windowNumber:0 context:nil characters:@"k"
            charactersIgnoringModifiers:@"k" isARepeat:YES keyCode:40];
        [view keyDown:repeat_k];
        CHECK(key_events == 1);

        [view doCommandBySelector:@selector(deleteBackward:)];
        CHECK(key_events == 2); // Correct selector spelling must emit Backspace.
        [view doCommandBySelector:@selector(deleteForward:)];
        CHECK(key_events == 3);

        [view detachSurface];
        [view release];
    }
}

TEST_CASE("Cocoa keyUp does not throw on isARepeat and IME commit preserves literal bytes") {
    @autoreleasepool {
        NSEvent* key_up = [NSEvent keyEventWithType:NSEventTypeKeyUp location:NSZeroPoint
            modifierFlags:0 timestamp:0 windowNumber:0 context:nil characters:@"a"
            charactersIgnoringModifiers:@"a" isARepeat:NO keyCode:0];
        const auto ev = platform::macos::MacInput::translate_key_event(key_up);
        CHECK(ev.action == input::KeyAction::Release);
        CHECK(ev.codepoint == U'a');

        auto surface = std::make_unique<gritty::platform::macos::CocoaLayerSurface>(80, 25);
        std::string committed;
        surface->set_text_commit_callback([&](std::string_view text) { committed.assign(text); });
        surface->set_bracketed_paste_provider([] { return true; });
        surface->commit_text("你好世界");
        CHECK(committed == "你好世界");
    }
}

TEST_CASE("Cocoa Command+Q routes to quit callback without firing activity disarm") {
    @autoreleasepool {
        [NSApplication sharedApplication];
        auto surface = std::make_unique<gritty::platform::macos::CocoaLayerSurface>(80, 25);
        int quits = 0;
        int new_windows = 0;
        int activities = 0;
        surface->set_quit_request_callback([&] { ++quits; });
        surface->set_new_window_callback([&] { ++new_windows; });
        surface->set_activity_callback([&] { ++activities; });

        auto* view = [[GrittyView alloc] initWithSurface:surface.get()];
        REQUIRE(view != nil);
        auto key = [](NSString* chars, NSEventModifierFlags flags, unsigned short code) {
            return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:flags
                timestamp:0 windowNumber:0 context:nil characters:chars
                charactersIgnoringModifiers:chars isARepeat:NO keyCode:code];
        };

        [view keyDown:key(@"q", NSEventModifierFlagCommand, 12)];
        CHECK(quits == 1);
        CHECK(activities == 0);

        [view keyDown:key(@"n", NSEventModifierFlagCommand, 45)];
        CHECK(new_windows == 1);
        CHECK(activities == 1);

        [view keyDown:key(@"k", 0, 40)];
        CHECK(activities == 2);

        [view detachSurface];
        [view release];
    }
}

TEST_CASE("macOS window placement centers first window, converts top-left position, and cascades") {
    using gritty::platform::WindowPosition;
    using gritty::platform::macos::ScreenRect;
    using gritty::platform::macos::compute_initial_window_origin;

    const ScreenRect visible{0.0, 40.0, 1440.0, 860.0};
    constexpr double primary_max_y = 900.0;

    // Default first window: centered horizontally, visually centered vertically.
    const auto centered = compute_initial_window_origin(
        800.0, 600.0, visible, primary_max_y, std::nullopt, std::nullopt);
    CHECK(centered.x == doctest::Approx(320.0));
    CHECK(centered.y == doctest::Approx(40.0 + std::floor(260.0 * 0.55)));

    // Explicit top-left position converts cleanly into AppKit bottom-left outer origin.
    const auto explicit_pos = compute_initial_window_origin(
        800.0, 600.0, visible, primary_max_y, WindowPosition{120, 80}, std::nullopt);
    CHECK(explicit_pos.x == doctest::Approx(120.0));
    CHECK(explicit_pos.y == doctest::Approx(900.0 - 80.0 - 600.0));

    // Subsequent windows cascade (+24, -24) and wrap inside visibleFrame when reaching edges.
    const ScreenRect ref{320.0, 183.0, 800.0, 600.0};
    const auto cascaded = compute_initial_window_origin(
        800.0, 600.0, visible, primary_max_y, std::nullopt, ref);
    CHECK(cascaded.x == doctest::Approx(344.0));
    CHECK(cascaded.y == doctest::Approx(159.0));

    const ScreenRect edge_ref{700.0, 45.0, 800.0, 600.0};
    const auto wrapped = compute_initial_window_origin(
        800.0, 600.0, visible, primary_max_y, std::nullopt, edge_ref);
    CHECK(wrapped.x == doctest::Approx(24.0));
    CHECK(wrapped.y == doctest::Approx(40.0 + 860.0 - 600.0 - 24.0));
}

TEST_CASE("Cocoa main menu populates App, File, Edit, View, Window and dispatches actions") {
    @autoreleasepool {
        NSPasteboard* board = [NSPasteboard generalPasteboard];
        NSString* previous = [board stringForType:NSPasteboardTypeString];
        [board clearContents];
        REQUIRE([board setString:@"menu-paste" forType:NSPasteboardTypeString]);

        int reopens = 0;
        int new_windows = 0;
        int quits = 0;
        gritty::platform::macos::initialize_cocoa_application({
            .on_reopen_no_windows = [&] { ++reopens; },
            .on_new_window = [&] { ++new_windows; },
            .on_quit_requested = [&] { ++quits; },
        });

        NSMenu* main_menu = [NSApp mainMenu];
        REQUIRE(main_menu != nil);
        REQUIRE([main_menu numberOfItems] == 5);
        CHECK([[[main_menu itemAtIndex:0] title] isEqualToString:@"Gritty"]);
        CHECK([[[main_menu itemAtIndex:1] title] isEqualToString:@"File"]);
        CHECK([[[main_menu itemAtIndex:2] title] isEqualToString:@"Edit"]);
        CHECK([[[main_menu itemAtIndex:3] title] isEqualToString:@"View"]);
        CHECK([[[main_menu itemAtIndex:4] title] isEqualToString:@"Window"]);
        CHECK([NSApp windowsMenu] == [[main_menu itemAtIndex:4] submenu]);

        NSMenu* app_menu = [[main_menu itemAtIndex:0] submenu];
        NSMenuItem* quit_item = [app_menu itemWithTitle:@"Quit Gritty"];
        REQUIRE(quit_item != nil);
        [NSApp sendAction:quit_item.action to:quit_item.target from:quit_item];
        CHECK(quits == 1);

        NSMenu* file_menu = [[main_menu itemAtIndex:1] submenu];
        NSMenuItem* new_item = [file_menu itemWithTitle:@"New Window"];
        REQUIRE(new_item != nil);
        [NSApp sendAction:new_item.action to:new_item.target from:new_item];
        CHECK(new_windows == 1);

        NSMenu* view_menu = [[main_menu itemAtIndex:3] submenu];
        CHECK([view_menu itemWithTitle:@"Zoom In"] != nil);
        CHECK([view_menu itemWithTitle:@"Zoom Out"] != nil);
        CHECK([view_menu itemWithTitle:@"Actual Size"] != nil);
        CHECK([view_menu itemWithTitle:@"Toggle Full Screen"] != nil);

        auto surface = std::make_unique<gritty::platform::macos::CocoaLayerSurface>(80, 25);
        std::string pasted;
        int activities = 0;
        std::vector<gritty::platform::macos::CocoaLayerSurface::ZoomAction> zooms;
        surface->set_paste_callback([&](std::string_view text) { pasted.assign(text); });
        surface->set_activity_callback([&] { ++activities; });
        surface->set_zoom_callback([&](auto action) { zooms.push_back(action); });
        auto* view = [[GrittyView alloc] initWithSurface:surface.get()];
        REQUIRE(view != nil);
        [view paste:nil];
        CHECK(pasted == "menu-paste");
        CHECK(activities == 1);

        [view zoomIn:nil];
        [view zoomOut:nil];
        [view resetZoom:nil];
        auto key = [](NSString* chars, NSEventModifierFlags flags, unsigned short code) {
            return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:flags
                timestamp:0 windowNumber:0 context:nil characters:chars
                charactersIgnoringModifiers:chars isARepeat:NO keyCode:code];
        };
        [view keyDown:key(@"=", NSEventModifierFlagCommand, 24)];
        [view keyDown:key(@"+", NSEventModifierFlagCommand | NSEventModifierFlagShift, 24)];
        [view keyDown:key(@"-", NSEventModifierFlagCommand, 27)];
        [view keyDown:key(@"0", NSEventModifierFlagCommand, 29)];
        using Zoom = gritty::platform::macos::CocoaLayerSurface::ZoomAction;
        REQUIRE(zooms.size() == 7);
        CHECK(zooms[0] == Zoom::In);
        CHECK(zooms[1] == Zoom::Out);
        CHECK(zooms[2] == Zoom::Reset);
        CHECK(zooms[3] == Zoom::In);
        CHECK(zooms[4] == Zoom::In);
        CHECK(zooms[5] == Zoom::Out);
        CHECK(zooms[6] == Zoom::Reset);
        CHECK(activities == 8);

        [view detachSurface];
        [view release];
        gritty::platform::macos::shutdown_cocoa_application();
        CHECK([[NSApp mainMenu] numberOfItems] == 0);
        CHECK([[NSApp windowsMenu] numberOfItems] == 0);
        CHECK(reopens == 0);

        [board clearContents];
        if (previous) [board setString:previous forType:NSPasteboardTypeString];
    }
}
