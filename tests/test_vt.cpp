#include <doctest/doctest.h>
#include "core/terminal_engine.hpp"
#include "core/vt_parser.hpp"

using namespace gritty::core;

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

    engine.set_cursor_pos(12, 5);
    engine.sgr_bold(true);
    engine.grid().reset_damage();
    // Switch to alt screen
    parser.feed("\x1b[?1049h");
    CHECK(engine.is_alt_screen());
    CHECK(engine.grid().cell(0, 0).codepoint == ' '); // cleared

    // As in foot, entering the alternate grid retains the cursor position;
    // the application explicitly homes it before drawing its frame.
    parser.feed("\x1b[HAlt Screen Data");
    CHECK(engine.grid().cell(0, 0).codepoint == U'A');

    // Switch back to primary
    parser.feed("\x1b[?1049l");
    CHECK(!engine.is_alt_screen());
    CHECK(engine.grid().cell(0, 0).codepoint == U'P');
    CHECK(engine.cursor_col() == 12);
    CHECK(engine.cursor_row() == 5);
    CHECK(engine.current_attrs().bold);
    for (int row = 0; row < engine.rows(); ++row) {
        CHECK(engine.grid().row(row).dirty);
    }
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

TEST_CASE("OSC 52 writes the clipboard and never answers queries") {
    TerminalEngine engine(10, 2);
    VtParser parser(engine);
    std::string got;
    int calls = 0;
    parser.set_clipboard_handler([&](std::string_view text) {
        got.assign(text);
        ++calls;
    });

    parser.feed("\x1b]52;c;SGVsbG8sIHRtdXgh\x1b\\");
    CHECK(calls == 1);
    CHECK(got == "Hello, tmux!");

    parser.feed("\x1b]52;c;?\x1b\\");      // Query: never read the clipboard back.
    CHECK(calls == 1);
    parser.feed("\x1b]52;c;!!!\x1b\\");    // Invalid base64 is ignored.
    CHECK(calls == 1);
    parser.feed("\x1b]52;c;\x1b\\");       // Empty payload is ignored.
    CHECK(calls == 1);

    // Unpadded base64 ("test") and the p/s selections also work.
    parser.feed("\x1b]52;p;dGVzdA\x1b\\");
    CHECK(calls == 2);
    CHECK(got == "test");
}

TEST_CASE("XTGETTCAP reports the capabilities tmux negotiates") {
    TerminalEngine engine(10, 2);
    VtParser parser(engine);
    std::string responses;
    parser.set_response_callback([&](std::string_view resp) { responses.append(resp); });

    // RGB, Ms, Sync, Smulx, Setulc and an unknown capability "ZZ".
    parser.feed("\x1bP+q524742;4d73;53796e63;536d756c78;536574756c63;5a5a\x1b\\");
    CHECK(responses.find("\x1bP1+r524742\x1b\\") != std::string::npos);              // Boolean.
    CHECK(responses.find("\x1bP1+r4d73=1b5d35323b") != std::string::npos);              // Ms -> OSC 52.
    CHECK(responses.find("\x1bP1+r53796e63\x1b\\") != std::string::npos);             // Sync.
    CHECK(responses.find("\x1bP1+r536d756c78=1b5b343a") != std::string::npos);          // Smulx.
    CHECK(responses.find("\x1bP1+r536574756c63=1b5b35383a") != std::string::npos);      // Setulc.
    CHECK(responses.find("\x1bP0+r5a5a\x1b\\") != std::string::npos);                 // Unknown.
}

TEST_CASE("DSR and DA1 responses are emitted") {
    TerminalEngine engine(10, 2);
    VtParser parser(engine);
    std::string responses;
    parser.set_response_callback([&](std::string_view resp) { responses.append(resp); });
    parser.feed("\x1b[6n");
    CHECK(responses == "\x1b[1;1R");
    responses.clear();
    parser.feed("\x1b[c");
    CHECK(responses == "\x1b[?62;22;4c"); // Sixel bit (DA1 parameter 4).
}

