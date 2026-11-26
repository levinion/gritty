#include <doctest/doctest.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <utf8proc.h>
#include <unistd.h>
#include "core/vt_parser.hpp"
#include "input/input_mapper.hpp"
#include "render/cpu_renderer.hpp"
#include "platform/posix_pty.hpp"
#include "width_cases.hpp"
#include "font_fixture.hpp"
#include "config/toml_loader.hpp"
#include "util/version.hpp"

namespace {
using namespace gritty;
using Clock = std::chrono::steady_clock;

// Every test uses its own socket and server; never touch the user's tmux server.
class TmuxHarness {
public:
    core::TerminalEngine engine{60, 18};
    core::VtParser parser{engine};
    platform::PosixPty client;
    std::string socket;
    std::string trace;
    std::string executable;
    render::FontEngine fonts;
    render::RenderConfig render_config;
    render::CpuRenderer renderer;
    util::PixmanImage pixels;
    util::PixmanImage full_frame;
    bool font_checked{false};
    bool font_loaded{false};

    explicit TmuxHarness(render::RenderConfig rendering = {})
        : render_config(rendering), renderer(fonts, rendering) {
        parser.set_default_colors(rendering.default_fg, rendering.default_bg);
        const char* path = std::getenv("GRITTY_TMUX");
        executable = path ? path : "tmux";
        char directory[] = "/tmp/gritty-tmux-XXXXXX";
        const char* created = ::mkdtemp(directory);
        if (!created) throw std::runtime_error("mkdtemp failed");
        socket = std::string(created) + "/s";
        std::ofstream config(socket + ".conf");
        config << "set -s extended-keys on\n";
        config.close();
        parser.set_response_callback([this](std::string_view response) { client.write(response); });
    }
    ~TmuxHarness() {
        command({"kill-server"});
        client.terminate();
        std::filesystem::remove_all(std::filesystem::path(socket).parent_path());
    }

