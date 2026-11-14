#include "core/screen_grid.hpp"

namespace gritty::core {

ScreenGrid::ScreenGrid(int cols, int rows, size_t scrollback_capacity)
    : cols_(cols), rows_(rows), scrollback_(scrollback_capacity) {
    visible_rows_.resize(rows_);
    for (auto& row : visible_rows_) {
        row.cells.resize(cols_);
        row.mark_all_dirty(cols_);
    }
}

void ScreenGrid::resize(int new_cols, int new_rows) {
    if (new_cols <= 0 || new_rows <= 0) return;

    if (new_rows < rows_) {
        // Pushing excess top rows into scrollback
        int excess = rows_ - new_rows;
        if (scrollback_.capacity() > 0) {
            for (int i = 0; i < excess; ++i) {
                scrollback_.push_back(std::move(visible_rows_[i]));
            }
        }
        visible_rows_.erase(visible_rows_.begin(), visible_rows_.begin() + excess);
    } else if (new_rows > rows_) {
        int add = new_rows - rows_;
        // Pull from scrollback if available
        while (add > 0 && !scrollback_.empty()) {
            Row r = std::move(scrollback_.back());
            scrollback_.pop_back();
            r.cells.resize(new_cols);
            r.mark_all_dirty(new_cols);
            visible_rows_.insert(visible_rows_.begin(), std::move(r));
            --add;
        }
        while (add > 0) {
            Row r;
            r.cells.resize(new_cols);
            r.mark_all_dirty(new_cols);
            visible_rows_.emplace_back(std::move(r));
            --add;
        }
    }

    cols_ = new_cols;
    rows_ = new_rows;

    for (auto& row : visible_rows_) {
        row.cells.resize(cols_);
        row.mark_all_dirty(cols_);
    }

    if (view_offset_ > scrollback_.size()) {
        view_offset_ = scrollback_.size();
    }
}

void ScreenGrid::set_scrollback_capacity(size_t capacity) {
    scrollback_.set_capacity(capacity);
    if (view_offset_ > scrollback_.size()) {
        view_offset_ = scrollback_.size();
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

void ScreenGrid::scroll_up(int top, int bottom, int count, const Cell& empty_cell) {
    if (top < 0 || bottom >= rows_ || top > bottom || count <= 0) return;
    count = std::min(count, bottom - top + 1);

    if (top == 0 && bottom == rows_ - 1) {
        // Full screen scroll: push top lines to scrollback
        if (scrollback_.capacity() > 0) {
            for (int i = 0; i < count; ++i) {
                scrollback_.push_back(std::move(visible_rows_[i]));
            }
        }
        for (int i = 0; i <= bottom - count; ++i) {
            visible_rows_[i] = std::move(visible_rows_[i + count]);
            visible_rows_[i].mark_all_dirty(cols_);
        }
        for (int i = bottom - count + 1; i <= bottom; ++i) {
            visible_rows_[i].cells.assign(cols_, empty_cell);
            visible_rows_[i].wrapped = false;
            visible_rows_[i].mark_all_dirty(cols_);
        }
    } else {
        // Margined scroll: lines outside margins do not move, no scrollback push
        for (int i = top; i <= bottom - count; ++i) {
            visible_rows_[i] = std::move(visible_rows_[i + count]);
            visible_rows_[i].mark_all_dirty(cols_);
        }
        for (int i = bottom - count + 1; i <= bottom; ++i) {
            visible_rows_[i].cells.assign(cols_, empty_cell);
            visible_rows_[i].wrapped = false;
            visible_rows_[i].mark_all_dirty(cols_);
        }
    }
}

void ScreenGrid::scroll_down(int top, int bottom, int count, const Cell& empty_cell) {
    if (top < 0 || bottom >= rows_ || top > bottom || count <= 0) return;
    count = std::min(count, bottom - top + 1);

    for (int i = bottom; i >= top + count; --i) {
        visible_rows_[i] = std::move(visible_rows_[i - count]);
        visible_rows_[i].mark_all_dirty(cols_);
    }
    for (int i = top; i < top + count; ++i) {
        visible_rows_[i].cells.assign(cols_, empty_cell);
        visible_rows_[i].wrapped = false;
        visible_rows_[i].mark_all_dirty(cols_);
    }
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

    for (int col = start; col <= end; ++col) {
        r.cells[col] = empty_cell;
        r.mark_dirty(col);
    }
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
        case 3: // Entire screen and scrollback
            for (int r = 0; r < rows_; ++r) {
                erase_in_line(0, r, 2, empty_cell);
            }
            scrollback_.clear();
            view_offset_ = 0;
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
    for (auto& row : visible_rows_) {
        row.mark_all_dirty(cols_);
    }
}

void ScreenGrid::reset_damage() noexcept {
    for (auto& row : visible_rows_) {
        row.reset_damage();
    }
}

void ScreenGrid::scroll_view(int delta) noexcept {
    int new_offset = static_cast<int>(view_offset_) + delta;
    if (new_offset < 0) {
        view_offset_ = 0;
    } else if (static_cast<size_t>(new_offset) > scrollback_.size()) {
        view_offset_ = scrollback_.size();
    } else {
        view_offset_ = static_cast<size_t>(new_offset);
    }
}

uint32_t ScreenGrid::store_combining_codepoints(std::span<const char32_t> cps) {
    if (cps.empty()) return 0;
    combining_table_.emplace_back(cps.begin(), cps.end());
    return static_cast<uint32_t>(combining_table_.size()); // 1-based index
}

std::span<const char32_t> ScreenGrid::get_combining_codepoints(uint32_t idx) const noexcept {
    if (idx == 0 || idx > combining_table_.size()) {
        return {};
    }
    return combining_table_[idx - 1];
}

} // namespace gritty::core
