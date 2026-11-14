#include <doctest/doctest.h>
#include <filesystem>
#include <thread>
#include <chrono>
#include "input/input_mapper.hpp"
#include "platform/posix_pty.hpp"

#if defined(PLATFORM_MACOS)
#include "platform/macos/ct_discovery.hpp"
#include "platform/macos/kqueue_loop.hpp"
#endif

using namespace squirt::input;
using namespace squirt::platform;

TEST_CASE("InputMapper legacy key translation") {
    squirt::core::TerminalEngine engine(80, 24);

    // Up arrow normal
    KeyEvent ev_up{.key = KeyCode::Up};
    CHECK(InputMapper::map_key(ev_up, engine) == "\x1b[A");

    // Up arrow in application cursor mode (DECCKM)
    engine.set_cursor_key_mode(true);
    CHECK(InputMapper::map_key(ev_up, engine) == "\x1bOA");
    engine.set_cursor_key_mode(false);

    // Ctrl+Up arrow
    KeyEvent ev_ctrl_up{.key = KeyCode::Up, .mods = {.ctrl = true}};
    CHECK(InputMapper::map_key(ev_ctrl_up, engine) == "\x1b[1;5A");

    // Home / End
    KeyEvent ev_home{.key = KeyCode::Home};
    CHECK(InputMapper::map_key(ev_home, engine) == "\x1b[H");

    // Ctrl+C
    KeyEvent ev_ctrl_c{.codepoint = 'c', .mods = {.ctrl = true}};
    CHECK(InputMapper::map_key(ev_ctrl_c, engine) == "\x03");
}

TEST_CASE("InputMapper Kitty Keyboard Protocol (CSI u)") {
    squirt::core::TerminalEngine engine(80, 24);
    // Push flag 1 (disambiguate)
    engine.push_kitty_keyboard_flags(1);

    KeyEvent ev_esc{.key = KeyCode::Escape};
    CHECK(InputMapper::map_key(ev_esc, engine) == "\x1b[27u");

    KeyEvent ev_enter{.key = KeyCode::Return};
    CHECK(InputMapper::map_key(ev_enter, engine) == "\x1b[13u");

    KeyEvent ev_tab{.key = KeyCode::Tab, .mods = {.shift = true}};
    // Shift is modifier 1 (param = 2)
    CHECK(InputMapper::map_key(ev_tab, engine) == "\x1b[9;2u");
}

TEST_CASE("InputMapper SGR Mouse reporting") {
    squirt::core::TerminalEngine engine(80, 24);
    engine.set_mouse_tracking(squirt::core::MouseTrackingMode::Normal);
    engine.set_mouse_format(squirt::core::MouseFormat::Sgr);

    // Left click at col 10, row 5
    MouseEvent ev_click{
        .button = MouseButton::Left,
        .action = MouseAction::Press,
        .col = 10,
        .row = 5
    };
    CHECK(InputMapper::map_mouse(ev_click, engine) == "\x1b[<0;11;6M");

    // Left release at col 10, row 5
    ev_click.action = MouseAction::Release;
    CHECK(InputMapper::map_mouse(ev_click, engine) == "\x1b[<0;11;6m");
}

TEST_CASE("PosixPty child process spawn and I/O") {
    PosixPty pty;
    PtySpawnOptions opts{
        .command = "/bin/sh",
        .args = {"-c", "echo 'PTY_TEST_SUCCESS'"},
        .cols = 80,
        .rows = 24
    };

    bool ok = pty.spawn(opts);
    REQUIRE(ok);
    CHECK(pty.master_fd() >= 0);
    CHECK(pty.child_pid() > 0);

    // Read until output is received or timeout
    std::string received;
    uint8_t buf[256];
    for (int attempts = 0; attempts < 20; ++attempts) {
        ssize_t n = pty.read(std::span<uint8_t>(buf, sizeof(buf)));
        if (n > 0) {
            received.append(reinterpret_cast<char*>(buf), n);
            if (received.find("PTY_TEST_SUCCESS") != std::string::npos) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    CHECK(received.find("PTY_TEST_SUCCESS") != std::string::npos);
}

#if defined(PLATFORM_MACOS)
TEST_CASE("CoreTextFinder font discovery") {
    macos::CoreTextFinder finder;
    auto match = finder.find_font(FontPattern{.family = "Monaco"});
    REQUIRE(match.has_value());
    CHECK(std::filesystem::exists(match->path));

    auto chain = finder.fallback_chain(FontPattern{.family = "Monaco"});
    CHECK(!chain.empty());
}

TEST_CASE("KqueueRunLoop pipe wake") {
    macos::KqueueRunLoop loop;
    loop.wake();
    int res = loop.poll_once(50);
    CHECK(res >= 0);
}
#endif
