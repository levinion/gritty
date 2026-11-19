#include <doctest/doctest.h>
#include "font_fixture.hpp"
#include "core/terminal_engine.hpp"
#include "render/font_engine.hpp"
#include <algorithm>
#include <utility>
#include <vector>

using namespace gritty;

namespace {
struct GlyphPixels {
    int width, height, x, y;
    bool subpixel;
    std::vector<uint8_t> pixels;
    bool operator==(const GlyphPixels&) const = default;
};
GlyphPixels snapshot(const render::CachedGlyph* glyph) {
    REQUIRE(glyph != nullptr);
    GlyphPixels result{glyph->width, glyph->height, glyph->bearing_x, glyph->bearing_y, glyph->is_subpixel, {}};
    if (!glyph->mask.valid()) return result;
    const int bytes = glyph->width * (glyph->is_subpixel ? 4 : 1);
    for (int y = 0; y < glyph->height; ++y) {
        const auto* row = reinterpret_cast<const uint8_t*>(glyph->mask.data()) + y * glyph->mask.stride();
        result.pixels.insert(result.pixels.end(), row, row + bytes);
    }
    return result;
}
void check_text(const core::ScreenGrid& grid, uint32_t id, std::span<const char32_t> expected) {
    const auto actual = grid.get_combining_codepoints(id);
    REQUIRE(actual.size() == expected.size());
    CHECK(std::equal(actual.begin(), actual.end(), expected.begin()));
}
}

TEST_CASE("Repeated combining text neither allocates unique entries nor loses wide leaders") {
    core::TerminalEngine terminal(8, 3);
    for (int i = 0; i < 50000; ++i) {
        terminal.set_cursor_pos(0, 0);
        terminal.print_char(U'e');
        terminal.print_char(U'\u0301');
    }
    CHECK(terminal.grid().combining_entry_count() == 1);
    CHECK(terminal.grid().combining_storage_bytes() < 1024);
    terminal.set_cursor_pos(2, 0);
    terminal.print_char(U'中');
    terminal.print_char(U'\u0301');
    CHECK(terminal.grid().get_cell_extra(terminal.grid().cell(2, 0).extra_idx).combining_idx != 0);
    CHECK(terminal.grid().cell(3, 0).extra_idx == 0);
}

TEST_CASE("Combining collection drops scrolled-off references and keeps grid copies independent") {
    core::ScreenGrid grid(4, 2);
    const char32_t acute[] = {U'\u0301'}, cedilla[] = {U'\u0327'};
    core::Cell a;
    a.codepoint = U'a';
    const uint32_t acute_id = grid.store_combining_codepoints(acute);
    grid.set_cell_with_extra(0, 0, a, core::CellExtra{.combining_idx = acute_id});
    grid.scroll_up(0, 1, 1); // The only acute reference leaves the visible grid.
    core::Cell b = a;
    const uint32_t cedilla_id = grid.store_combining_codepoints(cedilla);
    grid.set_cell_with_extra(1, 0, b, core::CellExtra{.combining_idx = cedilla_id});
    for (int i = 0; i < 50000; ++i) {
        const char32_t sequence[] = {static_cast<char32_t>(0x10000 + i), U'\u0301'};
        core::Cell next;
        grid.set_cell_with_extra(2, 1, next,
            core::CellExtra{.combining_idx = grid.store_combining_codepoints(sequence)});
    }
    CHECK(grid.combining_storage_bytes() < 1024 * 1024);
    grid.collect_combining_garbage();
    CHECK(grid.combining_entry_count() == 2); // Cedilla plus the last visible sequence.
    CHECK(grid.get_combining_codepoints(acute_id).empty());
    check_text(grid, cedilla_id, cedilla);

    const uint32_t readded = grid.store_combining_codepoints(acute);
    grid.set_cell_with_extra(0, 0, a, core::CellExtra{.combining_idx = readded});
    auto copy = grid;
    copy.collect_combining_garbage();
    check_text(copy, readded, acute); // Copies own their side tables.
    grid.erase_in_display(0, 0, 2);
    grid.collect_combining_garbage();
    CHECK(grid.combining_entry_count() == 0);
    CHECK(grid.combining_storage_bytes() == 0);
    check_text(copy, readded, acute); // Erasing one grid cannot free the other's entry.
}

TEST_CASE("Combining hash probe collisions compare full sequence content") {
    core::CombiningStore store;
    std::vector<uint32_t> ids;
    for (int i = 0; i < 1000; ++i) {
        const char32_t text[] = {static_cast<char32_t>(0x1000 + i * 16), U'\u0301'};
        ids.push_back(store.intern(text));
    }
    for (int i = 0; i < 1000; ++i) {
        const char32_t text[] = {static_cast<char32_t>(0x1000 + i * 16), U'\u0301'};
        CHECK(store.intern(text) == ids[i]);
        CHECK(store.get(ids[i])[0] == text[0]);
    }
    CHECK(store.size() == 1000);
}

