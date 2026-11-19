#include <doctest/doctest.h>
#include <iterator>
#include <utility>
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

    // Bulk path must match print_char() byte-for-byte, including the odd
    // trailing column wrap on a 7-column screen.
    TerminalEngine fast(7, 3), slow(7, 3);
    const char32_t run[] = {U'一', U'二', U'三', U'四', U'五'};
    fast.print_wide_run(run);
    for (const char32_t cp : run) slow.print_char(cp);
    CHECK(fast.cursor_col() == slow.cursor_col());
    CHECK(fast.cursor_row() == slow.cursor_row());
    CHECK(fast.grid().row(0).wrapped == slow.grid().row(0).wrapped);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 7; ++x)
            CHECK(fast.grid().cell(x, y) == slow.grid().cell(x, y));

    TerminalEngine parsed(16, 2);
    VtParser bulk_parser(parsed);
    bulk_parser.feed("一二三四");
    CHECK(parsed.cursor_col() == 8);
    CHECK(parsed.grid().cell(0, 0).codepoint == U'一');
    CHECK(parsed.grid().cell(6, 0).codepoint == U'四');
    CHECK(parsed.grid().cell(7, 0).attrs.width == 0);

    TerminalEngine narrow_fast(1, 2), narrow_slow(1, 2);
    const char32_t narrow_run[] = {U'一', U'二', U'三', U'四'};
    narrow_fast.print_wide_run(narrow_run);
    for (const char32_t cp : narrow_run) narrow_slow.print_char(cp);
    CHECK(narrow_fast.cursor_col() == narrow_slow.cursor_col());
    CHECK(narrow_fast.cursor_row() == narrow_slow.cursor_row());
    for (int y = 0; y < 2; ++y)
        CHECK(narrow_fast.grid().cell(0, y) == narrow_slow.grid().cell(0, y));
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
    const auto extra = engine.grid().get_cell_extra(c.extra_idx);
    CHECK(c.attrs.underline_style == static_cast<uint16_t>(UnderlineStyle::Curly));
    CHECK(extra.underline_color.is_rgb());
    CHECK(extra.underline_color.r() == 255);
    CHECK(extra.underline_color.g() == 128);
    CHECK(extra.underline_color.b() == 0);
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
    CHECK(engine.cwd() == "/home/user/code"); // Percent-decoded absolute path.
}

TEST_CASE("VtParser OSC 8 Hyperlinks") {
    TerminalEngine engine(80, 24);
    VtParser parser(engine);

    // OSC 8 ; ; https://example.com ST
    parser.feed("\x1b]8;;https://example.com\x1b\\Link\x1b]8;;\x1b\\");
    const auto& c = engine.grid().cell(0, 0);
    const uint16_t link_id = engine.grid().get_cell_extra(c.extra_idx).hyperlink_id;
    CHECK(link_id > 0);
    CHECK(engine.get_hyperlink_url(link_id) == "https://example.com");

    // After clearing, cell should have hyperlink_id 0
    parser.feed("NoLink");
    const auto& c_no_link = engine.grid().cell(4, 0);
    CHECK(engine.grid().get_cell_extra(c_no_link.extra_idx).hyperlink_id == 0);
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

    // RGB, Ms, Sync, Smulx, Setulc, an unsupported Sxl and an unknown "ZZ".
    parser.feed("\x1bP+q524742;4d73;53796e63;536d756c78;536574756c63;53786c;5a5a\x1b\\");
    CHECK(responses.find("\x1bP1+r524742\x1b\\") != std::string::npos);              // Boolean.
    CHECK(responses.find("\x1bP1+r4d73=1b5d35323b") != std::string::npos);              // Ms -> OSC 52.
    CHECK(responses.find("\x1bP1+r53796e63\x1b\\") != std::string::npos);             // Sync.
    CHECK(responses.find("\x1bP1+r536d756c78=1b5b343a") != std::string::npos);          // Smulx.
    CHECK(responses.find("\x1bP1+r536574756c63=1b5b35383a") != std::string::npos);      // Setulc.
    CHECK(responses.find("\x1bP0+r53786c\x1b\\") != std::string::npos);               // Sxl: unsupported.
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
    CHECK(responses == "\x1b[?62;22c"); // No DA1 parameter 4: Kitty only.

    // Unknown DCS strings are consumed to ST without corrupting Ground state.
    responses.clear();
    parser.feed("\x1bP1;2$qUNKNOWN;image\x1b\\READY");
    CHECK(responses.empty());
    CHECK(engine.grid().cell(0, 0).codepoint == U'R');
    CHECK(engine.grid().cell(4, 0).codepoint == U'Y');
    CHECK(engine.cursor_col() == 5);
}