TEST_CASE("Placed images follow scroll, erase and the active screen") {
    auto make_image = [](uint32_t color) {
        gritty::util::PixmanImage image(pixman_image_create_bits(PIXMAN_a8r8g8b8, 1, 1, nullptr, 0));
        *image.data() = color;
        return image;
    };
    TerminalEngine engine(10, 5);
    engine.place_image(PlacedImage{.image = make_image(0xffff0000u), .col = 2, .row = 2});
    REQUIRE(engine.images().size() == 1);
    CHECK(engine.images()[0].row == 2);
    CHECK(engine.images()[0].alt_screen == false);

    engine.scroll_up(1); // Region 0..4: the image moves up with the text.
    REQUIRE(engine.images().size() == 1);
    CHECK(engine.images()[0].row == 1);

    engine.set_cursor_pos(0, 1);
    engine.erase_in_line(2); // Erasing the row removes images anchored on it.
    CHECK(engine.images().empty());

    // Alt-screen images are tagged and survive a switch back untouched.
    engine.set_alt_screen(true, 47);
    engine.place_image(PlacedImage{.image = make_image(0xff00ff00u), .col = 0, .row = 0});
    REQUIRE(engine.images().size() == 1);
    CHECK(engine.images()[0].alt_screen == true);
    engine.set_alt_screen(false, 47);
    engine.scroll_up(1); // Primary scroll must not move the alt-screen image.
    REQUIRE(engine.images().size() == 1);
    CHECK(engine.images()[0].row == 0);
    engine.erase_in_display(2); // Clears only the primary screen's images.
    CHECK(engine.images().size() == 1);
    engine.set_alt_screen(true, 47);
    engine.clear_images();
    CHECK(engine.images().empty());

    // Deleting by image id mirrors Kitty delete actions.
    engine.place_image(PlacedImage{.image = make_image(0xff0000ffu), .image_id = 7, .row = 0});
    engine.remove_images_by_id(7);
    CHECK(engine.images().empty());
}

TEST_CASE("Partially scrolled-out images remain visible until fully outside the region") {
    TerminalEngine engine(20, 8);
    auto image = [](uint32_t color) {
        auto* raw = pixman_image_create_bits(PIXMAN_a8r8g8b8, 1, 1, nullptr, 0);
        auto* data = reinterpret_cast<uint32_t*>(pixman_image_get_data(raw));
        data[0] = color;
        return gritty::util::PixmanImage(raw);
    };
    engine.place_image(PlacedImage{.image = image(0xff00ff00u), .row = 0, .rows = 4});
    REQUIRE(engine.images().size() == 1);
    engine.set_margins(0, 4);
    engine.scroll_up(2);
    REQUIRE(engine.images().size() == 1);
    CHECK(engine.images()[0].row == -2); // Top two rows are off-screen; bottom two remain.
    engine.scroll_up(2);
    CHECK(engine.images().empty());     // Fully outside: now it may be removed.
}

TEST_CASE("Hyperlink table is bounded and clears recycled cell references") {
    TerminalEngine engine(10, 2);
    std::string first_url;
    uint16_t first_id = 0;
    for (int i = 0; i < 4096; ++i) {
        const auto url = "https://example.com/" + std::to_string(i);
        const auto id = engine.get_or_create_hyperlink_id(url);
        REQUIRE(id != 0);
        CHECK(engine.get_hyperlink_url(id) == url);
        if (i == 0) {
            first_url = url;
            first_id = id;
            engine.grid().cell(0, 0).hyperlink_id = id;
        }
    }
    CHECK(first_id == 1);
    CHECK(engine.get_hyperlink_url(first_id) != first_url);
    CHECK(engine.grid().cell(0, 0).hyperlink_id == 0); // No stale URL after LRU reuse.
    const auto reinserted = engine.get_or_create_hyperlink_id(first_url);
    CHECK(reinserted != 0);
    CHECK(engine.get_hyperlink_url(reinserted) == first_url);
}
