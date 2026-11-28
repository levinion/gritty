#include <doctest/doctest.h>
#include <string>
#include "core/vt_parser.hpp"
#include "util/version.hpp"

using namespace gritty::core;

// Behavioral references: upstream foot vt.c and tmux source tests.
TEST_CASE("foot ESC intermediates and SI/SO do not leak charset designators") {
    TerminalEngine engine(20, 4);
    VtParser parser(engine);
    parser.feed("\x1b(0lqk\x1b(BX\x1b)0\x0e" "qx\x0f" "A");
    const std::u32string expected = U"┌─┐X─│A";
    for (size_t i = 0; i < expected.size(); ++i)
        CHECK(engine.grid().cell(i, 0).codepoint == expected[i]);
    CHECK(engine.cursor_col() == expected.size());
}

TEST_CASE("foot REP and zero-default CSI counts preserve repeated tmux text") {
    TerminalEngine engine(20, 4);
    VtParser parser(engine);
    parser.feed("A\x1b[4b\x1b[0b");
    CHECK(engine.cursor_col() == 6);
    for (int col = 0; col < 6; ++col) CHECK(engine.grid().cell(col, 0).codepoint == U'A');
    parser.feed("\x1b[0D");
    CHECK(engine.cursor_col() == 5);
    parser.feed("\x1b[0;0H");
    CHECK(engine.cursor_col() == 0);
    CHECK(engine.cursor_row() == 0);
}

TEST_CASE("foot CSI prefix and intermediate dispatch never aliases normal SGR or cursor commands") {
    TerminalEngine engine(20, 4);
    VtParser parser(engine);
    parser.feed("\x1b[>4;2mX"); // XTMODKEYS, not underline + dim
    CHECK_FALSE(engine.grid().cell(0, 0).attrs.dim);
    CHECK(engine.grid().cell(0, 0).attrs.underline_style == 0);
    parser.feed("\x1b[?3b\x1b[2$H"); // unsupported private REP and intermediate CUP
    CHECK(engine.cursor_col() == 1);
    CHECK(engine.cursor_row() == 0);
}

TEST_CASE("foot DCS strings terminate without swallowing the next screen update") {
    TerminalEngine engine(20, 4);
    VtParser parser(engine);
    std::string payload;
    parser.set_dcs_handler([&](std::string_view value) { payload = value; });
    for (const char byte : std::string("\x1bP1;2qDATA\x07MORE\x1b\\OK"))
        parser.feed(std::string_view(&byte, 1));
    CHECK(payload == "1;2qDATA\x07MORE");
    CHECK(engine.grid().cell(0, 0).codepoint == U'O');
    CHECK(engine.grid().cell(1, 0).codepoint == U'K');
    parser.feed("\x1b]2;cancelled\x18Z");
    CHECK(engine.window_title().empty());
    CHECK(engine.grid().cell(2, 0).codepoint == U'Z');
    parser.feed("\x1b[2\x7f" "CX"); // DEL ignored within CSI
    CHECK(engine.grid().cell(5, 0).codepoint == U'X');
}

TEST_CASE("foot terminal discovery replies use an honest Gritty identity") {
    TerminalEngine engine(80, 24);
    VtParser parser(engine);
    std::string replies;
    parser.set_response_callback([&](std::string_view value) { replies += value; });
    parser.feed("\x1b[c\x1b[>c\x1b[>q\x1b[5n\x1b[6n\x1b[?2026$p\x1b[?69$p");
    const std::string xtversion = "\x1bP>|gritty(" +
        std::string(gritty::util::kVersion) + ")\x1b\\";
    CHECK(replies == "\x1b[?62;22c\x1b[>1;100;0c" + xtversion +
                     "\x1b[0n\x1b[1;1R\x1b[?2026;2$y\x1b[?69;0$y");
    // CSI > 0 q is the explicit form; other parameters are ignored.
    replies.clear();
    parser.feed("\x1b[>0q");
    CHECK(replies == xtversion);
    replies.clear();
    parser.feed("\x1b[>1q\x1b[>42;7q");
    CHECK(replies.empty());

    replies.clear();
    parser.feed("\x1b[?2026h\x1b[?2026$p\x1b[?2026l\x1b[3;10r\x1b[?6h\x1b[2;5H\x1b[6n");
    CHECK(replies == "\x1b[?2026;1$y\x1b[2;5R");
}