TEST_CASE("Placed images follow scroll, erase and the active screen") {
    auto make_image = [](uint32_t color) {
        gritty::util::PixmanImage image(pixman_image_create_bits(PIXMAN_a8r8g8b8, 1, 1, nullptr, 0));
        *image.data() = color;
        return image;
    };
    TerminalEngine engine(10, 5);
    PlacedImage image;
    image.image = make_image(0xffff0000u); image.col = 2; image.row = 2;
    engine.place_image(std::move(image));
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
    PlacedImage green;
    green.image = make_image(0xff00ff00u); green.col = 0; green.row = 0;
    engine.place_image(std::move(green));
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
    PlacedImage blue;
    blue.image = make_image(0xff0000ffu); blue.image_id = 7; blue.row = 0;
    engine.place_image(std::move(blue));
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
    PlacedImage placed;
    placed.image = image(0xff00ff00u); placed.row = 0; placed.rows = 4;
    engine.place_image(std::move(placed));
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
            auto linked = engine.grid().cell(0, 0);
            engine.grid().set_cell_with_extra(0, 0, linked, CellExtra{.hyperlink_id = id});
        }
    }
    CHECK(first_id == 1);
    CHECK(engine.get_hyperlink_url(first_id) != first_url);
    CHECK(engine.grid().get_cell_extra(engine.grid().cell(0, 0).extra_idx).hyperlink_id == 0); // No stale URL after LRU reuse.
    const auto reinserted = engine.get_or_create_hyperlink_id(first_url);
    CHECK(reinserted != 0);
    CHECK(engine.get_hyperlink_url(reinserted) == first_url);
}

TEST_CASE("ASCII batch writes match per-character VT semantics") {
    const std::string text = "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    TerminalEngine batched(10, 4), reference(10, 4);
    VtParser a(batched), b(reference);
    a.feed("\x1b[1;38;2;10;20;30m" + text);
    for (const char c : std::string("\x1b[1;38;2;10;20;30m")) b.feed(std::string_view(&c, 1));
    for (const char c : text) {
        const std::string one(1, c);
        b.feed(one);
    }
    CHECK(batched.cursor_col() == reference.cursor_col());
    CHECK(batched.cursor_row() == reference.cursor_row());
    for (int y = 0; y < 4; ++y) {
        CHECK(batched.grid().row(y).wrapped == reference.grid().row(y).wrapped);
        for (int x = 0; x < 10; ++x)
            CHECK(batched.grid().cell(x, y) == reference.grid().cell(x, y));
    }
}

TEST_CASE("mark_dirty_range merges a full span in one update") {
    TerminalEngine engine(20, 1);
    engine.grid().reset_damage();
    auto& row = engine.grid().row(0);
    row.mark_dirty_range(3, 9);
    CHECK(row.dirty);
    CHECK(row.damage_start == 3);
    CHECK(row.damage_end == 9);
    row.mark_dirty_range(1, 5);
    CHECK(row.damage_start == 1);
    CHECK(row.damage_end == 9);
}

