#include "core/screen_grid.hpp"
#include <algorithm>
#include <cstring>
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace gritty::core {
namespace {

void fill_cells(Cell* dst, size_t count, const Cell& value) noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
    const uint32x4_t cp = vdupq_n_u32(static_cast<uint32_t>(value.codepoint));
    const uint32x4_t fg = vdupq_n_u32(value.fg.value);
    const uint32x4_t bg = vdupq_n_u32(value.bg.value);
    const uint32x4_t tail = vdupq_n_u32(cell_tail_word(value));
    const uint32x4x4_t block{cp, fg, bg, tail};
    size_t i = 0;
    for (; i + 4 <= count; i += 4)
        vst4q_u32(reinterpret_cast<uint32_t*>(dst + i), block);
    for (; i < count; ++i) dst[i] = value;
#else
    for (size_t i = 0; i < count; ++i) dst[i] = value;
#endif
}

} // namespace


ScreenGrid::ScreenGrid(int cols, int rows)
    : cols_(cols), rows_(rows) {
    visible_rows_.resize(rows_);
    for (auto& row : visible_rows_) {
        row.cells.resize(cols_);
        row.mark_all_dirty(cols_);
    }
}

void ScreenGrid::resize(int new_cols, int new_rows) {
    if (new_cols <= 0 || new_rows <= 0) return;
    scroll_ops_.clear();

    if (new_rows < rows_) {
        // Rows above the new viewport are discarded; tmux owns history.
        visible_rows_.erase(visible_rows_.begin(), visible_rows_.begin() + (rows_ - new_rows));
        collect_combining_garbage();
    } else if (new_rows > rows_) {
        for (int i = rows_; i < new_rows; ++i) {
            Row r;
            r.cells.resize(new_cols);
            r.mark_all_dirty(new_cols);
            visible_rows_.emplace_back(std::move(r));
        }
    }

    cols_ = new_cols;
    rows_ = new_rows;

    for (auto& row : visible_rows_) {
        if (row.cells.size() != static_cast<size_t>(cols_)) {
            if (row.cells.capacity() > 2 * static_cast<size_t>(cols_))
                std::vector<Cell>(cols_, Cell{}).swap(row.cells);
            else row.cells.resize(cols_);
        }
        row.mark_all_dirty(cols_);
    }
}

void ScreenGrid::set_cell(int x, int y, const Cell& c) noexcept {
    if (x < 0 || x >= cols_ || y < 0 || y >= rows_) return;
    auto& target = visible_rows_[y].cells[x];
    if (target != c) {
        target = c;
        visible_rows_[y].mark_dirty(x);
    }
}

void ScreenGrid::set_cell_with_extra(int x, int y, const Cell& c, const CellExtra& extra) {
    Cell cell = c;
    cell.extra_idx = intern_cell_extra(extra);
    set_cell(x, y, cell);
}

uint16_t ScreenGrid::intern_cell_extra(const CellExtra& extra) {
    if (extra.is_default()) return 0;
    if (combining_store_.needs_collection() || cell_extras_.needs_collection())
        collect_combining_garbage(extra.combining_idx);
    return cell_extras_.intern(extra);
}

void ScreenGrid::clear_hyperlink_from_extras(uint16_t hyperlink_id) noexcept {
    if (hyperlink_id == 0) return;
    const auto affected = [&](const Row& row) {
        for (const auto& cell : row.cells)
            if (cell.extra_idx != 0 &&
                cell_extras_.get(cell.extra_idx).hyperlink_id == hyperlink_id) return true;
        return false;
    };
    bool visible_affected = false;
    for (const auto& row : visible_rows_) visible_affected |= affected(row);
    // Entries are interned and shared by visible cells; updating the entry in
    // place updates every reference without allocating or changing IDs.
    cell_extras_.clear_hyperlink(hyperlink_id);
    if (visible_affected)
        for (auto& row : visible_rows_) row.mark_all_dirty(cols_);
}

