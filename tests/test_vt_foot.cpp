#include <doctest/doctest.h>
#include <string>
#include "core/vt_parser.hpp"

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
    CHECK(replies == "\x1b[?62;22c\x1b[>1;100;0c\x1bP>|gritty(0.1.0)\x1b\\"
                     "\x1b[0n\x1b[1;1R\x1b[?2026;2$y\x1b[?69;0$y");
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
    CHECK(replies == "\x1b]10;rgb:dddd/dddd/dddd\x1b\\\x1b]11;rgb:1111/1111/1111\x1b\\");
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

TEST_CASE("foot scrolling, BCE, IRM and erase-scrollback semantics") {
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