    std::string command(std::vector<std::string> args) {
        platform::PtySpawnOptions options;
        options.command = executable;
        options.args = {"-S", socket};
        options.args.insert(options.args.end(), args.begin(), args.end());
        options.env = {"TMUX"};
        platform::PosixPty process;
        if (!process.spawn(options)) return "SPAWN_FAILED";
        std::string output;
        uint8_t buffer[4096];
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (Clock::now() < deadline) {
            const auto n = process.read(buffer);
            if (n > 0) output.append(reinterpret_cast<char*>(buffer), n);
            else if (n == 0) break;
            else if (!process.is_alive()) {
                while (const auto last = process.read(buffer)) {
                    if (last < 0) break;
                    output.append(reinterpret_cast<char*>(buffer), last);
                }
                break;
            }
            pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::erase(output, '\r');
        return output;
    }

    bool start(bool utf8 = true) {
        platform::PtySpawnOptions options;
        options.command = executable;
        options.args = {"-S", socket, "-f", socket + ".conf", "-T", "extkeys,RGB"};
        if (utf8) options.args.push_back("-u");
        options.args.insert(options.args.end(), {"new-session", "-s", "check",
            "printf '\033[2J\033[H\033[38;2;7;123;231;48;2;21;43;65mMAIN\033[0m\r\n"
            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\r\nEND'; sleep 60"});
        options.env = {"TMUX", utf8 ? "LC_ALL=C.UTF-8" : "LC_ALL=C"};
        options.cols = engine.cols();
        options.rows = engine.rows();
        return client.spawn(options);
    }

    void pump() {
        uint8_t buffer[16384];
        for (int i = 0; i < 32; ++i) {
            const auto n = client.read(buffer);
            if (n <= 0) break;
            trace.append(reinterpret_cast<char*>(buffer), n);
            parser.feed(buffer, n);
        }
    }

    template<class Predicate>
    bool until(Predicate ready) {
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        do {
            pump();
            if (ready()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (Clock::now() < deadline);
        return false;
    }

    // Compare the external terminal's parsed cells to tmux's own pane grid,
    // not to an expected escape stream produced by our parser.
    std::string mismatch() {
        const auto listing = command({"list-panes", "-F",
            "#{pane_id} #{pane_left} #{pane_top} #{pane_width} #{pane_height}"});
        std::istringstream panes(listing);
        std::string id;
        int left, top, width, height;
        int checked = 0;
        while (panes >> id >> left >> top >> width >> height) {
            ++checked;
            if (left < 0 || top < 0 || left + width > engine.cols() || top + height > engine.rows())
                return "waiting for tmux resize";
            const auto capture = command({"capture-pane", "-p", "-t", id});
            std::istringstream lines(capture);
            for (int row = 0; row < height; ++row) {
                std::string line;
                std::getline(lines, line);
                int col = 0;
                size_t offset = 0;
                while (col < width) {
                    char32_t expected = U' ';
                    int cell_width = 1;
                    if (offset < line.size()) {
                        utf8proc_int32_t cp;
                        const auto count = utf8proc_iterate(
                            reinterpret_cast<const uint8_t*>(line.data() + offset), line.size() - offset, &cp);
                        if (count < 1) return "invalid capture UTF-8";
                        offset += count;
                        cell_width = utf8proc_charwidth(cp);
                        if (cell_width <= 0) continue;
                        expected = cp;
                    }
                    const auto actual = engine.grid().cell(left + col, top + row).codepoint;
                    if (actual != expected) {
                        std::ostringstream error;
                        error << id << " cell " << left + col << ',' << top + row
                              << ": expected U+" << std::hex << static_cast<uint32_t>(expected)
                              << " got U+" << static_cast<uint32_t>(actual);
                        return error.str();
                    }
                    col += cell_width;
                }
            }
        }
        return checked ? "" : "no panes: " + listing;
    }

    void verify_incremental_render() {
        if (!font_checked) {
            font_checked = true;
            const auto path = test_font_path();
            font_loaded = !path.empty() && fonts.load_font(path, 12.0);
            if (!font_loaded) WARN("No test font: skipping incremental/full-frame pixel comparison");
        }
        if (!font_loaded || engine.synchronized_updates()) return;
        const int width = engine.cols() * fonts.metrics().cell_width;
        const int height = engine.rows() * fonts.metrics().cell_height;
        const bool resized = !pixels.valid() || pixels.width() != width || pixels.height() != height;
        if (resized) {
            pixels.reset(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, nullptr, 0));
            full_frame.reset(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, nullptr, 0));
        }
        REQUIRE(pixels.valid());
        REQUIRE(full_frame.valid());
        renderer.render(engine, pixels.get(), nullptr, resized);
        render::CpuRenderer reference(fonts, render_config);
        reference.render(engine, full_frame.get(), nullptr, true);
        // Real tmux dirty-cell rendering must match a clean full redraw.
        CHECK(std::equal(pixels.data(), pixels.data() + height * pixels.stride() / 4, full_frame.data()));
    }

    void expect_screen() {
        std::string error;
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        do {
            pump();
            error = mismatch();
            if (error.empty()) {
                verify_incremental_render();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (Clock::now() < deadline);
        INFO(error);
        FAIL_CHECK("external screen disagrees with tmux capture-pane");
    }
};
}

TEST_CASE("Real tmux startup, pane splits, scrolling, redraw and client resize") {
    TmuxHarness tmux;
    REQUIRE(tmux.start());
    // Wait until the isolated server is ready before issuing control commands.
    for (int i = 0; i < 100 && tmux.trace.empty(); ++i) {
        tmux.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE_FALSE(tmux.trace.empty());
    tmux.command({"set-option", "-g", "status-left", "STATUS_OK "});
    REQUIRE(tmux.until([&] {
        for (int i = 0; i < 9; ++i)
            if (tmux.engine.grid().cell(i, tmux.engine.rows() - 1).codepoint != U"STATUS_OK"[i]) return false;
        return true;
    }));
    tmux.expect_screen();
    tmux.command({"set-option", "-g", "status", "off"});
    tmux.expect_screen();
    CHECK(tmux.engine.grid().cell(0, 0).fg == core::Color::make_rgb(7, 123, 231));
    CHECK(tmux.engine.grid().cell(0, 0).bg == core::Color::make_rgb(21, 43, 65));
    tmux.command({"split-window", "-h",
        "printf '\033[2J\033[HRIGHT\r\nBBBBBBBBBBBBBBBBBBBB'; sleep 60"});
    CHECK(tmux.command({"list-panes", "-F", "#{pane_id}"}) == "%0\n%1\n");
    tmux.expect_screen();
    tmux.command({"split-window", "-v",
        "i=1; while [ $i -le 30 ]; do printf 'LINE%02d\\n' $i; i=$((i+1)); done; sleep 60"});
    CHECK(tmux.command({"list-panes", "-F", "#{pane_id}"}) == "%0\n%1\n%2\n");
    tmux.expect_screen();
    CHECK(tmux.command({"refresh-client"}).empty());
    tmux.expect_screen();
    tmux.engine.resize(80, 24);
    tmux.client.resize(80, 24, 800, 480);
    REQUIRE(tmux.until([&] {
        return tmux.command({"display-message", "-p", "#{client_width}x#{client_height}"}) == "80x24\n";
    }));
    tmux.expect_screen();
    tmux.engine.resize(50, 14);
    tmux.client.resize(50, 14, 500, 280);
    REQUIRE(tmux.until([&] {
        return tmux.command({"display-message", "-p", "#{client_width}x#{client_height}"}) == "50x14\n";
    }));
    tmux.expect_screen();

    // Real SGR mouse input must select the inactive left pane.
    tmux.command({"set-option", "-g", "mouse", "on"});
    REQUIRE(tmux.until([&] { return tmux.engine.mouse_tracking() != core::MouseTrackingMode::None; }));
    CHECK(tmux.engine.mouse_format() == core::MouseFormat::Sgr);
    input::MouseEvent click;
    click.button = input::MouseButton::Left;
    click.col = 1;
    click.row = 1;
    tmux.client.write(input::InputMapper::map_mouse(click, tmux.engine));
    click.action = input::MouseAction::Release;
    tmux.client.write(input::InputMapper::map_mouse(click, tmux.engine));
    REQUIRE(tmux.until([&] {
        return tmux.command({"display-message", "-p", "#{pane_id}"}).find("%0") != std::string::npos;
    }));

    // Exercise Ctrl-B + ':' through our encoder, including XTerm extended keys.
    REQUIRE(tmux.until([&] { return tmux.engine.modify_other_keys() == 2; }));
    input::KeyEvent prefix;
    prefix.codepoint = U'b';
    prefix.mods.ctrl = true;
    tmux.client.write(input::InputMapper::map_key(prefix, tmux.engine));
    tmux.client.write(":");
    // Let tmux install its command-prompt key table before sending the command,
    // just as separate interactive key events do (one huge batch races it).
    REQUIRE(tmux.until([&] {
        return tmux.engine.grid().cell(0, tmux.engine.rows() - 1).codepoint == U':';
    }));
    tmux.client.write("new-window -n keyboard 'printf INPUT_OK; sleep 60'\r");
    REQUIRE(tmux.until([&] {
        return tmux.command({"list-windows", "-F", "#{window_name}"}).find("keyboard") != std::string::npos;
    }));
    tmux.expect_screen();

    // Discovery must complete, without impersonating foot's larger feature set.
    const std::string expected_xtversion = "gritty(" + std::string(util::kVersion) + ")";
    REQUIRE(tmux.until([&] {
        return tmux.command({"display-message", "-p", "#{client_termtype}"}).find(expected_xtversion) != std::string::npos;
    }));
    const auto features = tmux.command({"display-message", "-p", "#{client_termfeatures}"});
    CHECK(features.find("sync") != std::string::npos);
    CHECK(features.find("margins") == std::string::npos);
    CHECK_FALSE(tmux.engine.synchronized_updates());
    CHECK(tmux.command({"detach-client"}).empty());
    REQUIRE(tmux.until([&] { return !tmux.client.is_alive(); }));
    tmux.pump();
    CHECK_FALSE(tmux.engine.is_alt_screen());
}

TEST_CASE("Pinned Unicode width vectors agree with the installed tmux format engine") {
    TmuxHarness tmux;
    REQUIRE(tmux.start());
    REQUIRE(tmux.until([&] { return !tmux.trace.empty(); }));
    for (const auto& item : width_cases) {
        INFO(item.name);
        uint8_t bytes[4];
        const auto count = utf8proc_encode_char(item.codepoint, bytes);
        const std::string text(reinterpret_cast<char*>(bytes), count);
        CHECK(tmux.command({"display-message", "-p", "#{w:#{l:" + text + "}}"}) == std::to_string(item.width) + "\n");
    }
}

TEST_CASE("Real tmux indexed prompt colors survive an ANSI theme override") {
    const auto loaded = config::TomlLoader::load_from_string("[colors]\nregular1 = '#123456'\n");
    REQUIRE(loaded.has_value());
    TmuxHarness tmux;
    tmux.engine.configure_palette((*loaded)->colors.palette);
    REQUIRE(tmux.start());
    REQUIRE(tmux.until([&] { return !tmux.trace.empty(); }));
    tmux.command({"new-window", "-n", "palette",
        "printf '\033[38;5;33m\342\226\210\033[48;5;196m \033[41m \033[0m'; sleep 60"});
    REQUIRE(tmux.until([&] { return tmux.engine.grid().cell(0, 0).codepoint == U'█'; }));
    tmux.expect_screen();
    REQUIRE(tmux.font_loaded);
    const auto& metrics = tmux.fonts.metrics();
    const auto pixel = [&](int col) {
        return tmux.pixels.data()[(metrics.cell_height / 2) * tmux.pixels.stride() / 4
                                 + col * metrics.cell_width + metrics.cell_width / 2];
    };
    CHECK(pixel(0) == 0xff0087ff);
    CHECK(pixel(1) == 0xffff0000);
    CHECK(pixel(2) == 0xff123456);
    tmux.command({"refresh-client"});
    tmux.pump();
    tmux.verify_incremental_render();
    CHECK(pixel(0) == 0xff0087ff);
}

TEST_CASE("Real tmux preserves default-background alpha across redraws and pane changes") {
    render::RenderConfig config;
    config.default_bg = 0x80604020;
    TmuxHarness tmux(config);
    REQUIRE(tmux.start());
    REQUIRE(tmux.until([&] { return !tmux.trace.empty(); }));
    CHECK(tmux.command({"set-option", "-g", "status", "off"}).empty());
    tmux.expect_screen();
    if (!tmux.font_loaded) { WARN("No font for tmux alpha pixel checks"); return; }
    const auto check_alpha = [&](bool expect_explicit_background) {
        const int stride = tmux.pixels.stride() / 4;
        const auto metrics = tmux.fonts.metrics();
        CHECK(tmux.pixels.data()[4 * metrics.cell_height * stride] == 0x80302010);
        bool found_explicit = false;
        for (int y = 0; y < tmux.engine.rows(); ++y) for (int x = 0; x < tmux.engine.cols(); ++x) {
            if (tmux.engine.grid().cell(x, y).bg.is_default()) continue;
            found_explicit = true;
            CHECK((tmux.pixels.data()[y * metrics.cell_height * stride + x * metrics.cell_width] >> 24) == 255);
        }
        if (expect_explicit_background) CHECK(found_explicit);
    };
    check_alpha(true);
    CHECK(tmux.command({"refresh-client"}).empty());
    tmux.expect_screen();
    check_alpha(true);
    CHECK(tmux.command({"split-window", "-h", "printf RIGHT; sleep 60"}).empty());
    tmux.expect_screen();
    // tmux may reflow MAIN off the top on resize; assert opacity against the
    // actual cell backgrounds, not a stale assumption about text coordinates.
    check_alpha(false);
}

TEST_CASE("Real tmux non-UTF8 ACS borders use DEC special graphics without leaked B characters") {
    TmuxHarness tmux;
    REQUIRE(tmux.start(false));
    REQUIRE(tmux.until([&] { return !tmux.trace.empty(); }));
    tmux.command({"set-option", "-g", "status", "off"});
    tmux.command({"split-window", "-h", "printf RIGHT; sleep 60"});
    tmux.expect_screen();
    const auto listing = tmux.command({"list-panes", "-t", "%0", "-F", "#{pane_width}"});
    std::istringstream widths(listing);
    int width = 0;
    widths >> width;
    REQUIRE(width > 0);
    CHECK(tmux.engine.grid().cell(width, 5).codepoint == U'│');
    CHECK((tmux.trace.find("\x1b(0") != std::string::npos || tmux.trace.find("\x0e") != std::string::npos));
}

TEST_CASE("tmux passthrough Kitty image renders like a direct APC placement") {
    TmuxHarness harness;
    REQUIRE(harness.fonts.load_font(test_font_path(), 12.0));
    harness.parser.set_apc_handler([&](std::string_view payload) {
        harness.engine.kitty_graphics().process_apc_sequence(payload, 0, 0);
    });
    // tmux doubles ESC in its DCS passthrough wrapper.
    harness.parser.feed(
        "\x1bPtmux;\x1b\x1b_Ga=T,i=31,f=32,s=1,v=1;/wAA/w==\x1b\x1b\\\x1b\\");
    REQUIRE(harness.engine.kitty_graphics().get_image(31) != nullptr);
    CHECK(harness.engine.kitty_graphics().placements().size() == 1);

    const int w = harness.engine.cols() * harness.fonts.metrics().cell_width;
    const int h = harness.engine.rows() * harness.fonts.metrics().cell_height;
    gritty::util::PixmanImage target(pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, nullptr, 0));
    gritty::util::PixmanImage reference(pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, nullptr, 0));
    harness.renderer.render(harness.engine, target.get(), nullptr, true);
    render::CpuRenderer fresh(harness.fonts, harness.render_config);
    fresh.render(harness.engine, reference.get(), nullptr, true);
    CHECK(std::equal(target.data(), target.data() + h * target.stride() / 4, reference.data()));
}
