#include "core/screen_grid.hpp"
#include <algorithm>
#include <cstring>
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace gritty::core {
namespace {

void fill_cells(Cell* dst, size_t count, const Cell& value) noexcept {
    if (count < 4) {
        for (size_t i = 0; i < count; ++i) dst[i] = value;
        return;
    }
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

[[nodiscard]] inline bool is_pure_blank(const Cell& c) noexcept {
    constexpr Cell kDefault{};
    return c.codepoint == U' ' && c.fg.is_default() && c.extra_idx == 0 &&
           cell_tail_word(c) == cell_tail_word(kDefault);
}

} // namespace


ScreenGrid::ScreenGrid(int cols, int rows)
    : cols_(cols), rows_(rows) {
    row_storage_.resize(rows_);
    row_order_.resize(rows_);
    for (int y = 0; y < rows_; ++y) {
        row_order_[y] = static_cast<uint16_t>(y);
        auto& r = row_storage_[y];
        r.cells.resize(cols_);
        r.clean_from = 0;
        r.clean_bg = Color::make_default();
        r.mark_all_dirty(cols_);
    }
}

void ScreenGrid::normalize_row_order() noexcept {
    if (top_row_ != 0) {
        std::rotate(row_order_.begin(), row_order_.begin() + top_row_, row_order_.end());
        top_row_ = 0;
    }
}

void ScreenGrid::resize(int new_cols, int new_rows) {
    if (new_cols <= 0 || new_rows <= 0) return;
    scroll_ops_.clear();
    last_dropped_top_ = -1;
    last_dropped_bottom_ = -1;
    normalize_row_order();

    const int old_rows = rows_;
    std::vector<Row> ordered;
    ordered.reserve(static_cast<size_t>(new_rows));
    const int skip = new_rows < old_rows ? (old_rows - new_rows) : 0;
    for (int y = skip; y < old_rows; ++y) {
        ordered.emplace_back(std::move(row_storage_[row_order_[y]]));
    }
    for (int y = old_rows; y < new_rows; ++y) {
        Row r;
        r.cells.resize(new_cols);
        r.clean_from = 0;
        r.clean_bg = Color::make_default();
        r.mark_all_dirty(new_cols);
        ordered.emplace_back(std::move(r));
    }

    row_storage_ = std::move(ordered);
    row_order_.resize(static_cast<size_t>(new_rows));
    for (int y = 0; y < new_rows; ++y) row_order_[y] = static_cast<uint16_t>(y);
    top_row_ = 0;

    cols_ = new_cols;
    rows_ = new_rows;

    const int preserved = std::min(old_rows, new_rows);
    for (int y = 0; y < preserved; ++y) {
        auto& r = row_storage_[y];
        if (r.cells.size() != static_cast<size_t>(cols_)) {
            if (r.cells.capacity() > 2 * static_cast<size_t>(cols_))
                std::vector<Cell>(cols_, Cell{}).swap(r.cells);
            else r.cells.resize(cols_);
        }
        r.clean_from = cols_;
        r.mark_all_dirty(cols_);
    }
    if (skip > 0) collect_combining_garbage();
}

void ScreenGrid::repair_wide_span(int start_col, int end_col, int y) noexcept {
    if (y < 0 || y >= rows_ || start_col > end_col) return;
    start_col = std::clamp(start_col, 0, cols_ - 1);
    end_col = std::clamp(end_col, 0, cols_ - 1);
    auto& r = row(y);
    if (start_col > 0 && r.cells[start_col].attrs.width == 0 &&
        r.cells[start_col - 1].attrs.width == 2) {
        Cell blank;
        blank.bg = r.cells[start_col - 1].bg;
        r.cells[start_col - 1] = blank;
        r.mark_dirty(start_col - 1);
        r.note_written(start_col - 1);
    }
    if (end_col + 1 < cols_ && r.cells[end_col].attrs.width == 2 &&
        r.cells[end_col + 1].attrs.width == 0) {
        Cell blank;
        blank.bg = r.cells[end_col + 1].bg;
        r.cells[end_col + 1] = blank;
        r.mark_dirty(end_col + 1);
        r.note_written(end_col + 1);
    }
}

void ScreenGrid::set_cell(int x, int y, const Cell& c) noexcept {
    if (x < 0 || x >= cols_ || y < 0 || y >= rows_) return;
    auto& r = row(y);
    auto& target = r.cells[x];
    if (target.attrs.width == 2 && x + 1 < cols_ && r.cells[x + 1].attrs.width == 0 &&
        c.attrs.width != 2) {
        Cell blank;
        blank.bg = r.cells[x + 1].bg;
        r.cells[x + 1] = blank;
        r.mark_dirty(x + 1);
        r.note_written(x + 1);
    }
    if (target.attrs.width == 0 && c.attrs.width != 0 && x > 0 && r.cells[x - 1].attrs.width == 2) {
        Cell blank;
        blank.bg = r.cells[x - 1].bg;
        r.cells[x - 1] = blank;
        r.mark_dirty(x - 1);
        r.note_written(x - 1);
    }
    if (target != c) {
        target = c;
        r.mark_dirty(x);
        r.note_written(x);
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
    for (const auto& r : row_storage_) visible_affected |= affected(r);
    // Entries are interned and shared by visible cells; updating the entry in
    // place updates every reference without allocating or changing IDs.
    cell_extras_.clear_hyperlink(hyperlink_id);
    if (visible_affected)
        for (auto& r : row_storage_) r.mark_all_dirty(cols_);
}

void ScreenGrid::clear_row(Row& r, const Cell& empty_cell) {
    // A row recycled by a scroll must not keep a stale wide allocation (for
    // example after a shrink followed by a delete-lines operation).
    const bool blank = is_pure_blank(empty_cell);
    if (r.cells.size() != static_cast<size_t>(cols_)) {
        if (r.cells.capacity() > 2 * static_cast<size_t>(cols_))
            std::vector<Cell>(cols_, empty_cell).swap(r.cells);
        else {
            r.cells.resize(cols_);
            fill_cells(r.cells.data(), r.cells.size(), empty_cell);
        }
        r.clean_from = blank ? 0 : cols_;
        r.clean_bg = empty_cell.bg;
    } else if (blank && empty_cell.bg == r.clean_bg) {
        const int limit = std::min(r.clean_from, cols_);
        if (limit > 0) fill_cells(r.cells.data(), static_cast<size_t>(limit), empty_cell);
        r.clean_from = 0;
    } else {
        fill_cells(r.cells.data(), r.cells.size(), empty_cell);
        r.clean_from = blank ? 0 : cols_;
        r.clean_bg = empty_cell.bg;
    }
    r.wrapped = false;
    r.mark_all_dirty(cols_);
}

void ScreenGrid::drop_scrolls() {
    for (const auto& op : scroll_ops_) {
        for (int y = std::max(0, op.top); y <= std::min(rows_ - 1, op.bottom); ++y)
            row(y).mark_all_dirty(cols_);
    }
    scroll_ops_.clear();
}

void ScreenGrid::record_scroll(int top, int bottom, int rows) {
    const int region = bottom - top + 1;
    if (rows == 0 || region <= 0) return;
    if (scroll_ops_.empty() && top == last_dropped_top_ && bottom == last_dropped_bottom_) {
        return; // Every row in [top, bottom] is already marked completely dirty this frame.
    }

    auto repaint_region = [&](int r_top, int r_bottom) {
        for (int y = std::max(0, r_top); y <= std::min(rows_ - 1, r_bottom); ++y)
            row(y).mark_all_dirty(cols_);
    };

    if (std::abs(rows) >= region) {
        // The whole region was replaced; no pixel shift can survive.
        scroll_ops_.clear();
        repaint_region(top, bottom);
        last_dropped_top_ = top;
        last_dropped_bottom_ = bottom;
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
                last_dropped_top_ = top;
                last_dropped_bottom_ = bottom;
            } else {
                last.rows = merged;
            }
            return;
        }
    }

    if (scroll_ops_.size() >= max_scroll_ops_) {
        drop_scrolls();
        repaint_region(top, bottom);
        last_dropped_top_ = top;
        last_dropped_bottom_ = bottom;
        return;
    }
    last_dropped_top_ = -1;
    last_dropped_bottom_ = -1;
    scroll_ops_.push_back(ScrollOp{top, bottom, rows});
}

void ScreenGrid::scroll_up(int top, int bottom, int count, const Cell& empty_cell) {
    if (top < 0 || bottom >= rows_ || top > bottom || count <= 0) return;
    const int region = bottom - top + 1;
    count = std::min(count, region);

    if (top == 0 && bottom == rows_ - 1) {
        top_row_ += count;
        if (top_row_ >= rows_) top_row_ -= rows_;
    } else {
        normalize_row_order();
        if (count == 1 && region > 1) {
            const uint16_t saved = row_order_[top];
            std::memmove(&row_order_[top], &row_order_[top + 1],
                         static_cast<size_t>(region - 1) * sizeof(uint16_t));
            row_order_[bottom] = saved;
        } else if (count < region) {
            std::rotate(row_order_.begin() + top,
                        row_order_.begin() + top + count,
                        row_order_.begin() + bottom + 1);
        }
    }
    for (int y = bottom - count + 1; y <= bottom; ++y) {
        clear_row(row(y), empty_cell);
    }
    record_scroll(top, bottom, count);
}

void ScreenGrid::scroll_down(int top, int bottom, int count, const Cell& empty_cell) {
    if (top < 0 || bottom >= rows_ || top > bottom || count <= 0) return;
    const int region = bottom - top + 1;
    count = std::min(count, region);

    if (top == 0 && bottom == rows_ - 1) {
        top_row_ -= count;
        if (top_row_ < 0) top_row_ += rows_;
    } else {
        normalize_row_order();
        if (count == 1 && region > 1) {
            const uint16_t saved = row_order_[bottom];
            std::memmove(&row_order_[top + 1], &row_order_[top],
                         static_cast<size_t>(region - 1) * sizeof(uint16_t));
            row_order_[top] = saved;
        } else if (count < region) {
            std::rotate(row_order_.begin() + top,
                        row_order_.begin() + bottom + 1 - count,
                        row_order_.begin() + bottom + 1);
        }
    }
    for (int y = top; y < top + count; ++y) {
        clear_row(row(y), empty_cell);
    }
    record_scroll(top, bottom, -count);
}

void ScreenGrid::erase_in_line(int x, int y, int mode, const Cell& empty_cell) {
    if (y < 0 || y >= rows_) return;
    x = std::clamp(x, 0, cols_ - 1);
    auto& r = row(y);

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

    repair_wide_span(start, end, y);
    const bool blank = is_pure_blank(empty_cell);
    if (end == cols_ - 1 && blank && empty_cell.bg == r.clean_bg) {
        const int fill_end = std::min(cols_, r.clean_from);
        if (fill_end > start) {
            fill_cells(r.cells.data() + start, static_cast<size_t>(fill_end - start), empty_cell);
        }
        r.clean_from = std::min(r.clean_from, start);
    } else {
        fill_cells(r.cells.data() + start, static_cast<size_t>(end - start + 1), empty_cell);
        if (start == 0 && end == cols_ - 1 && blank) {
            r.clean_from = 0;
            r.clean_bg = empty_cell.bg;
        } else if (end == cols_ - 1) {
            r.clean_from = cols_;
        } else {
            r.note_written(end);
        }
    }
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
    auto& r = row(y);
    if (r.cells[x].attrs.width == 0) {
        if (x > 0 && r.cells[x - 1].attrs.width == 2) {
            Cell blank;
            blank.bg = r.cells[x - 1].bg;
            r.cells[x - 1] = blank;
            r.mark_dirty(x - 1);
        }
        Cell blank;
        blank.bg = r.cells[x].bg;
        r.cells[x] = blank;
        r.mark_dirty(x);
    }
    if (cols_ - 1 - count >= x && r.cells[cols_ - 1 - count].attrs.width == 2) {
        // Rightmost wide leader shifted into the final column loses its continuation.
        Cell blank;
        blank.bg = r.cells[cols_ - 1 - count].bg;
        r.cells[cols_ - 1 - count] = blank;
        r.mark_dirty(cols_ - 1 - count);
    }

    for (int i = cols_ - 1; i >= x + count; --i) {
        r.cells[i] = r.cells[i - count];
        r.mark_dirty(i);
    }
    for (int i = x; i < x + count; ++i) {
        r.cells[i] = empty_cell;
        r.mark_dirty(i);
    }
    r.clean_from = cols_;
}

void ScreenGrid::delete_cells(int x, int y, int count, const Cell& empty_cell) {
    if (y < 0 || y >= rows_ || x < 0 || x >= cols_ || count <= 0) return;
    count = std::min(count, cols_ - x);
    auto& r = row(y);
    repair_wide_span(x, x, y);
    if (x + count < cols_ && r.cells[x + count].attrs.width == 0) {
        Cell blank;
        blank.bg = r.cells[x + count].bg;
        r.cells[x + count] = blank;
        r.mark_dirty(x + count);
    }

    for (int i = x; i <= cols_ - 1 - count; ++i) {
        r.cells[i] = r.cells[i + count];
        r.mark_dirty(i);
    }
    for (int i = cols_ - count; i < cols_; ++i) {
        r.cells[i] = empty_cell;
        r.mark_dirty(i);
    }
    r.clean_from = cols_;
}

void ScreenGrid::mark_all_dirty() noexcept {
    // A full repaint supersedes any pending pixel shift.
    scroll_ops_.clear();
    last_dropped_top_ = -1;
    last_dropped_bottom_ = -1;
    for (auto& r : row_storage_) {
        r.mark_all_dirty(cols_);
    }
}

void ScreenGrid::reset_damage() noexcept {
    scroll_ops_.clear();
    last_dropped_top_ = -1;
    last_dropped_bottom_ = -1;
    for (auto& r : row_storage_) {
        r.reset_damage();
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

    const auto mark_row = [this](const Row& r) {
        for (const auto& cell : r.cells) cell_extras_.mark(cell.extra_idx);
    };
    for (const auto& r : row_storage_) mark_row(r);
    cell_extras_.mark_combining(combining_store_);
    cell_extras_.sweep();
    combining_store_.sweep();
}

} // namespace gritty::core