TEST_CASE("CSI parameters are initialized lazily and never leak between sequences") {
    TerminalEngine engine(10, 3);
    VtParser parser(engine);

    parser.feed("\x1b[9;8H"); // Initializes two slots; row is clamped by the 3-row grid.
    CHECK(engine.cursor_row() == 2);
    CHECK(engine.cursor_col() == 7);

    parser.feed("\x1b[H"); // No digits: both parameters must be defaults.
    CHECK(engine.cursor_row() == 0);
    CHECK(engine.cursor_col() == 0);

    parser.feed("\x1b[;4H"); // Omitted first parameter.
    CHECK(engine.cursor_row() == 0);
    CHECK(engine.cursor_col() == 3);

    parser.feed("\x1b[4:3m");
    CHECK(static_cast<int>(engine.current_attrs().underline_style) == 3);
    parser.feed("\x1b[4m"); // Old colon subparameter must not survive.
    CHECK(static_cast<int>(engine.current_attrs().underline_style) == 1);
}

TEST_CASE("Short ASCII runs and inline UTF-8 decode match byte-wise feeding") {
    const std::string payload = "a\xe4\xb8\xad" "b\xf0\x9f\x98\x80" "c";
    TerminalEngine whole(20, 2), split(20, 2);
    VtParser a(whole), b(split);
    a.feed(payload);
    for (const char c : payload) b.feed(reinterpret_cast<const uint8_t*>(&c), 1);

    CHECK(whole.cursor_col() == split.cursor_col());
    CHECK(whole.cursor_row() == split.cursor_row());
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 20; ++x)
            CHECK(whole.grid().cell(x, y) == split.grid().cell(x, y));
    CHECK(whole.grid().cell(1, 0).codepoint == U'中');
    CHECK(whole.grid().cell(4, 0).codepoint == 0x1f600);
}

TEST_CASE("UTF-8 split at a continuation byte uses the stateful parser") {
    TerminalEngine engine(10, 2);
    VtParser parser(engine);
    const std::string payload = "A\xe4\xb8\xad" "B";
    parser.feed(std::string_view(payload).substr(0, 3));
    parser.feed(std::string_view(payload).substr(3));
    CHECK(engine.grid().cell(0, 0).codepoint == U'A');
    CHECK(engine.grid().cell(1, 0).codepoint == U'中');
    CHECK(engine.grid().cell(1, 0).attrs.width == 2);
    CHECK(engine.grid().cell(2, 0).attrs.width == 0);
    CHECK(engine.grid().cell(3, 0).codepoint == U'B');
}

TEST_CASE("Common CJK width fast path preserves zero-width katakana marks") {
    TerminalEngine engine(16, 2);
    VtParser parser(engine);
    parser.feed("\xe3\x81\x82\xe3\x82\x99"); // あ + combining voiced mark
    CHECK(engine.cursor_col() == 2);
    CHECK(engine.grid().cell(0, 0).codepoint == U'あ');
    CHECK(engine.grid().get_cell_extra(engine.grid().cell(0, 0).extra_idx).combining_idx != 0);

    parser.feed("\xe4\xb8\x80\xef\xbc\x81"); // 一 + fullwidth !
    CHECK(engine.cursor_col() == 6);
}

TEST_CASE("print_ascii_run honors insert mode and DEC graphics fallback") {
    TerminalEngine fast(10, 2), slow(10, 2);
    fast.set_insert_mode(true);
    slow.set_insert_mode(true);
    fast.print_ascii_run("ABC");
    slow.print_char(U'A');
    slow.print_char(U'B');
    slow.print_char(U'C');
    CHECK(fast.cursor_col() == slow.cursor_col());
    for (int x = 0; x < 10; ++x)
        CHECK(fast.grid().cell(x, 0) == slow.grid().cell(x, 0));

    fast.set_insert_mode(false);
    slow.set_insert_mode(false);
    fast.designate_charset(0, true);
    slow.designate_charset(0, true);
    fast.print_ascii_run("ab");
    slow.print_char(U'a');
    slow.print_char(U'b');
    CHECK(fast.grid().cell(0, 0) == slow.grid().cell(0, 0));
    CHECK(fast.grid().cell(1, 0) == slow.grid().cell(1, 0));
}

