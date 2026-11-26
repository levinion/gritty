#include <doctest/doctest.h>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <chrono>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "input/input_mapper.hpp"
#include "platform/posix_pty.hpp"
#include "platform/single_instance.hpp"

#if defined(PLATFORM_MACOS)
#include "platform/macos/ct_discovery.hpp"
#include "platform/macos/kqueue_loop.hpp"
#endif

using namespace gritty::input;
using namespace gritty::platform;

TEST_CASE("InputMapper legacy key translation") {
    gritty::core::TerminalEngine engine(80, 24);

    // Up arrow normal
    KeyEvent ev_up{.key = KeyCode::Up, .text = {}};
    CHECK(InputMapper::map_key(ev_up, engine) == "\x1b[A");

    // Up arrow in application cursor mode (DECCKM)
    engine.set_cursor_key_mode(true);
    CHECK(InputMapper::map_key(ev_up, engine) == "\x1bOA");
    engine.set_cursor_key_mode(false);

    // Ctrl+Up arrow
    KeyEvent ev_ctrl_up{.key = KeyCode::Up, .mods = {.ctrl = true}, .text = {}};
    CHECK(InputMapper::map_key(ev_ctrl_up, engine) == "\x1b[1;5A");

    // Home / End
    KeyEvent ev_home{.key = KeyCode::Home, .text = {}};
    CHECK(InputMapper::map_key(ev_home, engine) == "\x1b[H");

    // Ctrl+C
    KeyEvent ev_ctrl_c{.codepoint = 'c', .mods = {.ctrl = true}, .text = {}};
    CHECK(InputMapper::map_key(ev_ctrl_c, engine) == "\x03");
}

TEST_CASE("InputMapper Kitty Keyboard Protocol (CSI u)") {
    gritty::core::TerminalEngine engine(80, 24);
    // Push flag 1 (disambiguate)
    engine.push_kitty_keyboard_flags(1);

    KeyEvent ev_esc{.key = KeyCode::Escape, .text = {}};
    CHECK(InputMapper::map_key(ev_esc, engine) == "\x1b[27u");

    KeyEvent ev_enter{.key = KeyCode::Return, .text = {}};
    // foot/Kitty: disambiguation alone preserves unmodified Enter as CR.
    CHECK(InputMapper::map_key(ev_enter, engine) == "\r");

    KeyEvent ev_tab{.key = KeyCode::Tab, .mods = {.shift = true}, .text = {}};
    // Shift is modifier 1 (param = 2)
    CHECK(InputMapper::map_key(ev_tab, engine) == "\x1b[9;2u");
}