TEST_CASE("Combining stores copy independently and moved-from stores remain reusable") {
    core::CombiningStore source;
    const char32_t original[] = {U'\u0301', U'\u0327'};
    const auto id = source.intern(original);
    auto copy = source;
    core::CombiningStore moved = std::move(copy);
    CHECK(copy.size() == 0);
    CHECK(copy.storage_bytes() == 0);
    CHECK(moved.get(id).size() == 2);
    REQUIRE(copy.intern(original) != 0);
    moved.begin_collection();
    moved.sweep();
    CHECK(moved.storage_bytes() == 0);
    CHECK(source.get(id).size() == 2);
    source = std::move(copy);
    CHECK(copy.size() == 0);
    CHECK(source.get(1).size() == 2);
}

TEST_CASE("Collection preserves a borrowed input span and an explicit append prefix") {
    core::ScreenGrid grid(2, 1);
    uint32_t first = 0;
    for (int i = 0; i < 4096; ++i) {
        const char32_t text[] = {static_cast<char32_t>(0x10000 + i), U'\u0327'};
        const auto id = grid.store_combining_codepoints(text);
        if (i == 0) first = id;
    }
    SUBCASE("borrowed suffix") {
        const auto borrowed = grid.get_combining_codepoints(first).subspan(1);
        const auto id = grid.store_combining_codepoints(borrowed);
        const char32_t expected[] = {U'\u0327'};
        check_text(grid, id, expected);
        CHECK(grid.combining_entry_count() == 1);
    }
    SUBCASE("append pins prefix even without a cell") {
        const auto id = grid.append_combining_codepoint(first, U'\u0301');
        const char32_t expected[] = {U'\U00010000', U'\u0327', U'\u0301'};
        check_text(grid, id, expected);
    }
}

TEST_CASE("Long combining sequences survive periodic collection without truncation") {
    core::TerminalEngine terminal(4, 2);
    terminal.print_char(U'e');
    for (int i = 0; i < 5000; ++i) terminal.print_char(U'\u0301');
    terminal.grid().collect_combining_garbage();
    const auto text = terminal.grid().get_combining_codepoints(
        terminal.grid().get_cell_extra(terminal.grid().cell(0, 0).extra_idx).combining_idx);
    REQUIRE(text.size() == 5000);
    CHECK(std::all_of(text.begin(), text.end(), [](char32_t cp) { return cp == U'\u0301'; }));
    CHECK(terminal.grid().combining_entry_count() == 1);
    CHECK(terminal.grid().combining_storage_bytes() < 512 * 1024);
    terminal.set_alt_screen(true);
    terminal.print_char(U'x');
    terminal.print_char(U'\u0327');
    terminal.reset();
    CHECK(terminal.primary_grid().combining_storage_bytes() == 0);
    CHECK(terminal.alt_grid().combining_storage_bytes() == 0);
}

TEST_CASE("Glyph LRU preserves recently used entries and reports its budget") {
    render::FontEngine fonts(render::AntialiasMode::Grayscale, {1024 * 1024, 2});
    REQUIRE(fonts.load_font(test_font_path(), 13.0, 192));
    REQUIRE(fonts.get_glyph(U'A', false, false) != nullptr);
    REQUIRE(fonts.get_glyph(U'B', false, false) != nullptr);
    REQUIRE(fonts.get_glyph(U'A', false, false) != nullptr);
    REQUIRE(fonts.get_glyph(U'C', false, false) != nullptr);
    REQUIRE(fonts.get_glyph(U'A', false, false) != nullptr);
    CHECK(fonts.cache_stats().misses == 3);
    CHECK(fonts.cache_stats().hits == 2);
    CHECK(fonts.cache_stats().evictions == 1);
    REQUIRE(fonts.get_glyph(U'B', false, false) != nullptr);
    CHECK(fonts.cache_stats().misses == 4);
    CHECK(fonts.cache_stats().entries == 2);
    fonts.clear_cache();
    CHECK(fonts.cache_stats().entries == 0);
    CHECK(fonts.cache_stats().bitmap_bytes == 0);
}

