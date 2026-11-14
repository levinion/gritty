#include "core/terminal_engine.hpp"
#include <utf8proc.h>
#include <algorithm>

namespace gritty::core {

namespace {

constexpr std::array<uint32_t, 16> kDefaultAnsiColors = {
    0x000000, // 0: Black
    0xcc0403, // 1: Red
    0x19cb00, // 2: Green
    0xcecb00, // 3: Yellow
    0x0d73cc, // 4: Blue
    0xcb1ed1, // 5: Magenta
    0x0dcdcd, // 6: Cyan
    0xdddddd, // 7: White
    0x767676, // 8: Bright Black
    0xf2201f, // 9: Bright Red
    0x23fd00, // 10: Bright Green
    0xfffd00, // 11: Bright Yellow
    0x1a8fff, // 12: Bright Blue
    0xfd28ff, // 13: Bright Magenta
    0x14ffff, // 14: Bright Cyan
    0xffffff  // 15: Bright White
};

} // namespace

TerminalEngine::TerminalEngine(int cols, int rows, size_t scrollback_capacity)
    : cols_(cols),
      rows_(rows),
      primary_grid_(cols, rows, scrollback_capacity),
      alt_grid_(cols, rows, 0), // alt screen has 0 scrollback
      active_grid_(&primary_grid_),
      margin_top_(0),
      margin_bottom_(rows - 1) {
    reset_palette();
    init_tabs();
    for (auto& stack : kitty_keyboard_stacks_) stack.reserve(64);
}

void TerminalEngine::reset_palette() noexcept {
    // 0..15 ANSI colors
    for (size_t i = 0; i < 16; ++i) {
        palette_[i] = kDefaultAnsiColors[i];
    }

    // 16..231: 6x6x6 color cube
    constexpr uint8_t steps[6] = {0, 95, 135, 175, 215, 255};
    size_t idx = 16;
    for (uint8_t r : steps) {
        for (uint8_t g : steps) {
            for (uint8_t b : steps) {
                palette_[idx++] = (static_cast<uint32_t>(r) << 16) |
                                  (static_cast<uint32_t>(g) << 8) | b;
            }
        }
    }

    // 232..255: 24 grayscale levels
    for (size_t i = 0; i < 24; ++i) {
        uint8_t level = static_cast<uint8_t>(8 + i * 10);
        palette_[idx++] = (static_cast<uint32_t>(level) << 16) |
                          (static_cast<uint32_t>(level) << 8) | level;
    }
}

void TerminalEngine::resize(int cols, int rows) {
    if (cols <= 0 || rows <= 0) return;
    cols_ = cols;
    rows_ = rows;

    primary_grid_.resize(cols, rows);
    alt_grid_.resize(cols, rows);

    margin_top_ = 0;
    margin_bottom_ = rows - 1;

    cursor_.col = std::clamp(cursor_.col, 0, cols_ - 1);
    cursor_.row = std::clamp(cursor_.row, 0, rows_ - 1);
    wrap_next_ = false;

    init_tabs();
}

void TerminalEngine::set_alt_screen(bool enable, int mode) {
    if (enable == is_alt_screen_) return;
    // foot csi.c: 47/1047 switch grids; only 1049 saves/restores the cursor.
    if (enable && mode == 1049) save_cursor();
    is_alt_screen_ = enable;

    if (is_alt_screen_) {
        active_grid_ = &alt_grid_;
        alt_grid_.erase_in_display(0, 0, 2);
    } else {
        active_grid_ = &primary_grid_;
    }
    wrap_next_ = false;
    if (!enable && mode == 1049) restore_cursor();
    active_grid_->mark_all_dirty();
}

void TerminalEngine::set_cursor_pos(int col, int row) {
    wrap_next_ = false;
    if (origin_mode_) {
        row = margin_top_ + row;
        cursor_.row = std::clamp(row, margin_top_, margin_bottom_);
    } else {
        cursor_.row = std::clamp(row, 0, rows_ - 1);
    }
    cursor_.col = std::clamp(col, 0, cols_ - 1);
}

void TerminalEngine::move_cursor_up(int count) {
    wrap_next_ = false;
    const int min_row = origin_mode_ ? margin_top_ : 0;
    cursor_.row = std::max(min_row, cursor_.row - std::min(count, rows_));
}

void TerminalEngine::move_cursor_down(int count) {
    wrap_next_ = false;
    const int max_row = origin_mode_ ? margin_bottom_ : rows_ - 1;
    cursor_.row = std::min(max_row, cursor_.row + std::min(count, rows_));
}

void TerminalEngine::move_cursor_forward(int count) {
    wrap_next_ = false;
    cursor_.col = std::min(cols_ - 1, cursor_.col + std::min(count, cols_));
}

void TerminalEngine::move_cursor_backward(int count) {
    wrap_next_ = false;
    cursor_.col = std::max(0, cursor_.col - std::min(count, cols_));
}

void TerminalEngine::set_cursor_col(int col) {
    wrap_next_ = false;
    cursor_.col = std::clamp(col, 0, cols_ - 1);
}

void TerminalEngine::set_cursor_row(int row) {
    wrap_next_ = false;
    if (origin_mode_) {
        row = margin_top_ + row;
        cursor_.row = std::clamp(row, margin_top_, margin_bottom_);
    } else {
        cursor_.row = std::clamp(row, 0, rows_ - 1);
    }
}

void TerminalEngine::carriage_return() {
    cursor_.col = 0;
    wrap_next_ = false;
}

void TerminalEngine::line_feed() {
    wrap_next_ = false;
    if (cursor_.row == margin_bottom_) {
        scroll_up(1);
    } else if (cursor_.row < rows_ - 1) {
        cursor_.row++;
    }
}

void TerminalEngine::reverse_index() {
    wrap_next_ = false;
    if (cursor_.row == margin_top_) {
        scroll_down(1);
    } else if (cursor_.row > 0) {
        cursor_.row--;
    }
}

void TerminalEngine::save_cursor() {
    auto& target = is_alt_screen_ ? saved_cursor_alt_ : saved_cursor_primary_;
    target = cursor_;
    target.fg = current_fg_;
    target.bg = current_bg_;
    target.underline_color = current_underline_color_;
    target.attrs = current_attrs_;
    target.origin_mode = origin_mode_;
    target.auto_wrap = auto_wrap_;
    target.pending_wrap = wrap_next_;
    target.graphic_charsets = graphic_charsets_;
    target.selected_charset = selected_charset_;
}

void TerminalEngine::restore_cursor() {
    const auto& src = is_alt_screen_ ? saved_cursor_alt_ : saved_cursor_primary_;
    // Cursor visibility/shape are not part of DECSC's saved rendition state.
    cursor_.col = src.col;
    cursor_.row = src.row;
    current_fg_ = src.fg;
    current_bg_ = src.bg;
    current_underline_color_ = src.underline_color;
    current_attrs_ = src.attrs;
    origin_mode_ = src.origin_mode;
    auto_wrap_ = src.auto_wrap;
    wrap_next_ = src.pending_wrap;
    graphic_charsets_ = src.graphic_charsets;
    selected_charset_ = src.selected_charset;
    cursor_.col = std::clamp(cursor_.col, 0, cols_ - 1);
    cursor_.row = std::clamp(cursor_.row, 0, rows_ - 1);
}

void TerminalEngine::print_char(char32_t cp) {
    // DEC special graphics mapping, also used by tmux's ACS pane borders.
    if (graphic_charsets_[selected_charset_] && cp >= 0x60 && cp <= 0x7e) {
        constexpr std::u32string_view graphics = U"◆▒␉␌␍␊°±␤␋┘┐┌└┼⎺⎻─⎼⎽├┤┴┬│≤≥π≠£·";
        cp = graphics[cp - 0x60];
    }
    int width = utf8proc_charwidth(cp);
    if (width <= 0) {
        // Combining character: append to previous cell
        if (width == 0 && cursor_.col > 0) {
            int target_col = cursor_.col - 1;
            auto& prev_cell = active_grid_->cell(target_col, cursor_.row);
            if (prev_cell.attrs.width == 0 && target_col > 0) {
                target_col--; // If this was continuation cell, find leading cell
            }
            std::span<const char32_t> existing = active_grid_->get_combining_codepoints(prev_cell.combining_idx);
            std::vector<char32_t> combined(existing.begin(), existing.end());
            combined.push_back(cp);
            prev_cell.combining_idx = active_grid_->store_combining_codepoints(combined);
            active_grid_->row(cursor_.row).mark_dirty(target_col);
        }
        return;
    }

    if (wrap_next_) {
        if (auto_wrap_) {
            active_grid_->row(cursor_.row).wrapped = true;
            cursor_.col = 0;
            line_feed();
        } else {
            cursor_.col = cols_ - 1;
        }
        wrap_next_ = false;
    }

    if (width == 2 && cursor_.col == cols_ - 1) {
        // Wide char cannot fit on final col: wrap to next line
        if (auto_wrap_) {
            active_grid_->row(cursor_.row).wrapped = true;
            cursor_.col = 0;
            line_feed();
        }
    }

    if (insert_mode_) {
        Cell empty;
        empty.bg = current_bg_;
        active_grid_->insert_cells(cursor_.col, cursor_.row, width, empty);
    }
    last_printed_ = cp;
    Cell c;
    c.codepoint = cp;
    c.fg = current_fg_;
    c.bg = current_bg_;
    c.underline_color = current_underline_color_;
    c.attrs = current_attrs_;
    c.attrs.width = static_cast<uint16_t>(width);
    c.hyperlink_id = current_hyperlink_id_;

    active_grid_->set_cell(cursor_.col, cursor_.row, c);

    if (width == 2 && cursor_.col + 1 < cols_) {
        Cell cont;
        cont.codepoint = 0;
        cont.fg = current_fg_;
        cont.bg = current_bg_;
        cont.attrs = current_attrs_;
        cont.attrs.width = 0;
        active_grid_->set_cell(cursor_.col + 1, cursor_.row, cont);
    }

    cursor_.col += width;
    if (cursor_.col >= cols_) {
        cursor_.col = cols_ - 1;
        if (auto_wrap_) {
            wrap_next_ = true;
        }
    }
}

void TerminalEngine::print_utf8(std::string_view utf8_str) {
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(utf8_str.data());
    size_t len = utf8_str.size();

    while (len > 0) {
        utf8proc_int32_t cp;
        utf8proc_ssize_t res = utf8proc_iterate(ptr, len, &cp);
        if (res < 0) {
            // Invalid UTF-8 sequence, skip 1 byte
            ptr++;
            len--;
            continue;
        }
        print_char(static_cast<char32_t>(cp));
        ptr += res;
        len -= res;
    }
}

void TerminalEngine::erase_in_display(int mode) {
    if (mode != 3) wrap_next_ = false;
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    active_grid_->erase_in_display(cursor_.col, cursor_.row, mode, empty_cell);
}

void TerminalEngine::erase_in_line(int mode) {
    wrap_next_ = false;
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    active_grid_->erase_in_line(cursor_.col, cursor_.row, mode, empty_cell);
}

void TerminalEngine::insert_lines(int count) {
    if (cursor_.row < margin_top_ || cursor_.row > margin_bottom_) return;
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    active_grid_->insert_lines(cursor_.row, margin_bottom_, count, empty_cell);
}

void TerminalEngine::delete_lines(int count) {
    if (cursor_.row < margin_top_ || cursor_.row > margin_bottom_) return;
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    active_grid_->delete_lines(cursor_.row, margin_bottom_, count, empty_cell);
}

void TerminalEngine::insert_chars(int count) {
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    active_grid_->insert_cells(cursor_.col, cursor_.row, count, empty_cell);
}

void TerminalEngine::delete_chars(int count) {
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    active_grid_->delete_cells(cursor_.col, cursor_.row, count, empty_cell);
}

void TerminalEngine::erase_chars(int count) {
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    for (int i = 0; i < count && cursor_.col + i < cols_; ++i) {
        active_grid_->set_cell(cursor_.col + i, cursor_.row, empty_cell);
    }
}

void TerminalEngine::set_margins(int top, int bottom) {
    top = std::max(0, top);
    bottom = std::min(rows_ - 1, bottom);
    if (top < bottom) {
        margin_top_ = top;
        margin_bottom_ = bottom;
        set_cursor_pos(0, 0);
    }
    // Invalid margins are ignored, including their implied cursor movement.
}

void TerminalEngine::set_origin_mode(bool enable) noexcept {
    origin_mode_ = enable;
    set_cursor_pos(0, 0);
}

} // namespace gritty::core