TEST_CASE("InputMapper SGR Mouse reporting") {
    gritty::core::TerminalEngine engine(80, 24);
    engine.set_mouse_tracking(gritty::core::MouseTrackingMode::Normal);
    engine.set_mouse_format(gritty::core::MouseFormat::Sgr);

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
        .cwd = {},
        .env = {},
        .cols = 80,
        .rows = 24,
        .pixel_width = 640,
        .pixel_height = 480,
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

TEST_CASE("PosixPty moves transfer child ownership and reaping clears the PID") {
    PosixPty target;
    PtySpawnOptions waiting;
    waiting.command = "/bin/sh";
    waiting.args = {"-c", "read line"};
    REQUIRE(target.spawn(waiting));
    {
        PosixPty source;
        PtySpawnOptions options;
        options.command = "/bin/sh";
        options.args = {"-c", "read marker; printf MOVED_OK"};
        REQUIRE(source.spawn(options));
        const auto pid = source.child_pid();
        CHECK((::fcntl(source.master_fd(), F_GETFD) & FD_CLOEXEC) != 0);
        PosixPty moved(std::move(source));
        CHECK(source.child_pid() == -1);
        CHECK(source.master_fd() == -1);
        target = std::move(moved);
        CHECK(moved.child_pid() == -1);
        CHECK(target.child_pid() == pid);
    }
    REQUIRE(target.write("go\n") == 3);
    std::string received;
    uint8_t buffer[256];
    for (int i = 0; i < 100; ++i) {
        const auto n = target.read(buffer);
        if (n > 0) received.append(reinterpret_cast<char*>(buffer), n);
        if (!target.is_alive()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // The child may exit between the preceding read and waitpid; drain final output.
    while (true) {
        const auto n = target.read(buffer);
        if (n <= 0) break;
        received.append(reinterpret_cast<char*>(buffer), n);
    }
    CHECK(received.find("MOVED_OK") != std::string::npos);
    CHECK_FALSE(target.is_alive());
    CHECK(target.child_pid() == -1);
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

TEST_CASE("KqueueRunLoop one-shot timer and cancellation") {
    macos::KqueueRunLoop loop;
    bool fired = false;
    loop.set_timer(5, [&] { fired = true; });
    // A CFRunLoop source may wake the first poll before the kernel timer is
    // due; keep polling until the deadline instead of assuming a single wait.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (!fired && std::chrono::steady_clock::now() < deadline) loop.poll_once(5);
    CHECK(fired);
    fired = false;
    loop.set_timer(5, [&] { fired = true; });
    loop.set_timer(0, {});
    loop.poll_once(20);
    CHECK_FALSE(fired);
}

TEST_CASE("KqueueRunLoop keeps independent one-shot timers") {
    macos::KqueueRunLoop loop;
    int default_count = 0, watchdog_count = 0, coalesce_count = 0;
    loop.set_timer(IEventLoop::TimerId::Default, 1, [&] { ++default_count; });
    loop.set_timer(IEventLoop::TimerId::SyncWatchdog, 2, [&] { ++watchdog_count; });
    loop.set_timer(IEventLoop::TimerId::FrameCoalesce, 3, [&] { ++coalesce_count; });
    auto pump_timers = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (std::chrono::steady_clock::now() < deadline) loop.poll_once(5);
    };
    pump_timers();
    CHECK(default_count == 1);
    CHECK(watchdog_count == 1);
    CHECK(coalesce_count == 1);

    // Replacing one id must not cancel the others.
    loop.set_timer(IEventLoop::TimerId::SyncWatchdog, 0, {});
    loop.set_timer(IEventLoop::TimerId::SyncWatchdog, 1, [&] { ++watchdog_count; });
    pump_timers();
    CHECK(watchdog_count == 2);
    CHECK(default_count == 1);
    CHECK(coalesce_count == 1);
}

TEST_CASE("KqueueRunLoop run dispatches timers through CFRunLoop") {
    macos::KqueueRunLoop loop;
    bool fired = false;
    loop.set_timer(IEventLoop::TimerId::FrameCoalesce, 5, [&] {
        fired = true;
        loop.stop();
    });
    loop.run();
    CHECK(fired);
}

TEST_CASE("KqueueRunLoop pipe wake") {
    macos::KqueueRunLoop loop;
    loop.wake();
    int res = loop.poll_once(50);
    CHECK(res >= 0);
}

TEST_CASE("KqueueRunLoop isolates scoped timers by owner and runs posted callbacks") {
    macos::KqueueRunLoop loop;
    int owner1_hits = 0;
    int owner2_hits = 0;
    loop.set_scoped_timer(0x1001, IEventLoop::TimerId::FrameCoalesce, 5, [&] { ++owner1_hits; });
    loop.set_scoped_timer(0x2002, IEventLoop::TimerId::FrameCoalesce, 5, [&] { ++owner2_hits; });
    loop.cancel_scoped_timer(0x1001, IEventLoop::TimerId::FrameCoalesce);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (owner2_hits == 0 && std::chrono::steady_clock::now() < deadline) loop.poll_once(5);
    CHECK(owner1_hits == 0);
    CHECK(owner2_hits == 1);

    int posted_order = 0;
    loop.post([&] { posted_order += 1; });
    loop.post([&] { posted_order += 2; });
    loop.poll_once(50);
    CHECK(posted_order == 3);
}

TEST_CASE("SingleInstanceServer forwards launch requests, recovers stale sockets, and cleans up") {
    const std::string sock_path =
        "/tmp/gritty-ipc-test-" + std::to_string(static_cast<long>(::getpid())) + ".sock";
    const std::string lock_path = sock_path + ".lock";
    std::error_code ec;
    std::filesystem::remove(sock_path, ec);
    std::filesystem::remove(lock_path, ec);

    // Create an unattached stale socket file first to verify stale recovery.
    {
        int stale_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        REQUIRE(stale_fd >= 0);
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path.c_str());
        REQUIRE(::bind(stale_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
        ::close(stale_fd);
        CHECK(std::filesystem::exists(sock_path));
    }

    LaunchRequest initial;
    initial.inherited_cwd = "/tmp";
    SingleInstanceServer server;
    std::string err;
    CHECK(server.acquire_or_forward(sock_path, initial, &err) ==
          SingleInstanceServer::AcquireStatus::BecamePrimary);
    CHECK(server.is_primary());

    macos::KqueueRunLoop loop;
    std::vector<LaunchRequest> received;
    bool reject_next = false;
    bool saw_rejected = false;
    server.attach(loop, [&](const LaunchRequest& req) -> std::expected<void, std::string> {
        if (reject_next) {
            saw_rejected = true;
            return std::unexpected("bad override");
        }
        received.push_back(req);
        return {};
    });

    LaunchRequest second;
    second.config_path = "/tmp/custom.toml";
    second.explicit_cwd = "/tmp/work";
    second.inherited_cwd = "/Users/dev";
    second.title = "Second";
    second.app_id = "gritty.test";
    second.font_family = "Menlo";
    second.font_size = 14.5;
    second.cols = 100;
    second.rows = 30;
    second.hold = true;
    second.config_overrides = {"window.opacity=0.8"};
    second.command = "/usr/bin/env";
    second.command_args = {"FOO=1"};

    SingleInstanceServer::AcquireStatus forward_status = SingleInstanceServer::AcquireStatus::Failed;
    std::thread sender([&] {
        SingleInstanceServer probe;
        forward_status = probe.acquire_or_forward(sock_path, second, &err);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (received.empty() && std::chrono::steady_clock::now() < deadline) loop.poll_once(10);
    sender.join();

    CHECK(forward_status == SingleInstanceServer::AcquireStatus::ForwardedOk);
    REQUIRE(received.size() == 1);
    CHECK(received[0].config_path == second.config_path);
    CHECK(received[0].explicit_cwd == second.explicit_cwd);
    CHECK(received[0].inherited_cwd == second.inherited_cwd);
    CHECK(received[0].title == second.title);
    CHECK(received[0].app_id == second.app_id);
    CHECK(received[0].font_family == second.font_family);
    CHECK(received[0].font_size == doctest::Approx(14.5));
    CHECK(received[0].cols == 100);
    CHECK(received[0].rows == 30);
    CHECK(received[0].hold == true);
    CHECK(received[0].config_overrides == second.config_overrides);
    CHECK(received[0].command == second.command);
    CHECK(received[0].command_args == second.command_args);

    reject_next = true;
    std::string server_err;
    SingleInstanceServer::AcquireStatus reject_status = SingleInstanceServer::AcquireStatus::Failed;
    std::thread rejected_sender([&] {
        SingleInstanceServer probe;
        reject_status = probe.acquire_or_forward(sock_path, second, &server_err);
    });
    const auto deadline2 = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!saw_rejected && std::chrono::steady_clock::now() < deadline2) loop.poll_once(10);
    rejected_sender.join();
    CHECK(reject_status == SingleInstanceServer::AcquireStatus::ForwardedError);
    CHECK(server_err == "bad override");

    server.stop();
    CHECK_FALSE(server.is_primary());
    CHECK_FALSE(std::filesystem::exists(sock_path));
    std::filesystem::remove(lock_path, ec);
}
#endif