TEST_CASE("Bracketed paste payloads are wrapped and cannot close themselves") {
    TerminalEngine engine(40, 5);
    VtParser parser(engine);

    CHECK_FALSE(engine.bracketed_paste());
    CHECK(engine.format_paste_payload("raw\x1b[201~") == "raw\x1b[201~");

    parser.feed("\x1b[?2004h");
    REQUIRE(engine.bracketed_paste());
    CHECK(engine.format_paste_payload("caf\xc3\xa9") ==
          "\x1b[200~caf\xc3\xa9\x1b[201~");
    CHECK(engine.format_paste_payload("a\x1b[201~b") == "\x1b[200~ab\x1b[201~");
    CHECK(engine.format_paste_payload("\x1b[201~\x1b[201~") ==
          "\x1b[200~\x1b[201~");
    // Deleting one terminator must not join an earlier prefix and a later
    // '~' into a fresh terminator.
    CHECK(engine.format_paste_payload("\x1b[201\x1b[201~~") ==
          "\x1b[200~\x1b[201~");
    CHECK(engine.format_paste_payload("") == "\x1b[200~\x1b[201~");

    parser.feed("\x1b[?2004l");
    CHECK_FALSE(engine.bracketed_paste());
    CHECK(engine.format_paste_payload("plain") == "plain");
}

TEST_CASE("OSC 22 maps CSS/X11 pointer names and RIS resets the pointer") {
    TerminalEngine engine(10, 2);
    VtParser parser(engine);
    int changes = 0;
    gritty::platform::MouseCursorShape last{gritty::platform::MouseCursorShape::Default};
    parser.set_mouse_cursor_shape_handler([&](gritty::platform::MouseCursorShape shape) {
        last = shape;
        ++changes;
    });
    using Shape = gritty::platform::MouseCursorShape;
    const std::pair<const char*, Shape> cases[] = {
        {"default", Shape::Default}, {"left_ptr", Shape::Default},
        {"text", Shape::Text}, {"xterm", Shape::Text},
        {"pointer", Shape::Pointer}, {"hand2", Shape::Pointer},
        {"ew-resize", Shape::EwResize}, {"col-resize", Shape::EwResize},
        {"sb_h_double_arrow", Shape::EwResize},
        {"ns-resize", Shape::NsResize}, {"row-resize", Shape::NsResize},
        {"sb_v_double_arrow", Shape::NsResize},
        {"nesw-resize", Shape::NeswResize}, {"nwse-resize", Shape::NwseResize},
        {"crosshair", Shape::Crosshair}, {"move", Shape::Move},
        {"not-allowed", Shape::NotAllowed}, {"none", Shape::Hidden},
        {"bogus-name", Shape::Default},
    };
    for (const auto& [name, expected] : cases) {
        INFO(name);
        parser.feed(std::string("\x1b]22;") + name + "\x07");
        CHECK(engine.mouse_cursor_shape() == expected);
        CHECK(last == expected);
    }
    parser.feed("\x1b]22;text\x07");
    REQUIRE(engine.mouse_cursor_shape() == Shape::Text);
    parser.feed("\x1b" "c");
    CHECK(engine.mouse_cursor_shape() == Shape::Default);
    CHECK(last == Shape::Default);
    CHECK(changes == static_cast<int>(std::size(cases)) + 2);

    // A query's terminator is mirrored, including single-byte ST.
    std::string replies;
    parser.set_response_callback([&](std::string_view reply) { replies.append(reply); });
    parser.feed("\x1b]10;?\x9c");
    CHECK(replies == "\x1b]10;rgb:dddd/dddd/dddd\x1b\\");
}

