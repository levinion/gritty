#include <doctest/doctest.h>
#include "core/terminal_engine.hpp"
#include "core/vt_parser.hpp"

using namespace squirt::core;

TEST_CASE("VtParser basic text and cursor movement") {
    TerminalEngine engine(80, 24);
    VtParser parser(engine);

    parser.feed("Hello World!\r\nSecond Line");
    CHECK(engine.grid().cell(0, 0).codepoint == U'H');
    CHECK(engine.grid().cell(4, 0).codepoint == U'o');
    CHECK(engine.grid().cell(11, 0).codepoint == U'!');
    CHECK(engine.grid().cell(0, 1).codepoint == U'S');
    CHECK(engine.grid().cell(10, 1).codepoint == U'e');
    CHECK(engine.cursor_col() == 11);
    CHECK(engine.cursor_row() == 1);

    // Test CUP (cursor position \x1b[5;10H) -> row 5 (idx 4), col 10 (idx 9)
    parser.feed("\x1b[5;10H");
    CHECK(engine.cursor_row() == 4);
    CHECK(engine.cursor_col() == 9);

    // Relative moves: Up 2, Back 3
    parser.feed("\x1b[2A\x1b[3D");
    CHECK(engine.cursor_row() == 2);
    CHECK(engine.cursor_col() == 6);
}

TEST_CASE("VtParser UTF-8 multibyte and wide characters") {
    TerminalEngine engine(80, 24);
    VtParser parser(engine);

    // Chinese characters "世界" (each width 2)
    parser.feed("世界");
    // Column 0 has '世' with width 2
    CHECK(engine.grid().cell(0, 0).codepoint == U'世');
    CHECK(engine.grid().cell(0, 0).attrs.width == 2);
    // Column 1 is continuation cell (width 0)
    CHECK(engine.grid().cell(1, 0).attrs.width == 0);
    // Column 2 has '界' with width 2
    CHECK(engine.grid().cell(2, 0).codepoint == U'界');
    CHECK(engine.grid().cell(2, 0).attrs.width == 2);
    // Column 3 is continuation cell
    CHECK(engine.grid().cell(3, 0).attrs.width == 0);
    // Cursor advanced by 4 columns
    CHECK(engine.cursor_col() == 4);
}

TEST_CASE("VtParser SGR styling and colors") {
    TerminalEngine engine(80, 24);
    VtParser parser(engine);

    // Bold, Italic, 256-color FG (idx 208), Truecolor BG (20, 30, 40)
    parser.feed("\x1b[1;3;38;5;208;48;2;20;30;40mStyled");
    for (int i = 0; i < 6; ++i) {
        const auto& c = engine.grid().cell(i, 0);
        CHECK(c.attrs.bold == 1);
        CHECK(c.attrs.italic == 1);
        CHECK(c.fg.is_indexed());
        CHECK(c.fg.index() == 208);
        CHECK(c.bg.is_rgb());
        CHECK(c.bg.r() == 20);
        CHECK(c.bg.g() == 30);
        CHECK(c.bg.b() == 40);
    }

    // Reset SGR
    parser.feed("\x1b[0mNormal");
    const auto& c_normal = engine.grid().cell(6, 0);
    CHECK(c_normal.attrs.bold == 0);
    CHECK(c_normal.attrs.italic == 0);
    CHECK(c_normal.fg.is_default());
    CHECK(c_normal.bg.is_default());
}

TEST_CASE("VtParser truecolor colon subparameter format") {
    TerminalEngine engine(80, 24);
    VtParser parser(engine);

    // ISO colon format: 38:2::100:150:200m
    parser.feed("\x1b[38:2::100:150:200mColonColor");
    const auto& c = engine.grid().cell(0, 0);
    CHECK(c.fg.is_rgb());
    CHECK(c.fg.r() == 100);
    CHECK(c.fg.g() == 150);
    CHECK(c.fg.b() == 200);
}