TEST_CASE("tmux OSC default color queries receive replies rather than time out") {
    TerminalEngine engine(80, 24);
    VtParser parser(engine);
    std::string replies;
    parser.set_response_callback([&](std::string_view value) { replies += value; });
    parser.feed("\x1b]10;?\x1b\\\x1b]11;?\x07");
    CHECK(replies == "\x1b]10;rgb:dddd/dddd/dddd\x1b\\\x1b]11;rgb:1111/1111/1111\x07");
}

TEST_CASE("foot alternate screen modes distinguish cursor save from screen switching") {
    TerminalEngine engine(20, 6);
    VtParser parser(engine);
    parser.feed("\x1b[3;4H\x1b[?47h");
    CHECK(engine.cursor_col() == 3);
    CHECK(engine.cursor_row() == 2);
    parser.feed("\x1b[4;7H\x1b[?47l");
    CHECK(engine.cursor_col() == 6);
    CHECK(engine.cursor_row() == 3);
    parser.feed("\x1b[?1048h\x1b[H\x1b[?1048l");
    CHECK_FALSE(engine.is_alt_screen());
    CHECK(engine.cursor_col() == 6);
    CHECK(engine.cursor_row() == 3);
    parser.feed("\x1b[?1049h\x1b[H\x1b[?1049l");
    CHECK(engine.cursor_col() == 6);
    CHECK(engine.cursor_row() == 3);
}

TEST_CASE("foot saved cursor includes pending wrap and charset but not visibility") {
    TerminalEngine engine(4, 3);
    VtParser parser(engine);
    parser.feed("ABCD\x1b" "7\x1b[H\x1b[?25l\x1b" "8X");
    CHECK(engine.grid().cell(0, 1).codepoint == U'X');
    CHECK_FALSE(engine.cursor_visible());
    parser.feed("\x1b(0\x1b" "7\x1b(B\x1b" "8q");
    CHECK(engine.grid().cell(1, 1).codepoint == U'─');
}

TEST_CASE("foot scrolling, BCE, IRM and DECSED 3 no-op semantics") {
    TerminalEngine engine(10, 6);
    VtParser parser(engine);
    parser.feed("KEEP\x1b[3J");
    CHECK(engine.grid().cell(0, 0).codepoint == U'K');
    parser.feed("\x1b[H\x1b[4hX\x1b[4l");
    CHECK(engine.grid().cell(0, 0).codepoint == U'X');
    CHECK(engine.grid().cell(1, 0).codepoint == U'K');
    parser.feed("\x1b[2;5r\x1b[1;1H\x1b[5B"); // origin off: relative movement uses the screen
    CHECK(engine.cursor_row() == 5);
    parser.feed("\x1b[3;3r"); // invalid margins ignored, cursor not homed
    CHECK(engine.margin_top() == 1);
    CHECK(engine.margin_bottom() == 4);
    CHECK(engine.cursor_row() == 5);
    parser.feed("\x1b[44m\x1b[5;1H\n");
    CHECK(engine.grid().cell(0, 4).bg == Color::make_indexed(4));
}