TEST_CASE("Eviction and rerasterization preserve grayscale LCD mono and style pixels") {
    for (const auto mode : {render::AntialiasMode::Grayscale, render::AntialiasMode::SubpixelLcd, render::AntialiasMode::None}) {
        render::FontEngine limited(mode, {16 * 1024, 16});
        render::FontEngine reference(mode, {0, 0});
        REQUIRE(limited.load_font(test_font_path(), 13.0, 192));
        REQUIRE(reference.load_font(test_font_path(), 13.0, 192));
        for (int pass = 0; pass < 2; ++pass) {
            for (char32_t cp = U'A'; cp <= U'~'; ++cp) {
                for (int style = 0; style < 4; ++style) {
                    CHECK(snapshot(limited.get_glyph(cp, style & 1, style & 2)) ==
                        snapshot(reference.get_glyph(cp, style & 1, style & 2)));
                    CHECK(limited.cache_stats().entries <= 16);
                    CHECK(limited.cache_stats().bitmap_bytes <= 16 * 1024);
                }
            }
        }
        CHECK(limited.cache_stats().evictions > 0);
    }
}

TEST_CASE("Glyph byte budgets evict even below the entry-count ceiling") {
    render::FontEngine fonts;
    REQUIRE(fonts.load_font(test_font_path(), 13.0, 192));
    const auto* a = fonts.get_glyph(U'A', false, false);
    REQUIRE(a != nullptr);
    const size_t budget = 2 * static_cast<size_t>(a->mask.stride()) * a->height;
    REQUIRE(budget > 0);
    fonts.set_cache_limits({budget, 1000});
    for (char32_t cp = U'A'; cp <= U'Z'; ++cp) {
        REQUIRE(fonts.get_glyph(cp, false, false) != nullptr);
        CHECK(fonts.cache_stats().bitmap_bytes <= budget);
    }
    CHECK(fonts.cache_stats().evictions > 0);
    CHECK(fonts.cache_stats().entries < 26);
}

TEST_CASE("Oversized glyphs render through one accounted transient slot") {
    render::FontEngine fonts(render::AntialiasMode::Grayscale, {1, 16});
    render::FontEngine reference;
    REQUIRE(fonts.load_font(test_font_path(), 13.0, 192));
    REQUIRE(reference.load_font(test_font_path(), 13.0, 192));
    CHECK(snapshot(fonts.get_glyph(U'W', true, true)) == snapshot(reference.get_glyph(U'W', true, true)));
    CHECK(fonts.cache_stats().entries == 0);
    CHECK(fonts.cache_stats().bitmap_bytes == 0);
    CHECK(fonts.cache_stats().transient_bitmap_bytes > 1);
    fonts.set_cache_limits({1024 * 1024, 16});
    CHECK(fonts.cache_stats().transient_bitmap_bytes == 0);
    REQUIRE(fonts.get_glyph(U'W', true, true) != nullptr);
    CHECK(fonts.cache_stats().entries == 1);
    REQUIRE(fonts.load_font(test_font_path(), 14.0));
    CHECK(fonts.cache_stats().entries == 0);
}

TEST_CASE("Non-ASCII L1 cache preserves folded-index collisions") {
    render::FontEngine fonts(render::AntialiasMode::Grayscale, {1024 * 1024, 16});
    REQUIRE(fonts.load_font(test_font_path(), 13.0, 96));

    // Search two distinct codepoints that collide in the 256-slot folded index
    // and are covered by the test font.
    const auto folded = [](char32_t cp) { return static_cast<size_t>((cp ^ (cp >> 8)) & 0xff); };
    char32_t first = 0, second = 0;
    for (char32_t a = 0x4e00; a < 0x9fa0 && second == 0; ++a) {
        const auto* ga = fonts.get_glyph(a, false, false);
        if (!ga || ga->width == 0) continue;
        for (char32_t b = a + 1; b < 0x9fa0 && folded(b) == folded(a); ++b) {
            const auto* gb = fonts.get_glyph(b, false, false);
            if (gb && gb->width > 0 && ga != gb) { first = a; second = b; break; }
        }
    }
    if (first == 0) { WARN("Font has no colliding covered CJK pair"); return; }

    const auto* a_glyph = fonts.get_glyph(first, false, false);
    const auto* b_glyph = fonts.get_glyph(second, false, false);
    REQUIRE(a_glyph != nullptr);
    REQUIRE(b_glyph != nullptr);
    CHECK(a_glyph != b_glyph); // The tag must prevent slot aliasing.
    CHECK(fonts.get_glyph(first, false, false) == a_glyph);
    CHECK(fonts.get_glyph(second, false, false) == b_glyph);

    fonts.clear_cache();
    const auto* after_clear = fonts.get_glyph(first, false, false);
    REQUIRE(after_clear != nullptr);
    CHECK(fonts.get_glyph(second, false, false) != after_clear);
}
