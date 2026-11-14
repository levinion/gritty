#include <doctest/doctest.h>
#include "core/selection.hpp"
#include "core/overlays.hpp"
#include "core/screen_grid.hpp"

using namespace squirt::core;

TEST_CASE("SelectionController normal stream selection") {
    ScreenGrid grid(10, 3);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 10; ++x) {
            Cell c;
            c.codepoint = U'A' + (y * 10 + x);
            grid.set_cell(x, y, c);
        }
    }

    SelectionController sel;
    // Select from col 2 row 0 to col 5 row 1
    sel.start(2, 0, SelectionMode::Normal);
    sel.update(5, 1);

    CHECK(sel.is_active());
    CHECK(!sel.is_selected(1, 0));
    CHECK(sel.is_selected(2, 0));
    CHECK(sel.is_selected(9, 0));
    CHECK(sel.is_selected(0, 1));
    CHECK(sel.is_selected(5, 1));
    CHECK(!sel.is_selected(6, 1));

    std::string text = sel.extract_text(grid);
    CHECK(!text.empty());
    CHECK(text.starts_with("CDEF"));
}

TEST_CASE("SelectionController block selection") {
    ScreenGrid grid(10, 3);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 10; ++x) {
            Cell c;
            c.codepoint = U'0' + x;
            grid.set_cell(x, y, c);
        }
    }

    SelectionController sel;
    // Block select cols 2..4, rows 0..2
    sel.start(2, 0, SelectionMode::Block);
    sel.update(4, 2);

    CHECK(sel.is_selected(2, 0));
    CHECK(sel.is_selected(3, 1));
    CHECK(sel.is_selected(4, 2));
    CHECK(!sel.is_selected(1, 1));
    CHECK(!sel.is_selected(5, 1));
}

TEST_CASE("SearchOverlay key interaction") {
    SearchOverlay search;
    CHECK(!search.is_active());

    search.activate();
    CHECK(search.is_active());

    // Type "foo"
    search.handle_key(squirt::input::KeyEvent{.text = "f"});
    search.handle_key(squirt::input::KeyEvent{.text = "o"});
    search.handle_key(squirt::input::KeyEvent{.text = "o"});
    CHECK(search.query() == "foo");

    // Backspace
    search.handle_key(squirt::input::KeyEvent{.key = squirt::input::KeyCode::Backspace});
    CHECK(search.query() == "fo");

    // Escape closes overlay
    search.handle_key(squirt::input::KeyEvent{.key = squirt::input::KeyCode::Escape});
    CHECK(!search.is_active());
}

TEST_CASE("UrlOverlay hint interaction") {
    UrlOverlay url_overlay;
    url_overlay.activate();
    url_overlay.add_hint(5, 2, "aa", "https://github.com");

    CHECK(url_overlay.is_active());

    // Type 'a'
    url_overlay.handle_key(squirt::input::KeyEvent{.text = "a"});
    CHECK(url_overlay.is_active());

    // Type 'a' -> matches "aa", deactivates overlay
    url_overlay.handle_key(squirt::input::KeyEvent{.text = "a"});
    CHECK(!url_overlay.is_active());
}