void ScreenGrid::clear_row(Row& row, const Cell& empty_cell) {
    // A row recycled by a scroll must not keep a stale wide allocation (for
    // example after a shrink followed by a delete-lines operation).
    if (row.cells.size() != static_cast<size_t>(cols_)) {
        if (row.cells.capacity() > 2 * static_cast<size_t>(cols_))
            std::vector<Cell>(cols_, empty_cell).swap(row.cells);
        else row.cells.resize(cols_);
    }
    fill_cells(row.cells.data(), row.cells.size(), empty_cell);
    row.wrapped = false;
    row.mark_all_dirty(cols_);
}

void ScreenGrid::drop_scrolls() {
    for (const auto& op : scroll_ops_) {
        for (int y = std::max(0, op.top); y <= std::min(rows_ - 1, op.bottom); ++y)
            visible_rows_[y].mark_all_dirty(cols_);
    }
    scroll_ops_.clear();
}

void ScreenGrid::record_scroll(int top, int bottom, int rows) {
    const int region = bottom - top + 1;
    if (rows == 0 || region <= 0) return;

    auto repaint_region = [&](int r_top, int r_bottom) {
        for (int y = std::max(0, r_top); y <= std::min(rows_ - 1, r_bottom); ++y)
            visible_rows_[y].mark_all_dirty(cols_);
    };

    if (std::abs(rows) >= region) {
        // The whole region was replaced; no pixel shift can survive.
        scroll_ops_.clear();
        repaint_region(top, bottom);
        return;
    }

    if (!scroll_ops_.empty()) {
        ScrollOp& last = scroll_ops_.back();
        const bool same_direction = (last.rows > 0) == (rows > 0);
        if (last.top == top && last.bottom == bottom && same_direction) {
            const int merged = last.rows + rows;
            if (std::abs(merged) >= region) {
                // The merged shift replaces the region: drop every queued shift
                // and repaint all of their regions (correct, just slower).
                drop_scrolls();
                repaint_region(top, bottom);
            } else {
                last.rows = merged;
            }
            return;
        }
    }

    if (scroll_ops_.size() >= max_scroll_ops_) {
        drop_scrolls();
        repaint_region(top, bottom);
        return;
    }
    scroll_ops_.push_back(ScrollOp{top, bottom, rows});
}

void ScreenGrid::scroll_up(int top, int bottom, int count, const Cell& empty_cell) {
    if (top < 0 || bottom >= rows_ || top > bottom || count <= 0) return;
    count = std::min(count, bottom - top + 1);

    // Rotate ownership, not cell data. The rows that leave the margin are
    // wiped below, dropping their side-table references without a history copy.
    auto first = visible_rows_.begin() + top;
    auto last = visible_rows_.begin() + bottom + 1;
    std::rotate(first, first + count, last);
    for (int y = bottom - count + 1; y <= bottom; ++y) {
        clear_row(visible_rows_[y], empty_cell);
    }
    record_scroll(top, bottom, count);
}

void ScreenGrid::scroll_down(int top, int bottom, int count, const Cell& empty_cell) {
    if (top < 0 || bottom >= rows_ || top > bottom || count <= 0) return;
    count = std::min(count, bottom - top + 1);
    auto first = visible_rows_.begin() + top;
    auto last = visible_rows_.begin() + bottom + 1;
    std::rotate(first, last - count, last);
    for (int y = top; y < top + count; ++y) {
        clear_row(visible_rows_[y], empty_cell);
    }
    record_scroll(top, bottom, -count);
}

void ScreenGrid::erase_in_line(int x, int y, int mode, const Cell& empty_cell) {
    if (y < 0 || y >= rows_) return;
    x = std::clamp(x, 0, cols_ - 1);
    auto& r = visible_rows_[y];

    int start = 0;
    int end = 0;

    switch (mode) {
        case 0: // Cursor to end of line
            start = x;
            end = cols_ - 1;
            break;
        case 1: // Start of line to cursor
            start = 0;
            end = x;
            break;
        case 2: // Entire line
            start = 0;
            end = cols_ - 1;
            break;
        default:
            return;
    }

    fill_cells(r.cells.data() + start, static_cast<size_t>(end - start + 1), empty_cell);
    r.mark_dirty_range(start, end);
    if (mode == 2 || (mode == 0 && x == 0)) {
        r.wrapped = false;
    }
}