TEST_CASE("VtParser Underline styles and Underline color") {
    TerminalEngine engine(80, 24);
    VtParser parser(engine);

    // Curly underline (4:3m) with 24-bit underline color (58;2;255;128;0m)
    parser.feed("\x1b[4:3;58;2;255;128;0mCurly");
    const auto& c = engine.grid().cell(0, 0);
    CHECK(c.attrs.underline_style == static_cast<uint16_t>(UnderlineStyle::Curly));
    CHECK(c.underline_color.is_rgb());
    CHECK(c.underline_color.r() == 255);
    CHECK(c.underline_color.g() == 128);
    CHECK(c.underline_color.b() == 0);
}

TEST_CASE("VtParser Alternate Screen Buffer (DECSET 1049)") {
    TerminalEngine engine(80, 24);
    VtParser parser(engine);

    parser.feed("Primary Screen Data");
    CHECK(engine.grid().cell(0, 0).codepoint == U'P');
    CHECK(!engine.is_alt_screen());

    // Switch to alt screen
    parser.feed("\x1b[?1049h");
    CHECK(engine.is_alt_screen());
    CHECK(engine.grid().cell(0, 0).codepoint == ' '); // cleared

    parser.feed("Alt Screen Data");
    CHECK(engine.grid().cell(0, 0).codepoint == U'A');

    // Switch back to primary
    parser.feed("\x1b[?1049l");
    CHECK(!engine.is_alt_screen());
    CHECK(engine.grid().cell(0, 0).codepoint == U'P');
}

TEST_CASE("VtParser Synchronized Updates (DECSET 2026)") {
    TerminalEngine engine(80, 24);
    VtParser parser(engine);

    CHECK(!engine.synchronized_updates());
    // Begin synchronized update (BSU)
    parser.feed("\x1b[?2026h");
    CHECK(engine.synchronized_updates());

    // End synchronized update (ESU)
    parser.feed("\x1b[?2026l");
    CHECK(!engine.synchronized_updates());
}

TEST_CASE("VtParser Kitty Keyboard Protocol (CSI u)") {
    TerminalEngine engine(80, 24);
    VtParser parser(engine);

    std::string last_response;
    parser.set_response_callback([&](std::string_view resp) {
        last_response = resp;
    });

    // Query flags: \x1b[?u -> responds \x1b[?0u
    parser.feed("\x1b[?u");
    CHECK(last_response == "\x1b[?0u");

    // Push flags: \x1b[>1u (flag 1 = disambiguate escape codes)
    parser.feed("\x1b[>1u");
    CHECK(engine.kitty_keyboard_flags() == 1);

    // Push flags: \x1b[>3u
    parser.feed("\x1b[>3u");
    CHECK(engine.kitty_keyboard_flags() == 3);

    // Pop 1: \x1b[<1u
    parser.feed("\x1b[<1u");
    CHECK(engine.kitty_keyboard_flags() == 1);

    // Pop 1: \x1b[<1u
    parser.feed("\x1b[<1u");
    CHECK(engine.kitty_keyboard_flags() == 0);
}

TEST_CASE("VtParser OSC Window Title and CWD") {
    TerminalEngine engine(80, 24);
    VtParser parser(engine);

    // OSC 2 ; Title ST (ESC \)
    parser.feed("\x1b]2;My Terminal Window\x1b\\");
    CHECK(engine.window_title() == "My Terminal Window");

    // OSC 7 ; file:///path BEL (\x07)
    parser.feed("\x1b]7;file:///home/user/code\x07");
    CHECK(engine.cwd() == "file:///home/user/code");
}

TEST_CASE("VtParser OSC 8 Hyperlinks") {
    TerminalEngine engine(80, 24);
    VtParser parser(engine);

    // OSC 8 ; ; https://example.com ST
    parser.feed("\x1b]8;;https://example.com\x1b\\Link\x1b]8;;\x1b\\");
    const auto& c = engine.grid().cell(0, 0);
    CHECK(c.hyperlink_id > 0);
    CHECK(engine.get_hyperlink_url(c.hyperlink_id) == "https://example.com");

    // After clearing, cell should have hyperlink_id 0
    parser.feed("NoLink");
    const auto& c_no_link = engine.grid().cell(4, 0);
    CHECK(c_no_link.hyperlink_id == 0);
}
