#include "core/terminal_engine.hpp"
#include <utf8proc.h>
#include <algorithm>

namespace squirt::core {

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

void TerminalEngine::init_tabs() {
    tabs_.assign(cols_, false);
    for (int c = 8; c < cols_; c += 8) {
        tabs_[c] = true;
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

void TerminalEngine::set_alt_screen(bool enable) {
    if (enable == is_alt_screen_) return;
    is_alt_screen_ = enable;

    if (is_alt_screen_) {
        save_cursor();
        active_grid_ = &alt_grid_;
        alt_grid_.erase_in_display(0, 0, 2);
        cursor_.col = 0;
        cursor_.row = 0;
        wrap_next_ = false;
    } else {
        active_grid_ = &primary_grid_;
        restore_cursor();
    }
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
    int min_row = (origin_mode_ || cursor_.row >= margin_top_) ? margin_top_ : 0;
    cursor_.row = std::max(min_row, cursor_.row - count);
}

void TerminalEngine::move_cursor_down(int count) {
    wrap_next_ = false;
    int max_row = (origin_mode_ || cursor_.row <= margin_bottom_) ? margin_bottom_ : rows_ - 1;
    cursor_.row = std::min(max_row, cursor_.row + count);
}

void TerminalEngine::move_cursor_forward(int count) {
    wrap_next_ = false;
    cursor_.col = std::min(cols_ - 1, cursor_.col + count);
}

void TerminalEngine::move_cursor_backward(int count) {
    wrap_next_ = false;
    cursor_.col = std::max(0, cursor_.col - count);
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
        active_grid_->scroll_up(margin_top_, margin_bottom_, 1);
    } else if (cursor_.row < rows_ - 1) {
        cursor_.row++;
    }
}

void TerminalEngine::reverse_index() {
    wrap_next_ = false;
    if (cursor_.row == margin_top_) {
        active_grid_->scroll_down(margin_top_, margin_bottom_, 1);
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
}

void TerminalEngine::restore_cursor() {
    const auto& src = is_alt_screen_ ? saved_cursor_alt_ : saved_cursor_primary_;
    cursor_ = src;
    current_fg_ = src.fg;
    current_bg_ = src.bg;
    current_underline_color_ = src.underline_color;
    current_attrs_ = src.attrs;
    origin_mode_ = src.origin_mode;
    auto_wrap_ = src.auto_wrap;
    wrap_next_ = false;
    cursor_.col = std::clamp(cursor_.col, 0, cols_ - 1);
    cursor_.row = std::clamp(cursor_.row, 0, rows_ - 1);
}

void TerminalEngine::print_char(char32_t cp) {
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
    Cell empty_cell;
    empty_cell.bg = current_bg_;
    active_grid_->erase_in_display(cursor_.col, cursor_.row, mode, empty_cell);
}

void TerminalEngine::erase_in_line(int mode) {
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
    } else {
        margin_top_ = 0;
        margin_bottom_ = rows_ - 1;
    }
    set_cursor_pos(0, 0);
}

void TerminalEngine::set_origin_mode(bool enable) noexcept {
    origin_mode_ = enable;
    set_cursor_pos(0, 0);
}

void TerminalEngine::tab_forward(int count) {
    while (count-- > 0) {
        int next_tab = cols_ - 1;
        for (int c = cursor_.col + 1; c < cols_; ++c) {
            if (tabs_[c]) {
                next_tab = c;
                break;
            }
        }
        cursor_.col = next_tab;
    }
    wrap_next_ = false;
}

void TerminalEngine::tab_backward(int count) {
    while (count-- > 0) {
        int prev_tab = 0;
        for (int c = cursor_.col - 1; c >= 0; --c) {
            if (tabs_[c]) {
                prev_tab = c;
                break;
            }
        }
        cursor_.col = prev_tab;
    }
    wrap_next_ = false;
}

void TerminalEngine::set_tab_stop() {
    if (cursor_.col >= 0 && cursor_.col < cols_) {
        tabs_[cursor_.col] = true;
    }
}

void TerminalEngine::clear_tab_stop(int mode) {
    if (mode == 0) {
        if (cursor_.col >= 0 && cursor_.col < cols_) {
            tabs_[cursor_.col] = false;
        }
    } else if (mode == 3) {
        tabs_.assign(cols_, false);
    }
}

void TerminalEngine::sgr_reset() {
    current_fg_ = Color::make_default();
    current_bg_ = Color::make_default();
    current_underline_color_ = Color::make_default();
    current_attrs_ = CellAttributes{};
}

void TerminalEngine::push_kitty_keyboard_flags(uint32_t flags) {
    kitty_keyboard_stack_.push_back(flags);
}

void TerminalEngine::pop_kitty_keyboard_flags(size_t count) {
    while (count-- > 0 && !kitty_keyboard_stack_.empty()) {
        kitty_keyboard_stack_.pop_back();
    }
}

void TerminalEngine::set_kitty_keyboard_flags(uint32_t flags, int mode) {
    uint32_t current = kitty_keyboard_flags();
    uint32_t new_val = current;
    switch (mode) {
        case 1: new_val = flags; break;               // replace
        case 2: new_val = current | flags; break;      // union
        case 3: new_val = current & ~flags; break;     // difference
        default: break;
    }
    if (kitty_keyboard_stack_.empty()) {
        kitty_keyboard_stack_.push_back(new_val);
    } else {
        kitty_keyboard_stack_.back() = new_val;
    }
}

uint16_t TerminalEngine::get_or_create_hyperlink_id(std::string_view url) {
    if (url.empty()) return 0;
    for (size_t i = 0; i < hyperlinks_.size(); ++i) {
        if (hyperlinks_[i] == url) {
            return static_cast<uint16_t>(i + 1);
        }
    }
    hyperlinks_.emplace_back(url);
    return static_cast<uint16_t>(hyperlinks_.size());
}

std::string_view TerminalEngine::get_hyperlink_url(uint16_t id) const noexcept {
    if (id == 0 || id > hyperlinks_.size()) return {};
    return hyperlinks_[id - 1];
}

void TerminalEngine::set_current_hyperlink(std::string_view url) {
    current_hyperlink_id_ = get_or_create_hyperlink_id(url);
}

} // namespace squirt::core