TEST_CASE("VT streaming results are independent of PTY read chunk boundaries") {
    const std::string stream = "before\x1b[?1049h\x1b[H\x1b[1;38;2;12;34;56m世界\r\n"
        "A\x1b[8b\x1b(0lqqk\x1b(B\x1b[3;7r\x1b[?6h\x1b[2;4H\x1b[6n"
        "\x1bP$q r\x1b\\\x1b]2;chunk-title\x1b\\\x1b[?6l\x1b[?1049l";
    TerminalEngine expected(30, 8);
    VtParser whole(expected);
    std::string expected_replies;
    whole.set_response_callback([&](std::string_view data) { expected_replies += data; });
    whole.feed(stream);
    for (size_t size = 1; size <= 31; ++size) {
        CAPTURE(size);
        TerminalEngine actual(30, 8);
        VtParser fragmented(actual);
        std::string replies;
        fragmented.set_response_callback([&](std::string_view data) { replies += data; });
        for (size_t offset = 0; offset < stream.size(); offset += size)
            fragmented.feed(std::string_view(stream).substr(offset, size));
        CHECK(replies == expected_replies);
        CHECK(actual.cursor_col() == expected.cursor_col());
        CHECK(actual.cursor_row() == expected.cursor_row());
        CHECK(actual.window_title() == expected.window_title());
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 30; ++x) {
            REQUIRE(actual.primary_grid().cell(x, y) == expected.primary_grid().cell(x, y));
            REQUIRE(actual.alt_grid().cell(x, y) == expected.alt_grid().cell(x, y));
        }
    }
}

TEST_CASE("VT parser recovers from oversized strings and interrupted UTF-8") {
    TerminalEngine engine(20, 4);
    VtParser parser(engine);
    parser.feed("\x1b]2;" + std::string(70 * 1024, 'x') + "\x1b\\OK");
    CHECK(engine.window_title().empty());
    CHECK(engine.grid().cell(0, 0).codepoint == U'O');
    parser.feed("\xe4");
    parser.feed("\r\xb8\x96Z");
    CHECK(engine.grid().cell(0, 0).codepoint == U'Z');
}

TEST_CASE("foot CSI parser keeps trailing empty parameters and rejects overflow parameter lists") {
    TerminalEngine engine(20, 6);
    VtParser parser(engine);
    parser.feed("\x1b[1;mX"); // trailing omitted SGR resets the bold attribute
    CHECK_FALSE(engine.grid().cell(0, 0).attrs.bold);
    parser.feed("\x1b[999999999999999999999999999999999C");
    CHECK(engine.cursor_col() == 19);
    const auto before = engine.cursor_col();
    parser.feed("\x1b[" + std::string(40, ';') + "H");
    CHECK(engine.cursor_col() == before);
}

TEST_CASE("foot OSC 9 and Kitty OSC 99 notifications support capability queries, chunking, and base64") {
    TerminalEngine engine(80, 24);
    VtParser parser(engine);
    std::string replies;
    std::vector<std::pair<std::string, std::string>> notes;
    parser.set_response_callback([&](std::string_view value) { replies += value; });
    parser.set_notification_handler([&](std::string_view title, std::string_view body) {
        notes.emplace_back(std::string(title), std::string(body));
    });

    // OSC 9: simple notification + ConEmu progress filter (OSC 9;4;...).
    parser.feed("\x1b]9;Build complete\x1b\\\x1b]9;4;1;50\x07\x1b]9;4\a");
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].first == "Gritty");
    CHECK(notes[0].second == "Build complete");

    // OSC 99 capability query preserves id and terminator.
    parser.feed("\x1b]99;i=q1:p=?;\x1b\\");
    CHECK(replies == "\x1b]99;i=q1:p=?;p=title,body:e=0,1:d=0,1\x1b\\");
    replies.clear();
    parser.feed("\x1b]99;p=?;\a");
    CHECK(replies == "\x1b]99;p=?;p=title,body:e=0,1:d=0,1\a");

    // Chunked base64 title + plain body under id n1.
    notes.clear();
    parser.feed("\x1b]99;i=n1:d=0:e=1:p=title;SGVsbG8=\x1b\\");
    CHECK(notes.empty());
    parser.feed("\x1b]99;i=n1:d=0:p=body;first \x1b\\");
    CHECK(notes.empty());
    parser.feed("\x1b]99;i=n1:d=1:p=body;second\x1b\\");
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].first == "Hello");
    CHECK(notes[0].second == "first second");
}

