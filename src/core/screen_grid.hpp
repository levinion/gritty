#pragma once

#include <cstdint>
#include <vector>
#include <span>
#include <type_traits>
#include <algorithm>
#include "core/combining_store.hpp"
#include "core/cell_extra_store.hpp"
#include "core/screen_grid_cell.hpp"

namespace gritty::core {

struct Row {
    std::vector<Cell> cells;
    bool wrapped{false};
    bool dirty{true};
    int damage_start{0};
    int damage_end{-1};

    void mark_dirty(int col) noexcept {
        dirty = true;
        if (damage_end < 0) {
            damage_start = col;
            damage_end = col;
        } else {
            if (col < damage_start) damage_start = col;
            if (col > damage_end) damage_end = col;
        }
    }

    // Merge a whole dirty column span in one operation; used by printable ASCII
    // runs to avoid one bounds update per character.
    void mark_dirty_range(int start_col, int end_col) noexcept {
        if (end_col < start_col) return;
        dirty = true;
        if (damage_end < 0) {
            damage_start = start_col;
            damage_end = end_col;
        } else {
            damage_start = std::min(damage_start, start_col);
            damage_end = std::max(damage_end, end_col);
        }
    }

    void mark_all_dirty(int cols) noexcept {
        dirty = true;
        damage_start = 0;
        damage_end = cols - 1;
    }

    void reset_damage() noexcept {
        dirty = false;
        damage_start = 0;
        damage_end = -1;
    }
};

// A whole-width vertical shift of a scroll region, in rows. Recorded by the
// grid so the renderer can move pixels with memmove() instead of re-rasterizing
// every text row that merely changed position (foot render.c grid_render_scroll).
struct ScrollOp {
    int top{0};
    int bottom{0};
    int rows{0}; // > 0: content moved up (scroll_up / DL); < 0: moved down (scroll_down / IL).

    [[nodiscard]] bool valid() const noexcept { return rows != 0 && bottom >= top; }
    [[nodiscard]] int region_rows() const noexcept { return bottom - top + 1; }
};

class ScreenGrid final {
public:
    ScreenGrid(int cols, int rows);
    ~ScreenGrid() = default;

    ScreenGrid(const ScreenGrid&) = default;
    ScreenGrid& operator=(const ScreenGrid&) = default;
    ScreenGrid(ScreenGrid&&) noexcept = default;
    ScreenGrid& operator=(ScreenGrid&&) noexcept = default;

    void resize(int cols, int rows);

    [[nodiscard]] int cols() const noexcept { return cols_; }
    [[nodiscard]] int rows() const noexcept { return rows_; }

    [[nodiscard]] const Row& row(int y) const noexcept { return visible_rows_[y]; }
    [[nodiscard]] Row& row(int y) noexcept { return visible_rows_[y]; }

    [[nodiscard]] const Cell& cell(int x, int y) const noexcept { return visible_rows_[y].cells[x]; }
    [[nodiscard]] Cell& cell(int x, int y) noexcept { return visible_rows_[y].cells[x]; }

    void set_cell(int x, int y, const Cell& c) noexcept;
    // Side-table access. extra_idx == 0 is handled inline without a store lookup.
    [[nodiscard]] CellExtra get_cell_extra(uint16_t extra_idx) const noexcept {
        if (extra_idx == 0) return {};
        return cell_extras_.get(extra_idx);
    }
    uint16_t intern_cell_extra(const CellExtra& extra);
    // Intern first, then replace the whole cell. May allocate on first use of an
    // extra attribute combination, so it is not noexcept like set_cell().
    void set_cell_with_extra(int x, int y, const Cell& c, const CellExtra& extra);
    void clear_hyperlink_from_extras(uint16_t hyperlink_id) noexcept;
    [[nodiscard]] size_t cell_extra_count() const noexcept { return cell_extras_.size(); }
    [[nodiscard]] size_t cell_extra_storage_bytes() const noexcept { return cell_extras_.storage_bytes(); }

    // Scroll region manipulation
    void scroll_up(int top, int bottom, int count, const Cell& empty_cell = Cell{});
    void scroll_down(int top, int bottom, int count, const Cell& empty_cell = Cell{});

    // Pending pixel shifts, consumed by the renderer exactly once. Like foot's
    // scroll_damage list, several regions may shift in one frame (split panes);
    // the renderer applies them in order. A shift that replaces its whole
    // region, or more than `max_scroll_ops` regions in one frame, drops the
    // queue and repaints the affected regions instead.
    void take_pending_scrolls(std::vector<ScrollOp>& out) noexcept {
        out.assign(scroll_ops_.begin(), scroll_ops_.end());
        scroll_ops_.clear();
    }
    [[nodiscard]] bool has_pending_scroll() const noexcept { return !scroll_ops_.empty(); }
    void clear_pending_scroll() noexcept { scroll_ops_.clear(); }
    // Test/benchmark hook: 1 reproduces the old single-region behaviour.
    void set_max_pending_scrolls(size_t count) noexcept { max_scroll_ops_ = std::max<size_t>(1, count); }

    // Erase operations
    void erase_in_line(int x, int y, int mode, const Cell& empty_cell = Cell{});
    void erase_in_display(int x, int y, int mode, const Cell& empty_cell = Cell{});

    // Line/cell insert and delete
    void insert_lines(int y, int bottom, int count, const Cell& empty_cell = Cell{});
    void delete_lines(int y, int bottom, int count, const Cell& empty_cell = Cell{});
    void insert_cells(int x, int y, int count, const Cell& empty_cell = Cell{});
    void delete_cells(int x, int y, int count, const Cell& empty_cell = Cell{});

    // Damage tracking
    void mark_all_dirty() noexcept;
    void reset_damage() noexcept;

    // Grapheme cluster store
    uint32_t store_combining_codepoints(std::span<const char32_t> cps);
    uint32_t append_combining_codepoint(uint32_t prefix, char32_t cp);
    [[nodiscard]] std::span<const char32_t> get_combining_codepoints(uint32_t idx) const noexcept;
    [[nodiscard]] size_t combining_entry_count() const noexcept { return combining_store_.size(); }
    [[nodiscard]] size_t combining_storage_bytes() const noexcept { return combining_store_.storage_bytes(); }
    void collect_combining_garbage(uint32_t preserve = 0);

private:
    void clear_row(Row& row, const Cell& empty_cell);
    void record_scroll(int top, int bottom, int rows);
    void drop_scrolls();
    std::vector<ScrollOp> scroll_ops_;
    size_t max_scroll_ops_{8};
    int cols_;
    int rows_;
    std::vector<Row> visible_rows_;
    CombiningStore combining_store_;
    CellExtraStore cell_extras_;
};

} // namespace gritty::core
