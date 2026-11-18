#include <doctest/doctest.h>
#include <utf8proc.h>
#include "core/vt_parser.hpp"
#include "width_cases.hpp"

TEST_CASE("Pinned Unicode width vectors preserve cursor movement and wide-cell damage") {
    for (const auto& item : width_cases) {
        INFO(std::string(item.name));
        CHECK(utf8proc_charwidth(item.codepoint) == item.width);
        gritty::core::TerminalEngine engine(16, 3);
        gritty::core::VtParser parser(engine);
        parser.feed("A");
        engine.grid().reset_damage();
        uint8_t encoded[4];
        const auto count = utf8proc_encode_char(item.codepoint, encoded);
        REQUIRE(count > 0);
        // Deliberately split every UTF-8 byte across reads.
        for (int i = 0; i < count; ++i) parser.feed(encoded + i, 1);
        CHECK(engine.cursor_col() == 1 + item.width);
        const auto& row = engine.grid().row(0);
        CHECK(row.dirty);
        if (item.width == 2) {
            CHECK(row.cells[1].attrs.width == 2);
            CHECK(row.cells[2].attrs.width == 0);
            CHECK(row.damage_start == 1);
            CHECK(row.damage_end == 2);
        }
        parser.feed("B");
        CHECK(engine.grid().cell(1 + item.width, 0).codepoint == U'B');
    }
}

TEST_CASE("Wide characters at the right edge occupy two cells before deferred wrapping") {
    gritty::core::TerminalEngine engine(4, 2);
    gritty::core::VtParser parser(engine);
    parser.feed("AB界");
    CHECK(engine.cursor_col() == 3);
    CHECK(engine.cursor_row() == 0);
    CHECK(engine.grid().cell(2, 0).attrs.width == 2);
    CHECK(engine.grid().cell(3, 0).attrs.width == 0);
    parser.feed("X");
    CHECK(engine.grid().row(0).wrapped);
    CHECK(engine.grid().cell(0, 1).codepoint == U'X');
    CHECK(engine.cursor_col() == 1);
}