TEST_CASE("foot XTWINOPS title stack, XTSAVE/XTRESTORE, and DECRQSS m SGR queries") {
    TerminalEngine engine(40, 10);
    VtParser parser(engine);
    std::string replies;
    parser.set_response_callback([&](std::string_view value) { replies += value; });

    parser.feed("\x1b]2;base\x1b\\\x1b[22;2t\x1b]2;nested\x1b\\");
    CHECK(engine.window_title() == "nested");
    parser.feed("\x1b[23;2t");
    CHECK(engine.window_title() == "base");
    parser.feed("\x1b[23;2t"); // Popping empty stack is a no-op.
    CHECK(engine.window_title() == "base");

    // XTSAVE / XTRESTORE across private modes.
    parser.feed("\x1b[?2004h\x1b[?1004l\x1b[?2004;1004s");
    parser.feed("\x1b[?2004l\x1b[?1004h");
    CHECK_FALSE(engine.bracketed_paste());
    CHECK(engine.focus_events());
    parser.feed("\x1b[?2004;1004r");
    CHECK(engine.bracketed_paste());
    CHECK_FALSE(engine.focus_events());

    // DECRQSS m reports current SGR state.
    parser.feed("\x1b[0;1;3;4:3;31;48:2::10:20:30;58:5:9m");
    replies.clear();
    parser.feed("\x1bP$qm\x1b\\");
    CHECK(replies == "\x1bP1$r0;1;3;4:3;31;48:2::10:20:30;58:5:9m\x1b\\");
}

TEST_CASE("foot DECSET 2027 grapheme clustering, DECSET 2031 color scheme, 2048 resize, and BEL") {
    TerminalEngine engine(20, 4);
    engine.set_cell_size(10, 20);
    VtParser parser(engine);
    std::string replies;
    int bell_count = 0;
    parser.set_response_callback([&](std::string_view value) { replies += value; });
    parser.set_bell_handler([&] { ++bell_count; });

    // Mode 2027 enables grapheme cluster segmentation (ZWJ emoji + regional flag + VS16).
    parser.feed("\x1b[?2027h\x1b[?2027$p");
    CHECK(replies == "\x1b[?2027;1$y");
    parser.feed("👨\u200d👩\u200d👧X");
    CHECK(engine.cursor_col() == 3);
    CHECK(engine.grid().cell(0, 0).attrs.width == 2);
    CHECK(engine.grid().cell(2, 0).codepoint == U'X');
    const auto zwj_marks = engine.grid().get_combining_codepoints(
        engine.grid().get_cell_extra(engine.grid().cell(0, 0).extra_idx).combining_idx);
    CHECK(zwj_marks.size() == 4);

    // Regional indicator pair (🇨🇳) stays within one double-width cluster.
    parser.feed("\r\n\U0001F1E8\U0001F1F3Z");
    CHECK(engine.cursor_col() == 3);
    CHECK(engine.grid().cell(0, 1).attrs.width == 2);
    CHECK(engine.grid().cell(2, 1).codepoint == U'Z');

    // Mode 2031 + CSI ? 996 n color scheme query and dynamic dark<->light notification.
    replies.clear();
    parser.feed("\x1b[?996n\x1b[?2031h");
    CHECK(replies == "\x1b[?997;1n");
    replies.clear();
    parser.feed("\x1b]11;#ffffff\x1b\\"); // Switch default bg from #111111 (dark) -> light.
    CHECK(replies == "\x1b[?997;2n");
    replies.clear();
    parser.feed("\x1b]11;#eeeeee\x1b\\"); // Still light -> no duplicate report.
    CHECK(replies.empty());
    parser.feed("\x1b]11;#000000\x1b\\"); // Back to dark -> emits report.
    CHECK(replies == "\x1b[?997;1n");

    // Mode 2048 emits immediate resize report upon activation.
    replies.clear();
    parser.feed("\x1b[?2048h\x1b[?2048$p");
    CHECK(replies == "\x1b[48;4;20;80;200t\x1b[?2048;1$y");

    // BEL triggers handler directly and through tmux DCS passthrough.
    parser.feed("\x07\x1bPtmux;\x07\x1b\\");
    CHECK(bell_count == 2);
}
