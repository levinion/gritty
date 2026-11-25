#include <doctest/doctest.h>
#include "core/screen_grid.hpp"

using namespace gritty::core;

TEST_CASE("ScreenGrid initialization and cell access") {
    ScreenGrid grid(80, 24);
    CHECK(grid.cols() == 80);
    CHECK(grid.rows() == 24);

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

TEST_CASE("ScreenGrid full screen scroll discards top rows") {
    ScreenGrid grid(10, 4);
    // Fill row 0 with '1', row 1 with '2', etc.
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 10; ++x) {
            Cell c;
            c.codepoint = U'0' + y;
            grid.set_cell(x, y, c);
        }
    }

    // Scroll up by 1; the top row is gone, not retained for history.
    grid.scroll_up(0, 3, 1);

    // Row 0 should now have '1'
    CHECK(grid.cell(0, 0).codepoint == U'1');
    CHECK(grid.cell(0, 1).codepoint == U'2');
    CHECK(grid.cell(0, 2).codepoint == U'3');
    // Row 3 should now be empty (default ' ')
    CHECK(grid.cell(0, 3).codepoint == ' ');

    // Scroll up by 2 more
    grid.scroll_up(0, 3, 2);
    CHECK(grid.cell(0, 0).codepoint == U'3');
    CHECK(grid.cell(0, 1).codepoint == ' ');
}

TEST_CASE("ScreenGrid margined scroll preserves rows outside the region") {
    ScreenGrid grid(10, 5);
    for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 10; ++x) {
            Cell c;
            c.codepoint = U'0' + y;
            grid.set_cell(x, y, c);
        }
    }

    // Scroll margin between row 1 and row 3
    grid.scroll_up(1, 3, 1);

    CHECK(grid.cell(0, 0).codepoint == U'0'); // Untouched
    CHECK(grid.cell(0, 1).codepoint == U'2'); // Shifted up
    CHECK(grid.cell(0, 2).codepoint == U'3'); // Shifted up
    CHECK(grid.cell(0, 3).codepoint == ' ');  // Cleared
    CHECK(grid.cell(0, 4).codepoint == U'4'); // Untouched
}

TEST_CASE("ScreenGrid erase line and display") {
    ScreenGrid grid(10, 5);
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
    ScreenGrid grid(5, 2);
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

TEST_CASE("Whole-region scrolls record pending pixel shifts") {
    ScreenGrid grid(10, 5);
    std::vector<gritty::core::ScrollOp> ops;
    CHECK_FALSE(grid.has_pending_scroll());

    grid.scroll_up(0, 4, 1);
    grid.take_pending_scrolls(ops);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].top == 0);
    CHECK(ops[0].bottom == 4);
    CHECK(ops[0].rows == 1);
    CHECK_FALSE(grid.has_pending_scroll());

    // Same region and direction coalesce into one shift.
    grid.scroll_up(0, 4, 1);
    grid.scroll_up(0, 4, 2);
    grid.take_pending_scrolls(ops);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].rows == 3);

    // Downward motion is the opposite sign.
    grid.scroll_down(1, 3, 1);
    grid.take_pending_scrolls(ops);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].rows == -1);
    CHECK(ops[0].top == 1);
    CHECK(ops[0].bottom == 3);

    // Replacing the whole region cannot be reproduced by a pixel shift.
    grid.scroll_up(0, 4, 5);
    CHECK_FALSE(grid.has_pending_scroll());
    for (int y = 0; y < 5; ++y) CHECK(grid.row(y).dirty);
}

TEST_CASE("Distinct scroll regions queue independently with bounded fallback") {
    ScreenGrid grid(10, 6);
    std::vector<gritty::core::ScrollOp> ops;

    // Split panes: two regions scroll in the same frame.
    grid.scroll_up(0, 2, 1);
    grid.scroll_up(3, 5, 1);
    grid.take_pending_scrolls(ops);
    REQUIRE(ops.size() == 2);
    CHECK(ops[0].top == 0);
    CHECK(ops[0].bottom == 2);
    CHECK(ops[1].top == 3);
    CHECK(ops[1].bottom == 5);
    CHECK(ops[1].rows == 1);

    // Opposite directions in the same region are separate shifts too.
    grid.scroll_up(0, 2, 1);
    grid.scroll_down(0, 2, 1);
    grid.take_pending_scrolls(ops);
    REQUIRE(ops.size() == 2);
    CHECK(ops[0].rows == 1);
    CHECK(ops[1].rows == -1);

    // Exceeding the configured queue repaints every affected region instead.
    grid.set_max_pending_scrolls(1);
    grid.scroll_up(0, 2, 1);
    grid.scroll_up(3, 5, 1);
    CHECK_FALSE(grid.has_pending_scroll());
    for (int y = 0; y < 6; ++y) CHECK(grid.row(y).dirty);

    // A merged shift that replaces its region drops every queued shift.
    grid.set_max_pending_scrolls(8);
    grid.scroll_up(0, 5, 3);
    grid.scroll_up(0, 5, 3); // Merges to 6 == region size.
    CHECK_FALSE(grid.has_pending_scroll());
    for (int y = 0; y < 6; ++y) CHECK(grid.row(y).dirty);

    // A full repaint supersedes whatever shifts were pending.
    grid.scroll_up(0, 2, 1);
    REQUIRE(grid.has_pending_scroll());
    grid.mark_all_dirty();
    CHECK_FALSE(grid.has_pending_scroll());

    // Insert/delete lines are scrolls too.
    ScreenGrid lines(10, 5);
    lines.delete_lines(1, 4, 1);
    lines.take_pending_scrolls(ops);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].rows == 1);
    lines.insert_lines(1, 4, 1);
    lines.take_pending_scrolls(ops);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].rows == -1);
}

