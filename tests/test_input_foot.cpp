#include <doctest/doctest.h>
#include "core/vt_parser.hpp"
#include "input/input_mapper.hpp"

using namespace gritty::core;
using namespace gritty::input;

TEST_CASE("foot XTerm modifyOtherKeys does not replace the legacy tmux prefix unless negotiated") {
    TerminalEngine engine(80, 24);
    VtParser parser(engine);
    KeyEvent prefix;
    prefix.codepoint = U'b';
    prefix.mods.ctrl = true;
    CHECK(InputMapper::map_key(prefix, engine) == "\x02");
    parser.feed("\x1b[>4;2m");
    CHECK(InputMapper::map_key(prefix, engine) == "\x1b[27;5;98~");
    parser.feed("\x1b[>4;0m");
    CHECK(InputMapper::map_key(prefix, engine) == "\x02");
    prefix.codepoint = U' ';
    CHECK(InputMapper::map_key(prefix, engine) == std::string(1, '\0'));
    prefix.codepoint = U'[';
    CHECK(InputMapper::map_key(prefix, engine) == "\x1b");
    engine.set_cursor_key_mode(true);
    KeyEvent home;
    home.key = KeyCode::Home;
    CHECK(InputMapper::map_key(home, engine) == "\x1bOH");
    home.key = KeyCode::F1;
    home.mods.ctrl = true;
    CHECK(InputMapper::map_key(home, engine) == "\x1b[1;5P");
}

TEST_CASE("foot Kitty disambiguation preserves plain typing and functional key encoding") {
    TerminalEngine engine(80, 24);
    engine.push_kitty_keyboard_flags(1);
    KeyEvent event;
    event.codepoint = U'a';
    event.text = "a";
    CHECK(InputMapper::map_key(event, engine) == "a");
    event.mods.alt = true;
    CHECK(InputMapper::map_key(event, engine) == "\x1b[97;3u");
    event.key = KeyCode::Up;
    event.text.clear();
    CHECK(InputMapper::map_key(event, engine) == "\x1b[1;3A");
    event.key = KeyCode::Delete;
    CHECK(InputMapper::map_key(event, engine) == "\x1b[3;3~");
    engine.push_kitty_keyboard_flags(3);
    event.key = KeyCode::Unknown;
    event.action = KeyAction::Release;
    CHECK(InputMapper::map_key(event, engine) == "\x1b[97;3:3u");
    engine.push_kitty_keyboard_flags(8 | 4 | 16);
    event.action = KeyAction::Press;
    event.mods = {.shift = true};
    event.codepoint = U'A';
    event.base_codepoint = U'a';
    event.text = "A";
    CHECK(InputMapper::map_key(event, engine) == "\x1b[97:65;2;65u");
}

TEST_CASE("foot Kitty keyboard stacks belong to their respective screen buffers") {
    TerminalEngine engine(80, 24);
    VtParser parser(engine);
    parser.feed("\x1b[>5u\x1b[?1049h");
    CHECK(engine.kitty_keyboard_flags() == 0);
    parser.feed("\x1b[>3u\x1b[?1049l");
    CHECK(engine.kitty_keyboard_flags() == 5);
    parser.feed("\x1b[<u");
    CHECK(engine.kitty_keyboard_flags() == 0);
}

TEST_CASE("foot mouse mode resets do not disable a different active mode") {
    TerminalEngine engine(80, 24);
    VtParser parser(engine);
    parser.feed("\x1b[?1002h\x1b[?1003l\x1b[?1006h\x1b[?1005l");
    CHECK(engine.mouse_tracking() == MouseTrackingMode::ButtonEvent);
    CHECK(engine.mouse_format() == MouseFormat::Sgr);
    MouseEvent event;
    event.action = MouseAction::Move;
    event.col = 10;
    event.row = 5;
    CHECK(InputMapper::map_mouse(event, engine).empty());
    event.button = MouseButton::Left;
    CHECK(InputMapper::map_mouse(event, engine) == "\x1b[<32;11;6M");
    engine.set_mouse_tracking(MouseTrackingMode::Normal);
    CHECK(InputMapper::map_mouse(event, engine).empty());
    event.action = MouseAction::Release;
    CHECK(InputMapper::map_mouse(event, engine) == "\x1b[<0;11;6m");
    event.button = MouseButton::WheelUp;
    CHECK(InputMapper::map_mouse(event, engine).empty());
}

TEST_CASE("foot legacy and URXVT mouse formats are not mislabeled SGR reports") {
    TerminalEngine engine(80, 24);
    engine.set_mouse_tracking(MouseTrackingMode::Normal);
    MouseEvent event;
    event.button = MouseButton::Left;
    event.col = 10;
    event.row = 5;
    const auto legacy = InputMapper::map_mouse(event, engine);
    REQUIRE(legacy.size() == 6);
    CHECK(legacy.substr(0, 3) == "\x1b[M");
    CHECK(legacy[3] == 32);
    CHECK(legacy[4] == 43);
    CHECK(legacy[5] == 38);
    engine.set_mouse_format(MouseFormat::Urxvt);
    CHECK(InputMapper::map_mouse(event, engine) == "\x1b[32;11;6M");
    engine.set_mouse_format(MouseFormat::Normal);
    event.col = 223;
    CHECK(InputMapper::map_mouse(event, engine).empty());
}

TEST_CASE("foot DECSET 1007 alternate scroll translates wheel events on alt screen only") {
    TerminalEngine engine(80, 24);
    VtParser parser(engine);
    MouseEvent wheel;
    wheel.action = MouseAction::Press;
    wheel.button = MouseButton::WheelUp;

    // Primary screen with no mouse tracking does not emit arrow keys.
    CHECK(InputMapper::map_mouse(wheel, engine).empty());

    // Alternate screen defaults to alternate scroll enabled.
    parser.feed("\x1b[?1049h\x1b[?1007$p");
    CHECK(InputMapper::map_mouse(wheel, engine) == "\x1b[A");
    wheel.button = MouseButton::WheelDown;
    engine.set_cursor_key_mode(true);
    CHECK(InputMapper::map_mouse(wheel, engine) == "\x1bOB");

    // Explicitly disabling DECSET 1007 suppresses alternate scroll.
    parser.feed("\x1b[?1007l");
    CHECK(InputMapper::map_mouse(wheel, engine).empty());
}
