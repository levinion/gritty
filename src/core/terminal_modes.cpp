#include "core/terminal_engine.hpp"
#include <algorithm>

namespace gritty::core {

void TerminalEngine::designate_charset(unsigned int index, bool graphics) noexcept {
    if (index < graphic_charsets_.size()) graphic_charsets_[index] = graphics;
}

void TerminalEngine::select_charset(unsigned int index) noexcept {
    if (index < graphic_charsets_.size()) selected_charset_ = static_cast<uint8_t>(index);
}

void TerminalEngine::repeat_last_char(int count) {
    if (last_printed_ == 0) return;
    // Like foot's REP implementation, bound malformed/huge requests to the grid.
    count = static_cast<int>(std::min<int64_t>(count, static_cast<int64_t>(cols_) * rows_));
    const char32_t cp = last_printed_;
    while (count-- > 0) print_char(cp);
}

void TerminalEngine::scroll_up(int count) {
    Cell blank;
    blank.bg = current_bg_;
    active_grid_->scroll_up(margin_top_, margin_bottom_, count, blank);
}

void TerminalEngine::scroll_down(int count) {
    Cell blank;
    blank.bg = current_bg_;
    active_grid_->scroll_down(margin_top_, margin_bottom_, count, blank);
}

void TerminalEngine::reset() {
    set_alt_screen(false, 47);
    primary_grid_.erase_in_display(0, 0, 2);
    primary_grid_.erase_in_display(0, 0, 3);
    alt_grid_.erase_in_display(0, 0, 2);
    cursor_ = CursorState{};
    saved_cursor_primary_ = saved_cursor_alt_ = CursorState{};
    graphic_charsets_.fill(false);
    selected_charset_ = 0;
    wrap_next_ = false;
    last_printed_ = 0;
    margin_top_ = 0;
    margin_bottom_ = rows_ - 1;
    auto_wrap_ = true;
    origin_mode_ = bracketed_paste_ = insert_mode_ = cursor_key_mode_ = synchronized_updates_ = false;
    modify_other_keys_ = 0;
    mouse_tracking_ = MouseTrackingMode::None;
    mouse_format_ = MouseFormat::Normal;
    for (auto& stack : kitty_keyboard_stacks_) stack.clear();
    current_hyperlink_id_ = 0;
    sgr_reset();
    reset_palette();
    init_tabs();
}

void TerminalEngine::push_kitty_keyboard_flags(uint32_t flags) {
    auto& stack = kitty_keyboard_stacks_[is_alt_screen_ ? 1 : 0];
    if (stack.size() == 64) stack.erase(stack.begin());
    stack.push_back(flags & 31);
}

void TerminalEngine::pop_kitty_keyboard_flags(size_t count) {
    auto& stack = kitty_keyboard_stacks_[is_alt_screen_ ? 1 : 0];
    while (count-- > 0 && !stack.empty()) {
        stack.pop_back();
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
    auto& stack = kitty_keyboard_stacks_[is_alt_screen_ ? 1 : 0];
    if (stack.empty()) stack.push_back(new_val & 31);
    else stack.back() = new_val & 31;
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

void TerminalEngine::init_tabs() {
    tabs_.assign(cols_, false);
    for (int c = 8; c < cols_; c += 8) {
        tabs_[c] = true;
    }
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

} // namespace gritty::core