TEST_CASE("Scrolling reuses discarded row storage rather than allocating each line") {
    ScreenGrid grid(8, 3);
    grid.row(0).cells.reserve(128);
    grid.row(1).cells.reserve(256);
    grid.row(2).cells.reserve(512);
    const auto* first = grid.row(0).cells.data();
    const auto capacity = grid.row(0).cells.capacity();
    grid.scroll_up(0, 2, 1);
    CHECK(grid.row(2).cells.data() == first);
    CHECK(grid.row(2).cells.capacity() == capacity);
    grid.scroll_down(0, 2, 1);
    CHECK(grid.row(0).cells.data() == first);
    CHECK(grid.row(0).cells.capacity() == capacity);
}

TEST_CASE("Shrinking the viewport drops obsolete wide row allocations") {
    ScreenGrid grid(128, 3);
    grid.scroll_up(0, 2, 2);
    grid.resize(8, 3);
    for (int y = 0; y < grid.rows(); ++y)
        CHECK(grid.row(y).cells.capacity() <= 16);
}

TEST_CASE("Full-screen ring scroll wraps cleanly across partial scrolls and resize") {
    ScreenGrid grid(6, 5);
    auto fill_row = [&](int y, char32_t ch) {
        Cell c;
        c.codepoint = ch;
        for (int x = 0; x < 6; ++x) grid.set_cell(x, y, c);
    };
    for (int y = 0; y < 5; ++y) fill_row(y, U'0' + y);

    // Full-screen scrolls wrap top_row_ past rows_ multiple times.
    for (int step = 0; step < 7; ++step) {
        grid.scroll_up(0, 4, 1);
        fill_row(4, U'A' + step);
    }
    // Logical rows after 7 scrolls: old rows gone, last 5 are 'C','D','E','F','G'.
    for (int y = 0; y < 5; ++y) CHECK(grid.cell(0, y).codepoint == U'C' + y);

    // Downward full-screen scroll followed by partial DECSTBM scroll [1..3].
    grid.scroll_down(0, 4, 2);
    CHECK(grid.cell(0, 0).codepoint == U' ');
    CHECK(grid.cell(0, 2).codepoint == U'C');
    CHECK(grid.cell(0, 4).codepoint == U'E');
    grid.scroll_up(1, 3, 1);
    CHECK(grid.cell(0, 1).codepoint == U'C');
    CHECK(grid.cell(0, 2).codepoint == U'D');
    CHECK(grid.cell(0, 3).codepoint == U' ');
    CHECK(grid.cell(0, 4).codepoint == U'E');

    // Resize shrink discards top row and normalizes ring order.
    grid.resize(6, 3);
    CHECK(grid.cell(0, 0).codepoint == U'D');
    CHECK(grid.cell(0, 1).codepoint == U' ');
    CHECK(grid.cell(0, 2).codepoint == U'E');
    grid.resize(6, 5);
    CHECK(grid.cell(0, 2).codepoint == U'E');
    CHECK(grid.cell(0, 4).codepoint == U' ');
}

TEST_CASE("clean_from tail tracking handles BCE backgrounds and scroll-overflow skips") {
    ScreenGrid grid(8, 4);
    Cell ch;
    ch.codepoint = U'X';
    grid.set_cell(2, 0, ch);
    CHECK(grid.row(0).clean_from == 3);

    // BCE scroll with non-default background fills whole cleared row and updates clean_bg.
    Cell bce_blank;
    bce_blank.bg = Color::make_indexed(2);
    grid.scroll_up(0, 3, 1, bce_blank);
    CHECK(grid.row(3).clean_from == 0);
    CHECK(grid.row(3).clean_bg == bce_blank.bg);
    for (int x = 0; x < 8; ++x) CHECK(grid.cell(x, 3).bg == bce_blank.bg);

    // Next scroll with default bg detects clean_bg mismatch and wipes whole row cleanly.
    Cell def_blank;
    grid.set_cell(0, 3, ch);
    grid.scroll_up(0, 3, 1); // Recycles row with bce_blank at tail -> row 3 has default blank on row 3? wait: row 2 was scrolled to 1, row 3 was scrolled to 2.
    grid.scroll_up(0, 3, 2, def_blank);
    for (int x = 0; x < 8; ++x) CHECK(grid.cell(x, 3).bg.is_default());

    // Scroll overflow marks all dirty once, skips redundant re-marks, and resets after take_pending_scrolls.
    std::vector<ScrollOp> ops;
    grid.take_pending_scrolls(ops);
    grid.reset_damage();
    for (int i = 0; i < 20; ++i) grid.scroll_up(0, 3, 1);
    CHECK_FALSE(grid.has_pending_scroll());
    for (int y = 0; y < 4; ++y) CHECK(grid.row(y).dirty);
    grid.take_pending_scrolls(ops);
    grid.scroll_up(0, 3, 1);
    CHECK(grid.has_pending_scroll());
}
