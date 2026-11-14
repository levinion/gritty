#include <doctest/doctest.h>
#include "core/screen_grid.hpp"

using namespace gritty::core;

TEST_CASE("ScreenGrid initialization and cell access") {
    ScreenGrid grid(80, 24, 100);
    CHECK(grid.cols() == 80);
    CHECK(grid.rows() == 24);
    CHECK(grid.scrollback_size() == 0);
    CHECK(grid.scrollback_capacity() == 100);

    Cell c;
    c.codepoint = U'A';
    c.fg = Color::make_rgb(255, 0, 0);
    grid.set_cell(5, 10, c);

    const Cell& read_back = grid.cell(5, 10);
    CHECK(read_back.codepoint == U'A');
    CHECK(read_back.fg.is_rgb());
    CHECK(read_back.fg.r() == 255);
    CHECK(read_back.fg.g() == 0);
    CHECK(read_back.fg.b() == 0);
}

TEST_CASE("ScreenGrid full screen scroll and scrollback") {
    ScreenGrid grid(10, 4, 10);
    // Fill row 0 with '1', row 1 with '2', etc.
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 10; ++x) {
            Cell c;
            c.codepoint = U'0' + y;
            grid.set_cell(x, y, c);
        }
    }

    CHECK(grid.scrollback_size() == 0);

    // Scroll up by 1
    grid.scroll_up(0, 3, 1);
    CHECK(grid.scrollback_size() == 1);
    CHECK(grid.scrollback_row(0).cells[0].codepoint == U'0');

    // Row 0 should now have '1'
    CHECK(grid.cell(0, 0).codepoint == U'1');
    CHECK(grid.cell(0, 1).codepoint == U'2');
    CHECK(grid.cell(0, 2).codepoint == U'3');
    // Row 3 should now be empty (default ' ')
    CHECK(grid.cell(0, 3).codepoint == ' ');

    // Scroll up by 2 more
    grid.scroll_up(0, 3, 2);
    CHECK(grid.scrollback_size() == 3);
    CHECK(grid.scrollback_row(1).cells[0].codepoint == U'1');
    CHECK(grid.scrollback_row(2).cells[0].codepoint == U'2');
    CHECK(grid.cell(0, 0).codepoint == U'3');
    CHECK(grid.cell(0, 1).codepoint == ' ');
}

TEST_CASE("ScreenGrid margined scroll does not enter scrollback") {
    ScreenGrid grid(10, 5, 10);
    for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 10; ++x) {
            Cell c;
            c.codepoint = U'0' + y;
            grid.set_cell(x, y, c);
        }
    }

    // Scroll margin between row 1 and row 3
    grid.scroll_up(1, 3, 1);
    CHECK(grid.scrollback_size() == 0);

    CHECK(grid.cell(0, 0).codepoint == U'0'); // Untouched
    CHECK(grid.cell(0, 1).codepoint == U'2'); // Shifted up
    CHECK(grid.cell(0, 2).codepoint == U'3'); // Shifted up
    CHECK(grid.cell(0, 3).codepoint == ' ');  // Cleared
    CHECK(grid.cell(0, 4).codepoint == U'4'); // Untouched
}

TEST_CASE("ScreenGrid erase line and display") {
    ScreenGrid grid(10, 5, 10);
    for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 10; ++x) {
            Cell c;
            c.codepoint = U'X';
            grid.set_cell(x, y, c);
        }
    }

    // Erase in line 2 mode 0 (cursor 5 to end)
    grid.erase_in_line(5, 2, 0);
    CHECK(grid.cell(4, 2).codepoint == U'X');
    CHECK(grid.cell(5, 2).codepoint == ' ');
    CHECK(grid.cell(9, 2).codepoint == ' ');

    // Erase in line 1 mode 1 (start to cursor 3)
    grid.erase_in_line(3, 1, 1);
    CHECK(grid.cell(0, 1).codepoint == ' ');
    CHECK(grid.cell(3, 1).codepoint == ' ');
    CHECK(grid.cell(4, 1).codepoint == U'X');

    // Erase in display mode 2 (entire screen)
    grid.erase_in_display(0, 0, 2);
    for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 10; ++x) {
            CHECK(grid.cell(x, y).codepoint == ' ');
        }
    }
}

TEST_CASE("ScreenGrid cell insertion and deletion") {
    ScreenGrid grid(5, 2, 10);
    for (int x = 0; x < 5; ++x) {
        Cell c;
        c.codepoint = U'A' + x;
        grid.set_cell(x, 0, c);
    }
    // Row 0 has "ABCDE"

    // Insert 2 cells at x=1
    grid.insert_cells(1, 0, 2);
    // Should be "A", ' ', ' ', "B", "C" (D and E shifted off)
    CHECK(grid.cell(0, 0).codepoint == U'A');
    CHECK(grid.cell(1, 0).codepoint == ' ');
    CHECK(grid.cell(2, 0).codepoint == ' ');
    CHECK(grid.cell(3, 0).codepoint == U'B');
    CHECK(grid.cell(4, 0).codepoint == U'C');

    // Delete 1 cell at x=1
    grid.delete_cells(1, 0, 1);
    // Should be "A", ' ', "B", "C", ' '
    CHECK(grid.cell(0, 0).codepoint == U'A');
    CHECK(grid.cell(1, 0).codepoint == ' ');
    CHECK(grid.cell(2, 0).codepoint == U'B');
    CHECK(grid.cell(3, 0).codepoint == U'C');
    CHECK(grid.cell(4, 0).codepoint == ' ');
}