void ScreenGrid::erase_in_display(int x, int y, int mode, const Cell& empty_cell) {
    if (y < 0 || y >= rows_) return;
    x = std::clamp(x, 0, cols_ - 1);

    switch (mode) {
        case 0: // Cursor to end of screen
            erase_in_line(x, y, 0, empty_cell);
            for (int r = y + 1; r < rows_; ++r) {
                erase_in_line(0, r, 2, empty_cell);
            }
            break;
        case 1: // Start of screen to cursor
            for (int r = 0; r < y; ++r) {
                erase_in_line(0, r, 2, empty_cell);
            }
            erase_in_line(x, y, 1, empty_cell);
            break;
        case 2: // Entire screen
            for (int r = 0; r < rows_; ++r) {
                erase_in_line(0, r, 2, empty_cell);
            }
            break;
        case 3: // DECSED 3: no scrollback to erase; tmux owns history.
            break;
        default:
            break;
    }
}

void ScreenGrid::insert_lines(int y, int bottom, int count, const Cell& empty_cell) {
    scroll_down(y, bottom, count, empty_cell);
}

void ScreenGrid::delete_lines(int y, int bottom, int count, const Cell& empty_cell) {
    scroll_up(y, bottom, count, empty_cell);
}

void ScreenGrid::insert_cells(int x, int y, int count, const Cell& empty_cell) {
    if (y < 0 || y >= rows_ || x < 0 || x >= cols_ || count <= 0) return;
    count = std::min(count, cols_ - x);
    auto& r = visible_rows_[y];

    for (int i = cols_ - 1; i >= x + count; --i) {
        r.cells[i] = r.cells[i - count];
        r.mark_dirty(i);
    }
    for (int i = x; i < x + count; ++i) {
        r.cells[i] = empty_cell;
        r.mark_dirty(i);
    }
}

void ScreenGrid::delete_cells(int x, int y, int count, const Cell& empty_cell) {
    if (y < 0 || y >= rows_ || x < 0 || x >= cols_ || count <= 0) return;
    count = std::min(count, cols_ - x);
    auto& r = visible_rows_[y];

    for (int i = x; i <= cols_ - 1 - count; ++i) {
        r.cells[i] = r.cells[i + count];
        r.mark_dirty(i);
    }
    for (int i = cols_ - count; i < cols_; ++i) {
        r.cells[i] = empty_cell;
        r.mark_dirty(i);
    }
}

void ScreenGrid::mark_all_dirty() noexcept {
    // A full repaint supersedes any pending pixel shift.
    scroll_ops_.clear();
    for (auto& row : visible_rows_) {
        row.mark_all_dirty(cols_);
    }
}

void ScreenGrid::reset_damage() noexcept {
    scroll_ops_.clear();
    for (auto& row : visible_rows_) {
        row.reset_damage();
    }
}

uint32_t ScreenGrid::store_combining_codepoints(std::span<const char32_t> cps) {
    if (combining_store_.needs_collection()) {
        // A caller may pass a span borrowed from this very store. Preserve its
        // bytes before sweeping, even if no grid cell currently references it.
        const std::vector<char32_t> pending(cps.begin(), cps.end());
        collect_combining_garbage();
        return combining_store_.intern(pending);
    }
    return combining_store_.intern(cps);
}

uint32_t ScreenGrid::append_combining_codepoint(uint32_t prefix, char32_t cp) {
    if (combining_store_.needs_collection()) collect_combining_garbage(prefix);
    return combining_store_.append(prefix, cp);
}

std::span<const char32_t> ScreenGrid::get_combining_codepoints(uint32_t idx) const noexcept {
    return combining_store_.get(idx);
}

void ScreenGrid::collect_combining_garbage(uint32_t preserve) {
    cell_extras_.begin_collection();
    combining_store_.begin_collection();
    combining_store_.mark(preserve);

    const auto mark_row = [this](const Row& row) {
        for (const auto& cell : row.cells) cell_extras_.mark(cell.extra_idx);
    };
    for (const auto& row : visible_rows_) mark_row(row);
    cell_extras_.mark_combining(combining_store_);
    cell_extras_.sweep();
    combining_store_.sweep();
}

} // namespace gritty::core