TEST_CASE("OSC 7 accepts only local file URLs and decodes the path") {
    TerminalEngine engine(10, 2);
    VtParser parser(engine);
    parser.feed("\x1b]7;file:///Users/test/my%20dir\x07");
    CHECK(engine.cwd() == "/Users/test/my dir");
    parser.feed("\x1b]7;FILE://LOCALHOST/tmp/x\x1b\\");
    CHECK(engine.cwd() == "/tmp/x");
    parser.feed("\x1b]7;file://remote.example/tmp/x\x07");
    CHECK(engine.cwd() == "/tmp/x");
    parser.feed("\x1b]7;file:///tmp/%00bad\x07");
    CHECK(engine.cwd() == "/tmp/x");
    parser.feed("\x1b]7;file:///tmp/%zz\x07");
    CHECK(engine.cwd() == "/tmp/x");
    parser.feed("\x1b]7;relative/path\x07");
    CHECK(engine.cwd() == "/tmp/x");
}

TEST_CASE("OSC 777 notifications keep semicolons in the body") {
    TerminalEngine engine(10, 2);
    VtParser parser(engine);
    std::string title, body;
    int calls = 0;
    parser.set_notification_handler([&](std::string_view t, std::string_view b) {
        title.assign(t);
        body.assign(b);
        ++calls;
    });
    parser.feed("\x1b]777;notify;Title;Body;with;semicolon\x07");
    REQUIRE(calls == 1);
    CHECK(title == "Title");
    CHECK(body == "Body;with;semicolon");
    parser.feed("\x1b]777;notify;NoBody\x1b\\");
    CHECK(calls == 1);
}

TEST_CASE("OSC 52 four-state policy gates writes, clears and reads independently") {
    using gritty::config::Osc52Mode;
    struct Case {
        Osc52Mode mode;
        bool write_allowed;
        bool clear_allowed;
        bool read_allowed;
    };
    const Case cases[] = {
        {Osc52Mode::Disabled, false, false, false},
        {Osc52Mode::CopyOnly, true, true, false},
        {Osc52Mode::PasteOnly, false, false, true},
        {Osc52Mode::Enabled, true, true, true},
    };
    for (const auto& test : cases) {
        TerminalEngine engine(10, 2);
        VtParser parser(engine);
        parser.set_osc52_mode(test.mode);
        std::string replies, written = "stale";
        int writes = 0, clears = 0, reads = 0;
        parser.set_response_callback([&](std::string_view reply) { replies.append(reply); });
        parser.set_clipboard_handler([&](std::string_view text) {
            if (text.empty()) ++clears;
            else { written.assign(text); ++writes; }
        });
        parser.set_clipboard_read_handler([&] { ++reads; return std::string("secret"); });

        parser.feed("\x1b]52;c;aGVsbG8\x07");       // "hello"
        parser.feed("\x1b]52;c;!\x07");             // Explicit clear.
        parser.feed("\x1b]52;;?\x07");             // Empty target => "c".
        CHECK(writes == (test.write_allowed ? 1 : 0));
        CHECK(clears == (test.clear_allowed ? 1 : 0));
        CHECK(reads == (test.read_allowed ? 1 : 0));
        if (test.write_allowed) CHECK(written == "hello");
        CHECK(replies == (test.read_allowed ? "\x1b]52;c;c2VjcmV0\x07" : ""));

        // The reply terminator mirrors the request; denials stay silent.
        replies.clear();
        parser.feed("\x1b]52;p;?\x1b\\");
        CHECK(replies == (test.read_allowed ? "\x1b]52;p;c2VjcmV0\x1b\\" : ""));
    }
}

TEST_CASE("Unknown and oversized OSC payloads are consumed without grid output") {
    TerminalEngine engine(10, 2);
    VtParser parser(engine);
    parser.feed("\x1b]9;4;1;50\x07\x1b]133;A\x1b\\");
    parser.feed("\x1b]1337;" + std::string(256 * 1024, 'x') + "\x07");
    for (int y = 0; y < engine.rows(); ++y)
        for (int x = 0; x < engine.cols(); ++x)
            CHECK(engine.grid().cell(x, y).codepoint == U' ');
    parser.feed("OK");
    CHECK(engine.grid().cell(0, 0).codepoint == U'O');
    CHECK(engine.grid().cell(1, 0).codepoint == U'K');
    CHECK(engine.cursor_col() == 2);
}
