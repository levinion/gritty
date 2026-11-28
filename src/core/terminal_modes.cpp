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
    reset_grapheme_state();
    Cell blank;
    blank.bg = current_bg_;
    active_grid_->scroll_up(margin_top_, margin_bottom_, count, blank);
    scroll_images(margin_top_, margin_bottom_, count);
}

void TerminalEngine::scroll_down(int count) {
    reset_grapheme_state();
    Cell blank;
    blank.bg = current_bg_;
    active_grid_->scroll_down(margin_top_, margin_bottom_, count, blank);
    scroll_images(margin_top_, margin_bottom_, -count);
}

void TerminalEngine::reset() {
    clear_images();
    kitty_graphics_.clear();
    set_alt_screen(false, 47);
    primary_grid_.erase_in_display(0, 0, 2);
    primary_grid_.collect_combining_garbage();
    alt_grid_.erase_in_display(0, 0, 2);
    alt_grid_.collect_combining_garbage();
    cursor_ = CursorState{};
    saved_cursor_primary_ = saved_cursor_alt_ = CursorState{};
    graphic_charsets_.fill(false);
    selected_charset_ = 0;
    wrap_next_ = false;
    reset_grapheme_state();
    last_printed_ = 0;
    margin_top_ = 0;
    margin_bottom_ = rows_ - 1;
    auto_wrap_ = true;
    origin_mode_ = bracketed_paste_ = insert_mode_ = cursor_key_mode_ = synchronized_updates_ = false;
    reverse_video_ = false;
    alternate_scroll_ = true;
    grapheme_clustering_ = false;
    color_scheme_updates_ = false;
    in_band_resize_ = false;
    focus_events_ = false;
    modify_other_keys_ = 0;
    mouse_tracking_ = MouseTrackingMode::None;
    mouse_format_ = MouseFormat::Normal;
    mouse_cursor_shape_ = platform::MouseCursorShape::Default;
    for (auto& stack : kitty_keyboard_stacks_) stack.clear();
    saved_dec_modes_.clear();
    title_stack_.clear();
    current_hyperlink_id_ = 0;
    hyperlinks_.clear();
    hyperlink_index_.clear();
    hyperlink_lru_.clear();
    hyperlink_free_.clear();
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

void TerminalEngine::clear_hyperlink_references(uint16_t id) noexcept {
    primary_grid_.clear_hyperlink_from_extras(id);
    alt_grid_.clear_hyperlink_from_extras(id);
}

uint16_t TerminalEngine::get_or_create_hyperlink_id(std::string_view url) {
    if (url.empty()) return 0;
    constexpr uint16_t kMaxHyperlinks = 4095; // 0 is reserved for "no link".
    const auto found = hyperlink_index_.find(std::string(url));
    if (found != hyperlink_index_.end()) {
        const auto id = found->second;
        hyperlink_lru_.erase(std::remove(hyperlink_lru_.begin(), hyperlink_lru_.end(), id),
            hyperlink_lru_.end());
        hyperlink_lru_.push_back(id);
        return id;
    }

    uint16_t id = 0;
    if (!hyperlink_free_.empty()) {
        id = hyperlink_free_.back();
        hyperlink_free_.pop_back();
    } else if (hyperlinks_.size() < kMaxHyperlinks) {
        id = static_cast<uint16_t>(hyperlinks_.size() + 1);
    } else {
        // Reuse the least recently used slot and detach cells that still point
        // at it; a uint16_t ID cannot carry a generation counter.
        id = hyperlink_lru_.front();
        hyperlink_lru_.pop_front();
        hyperlink_index_.erase(hyperlinks_[id - 1]);
        clear_hyperlink_references(id);
        hyperlinks_[id - 1].clear();
    }
    hyperlinks_.resize(std::max(hyperlinks_.size(), static_cast<size_t>(id)), {});
    hyperlinks_[id - 1].assign(url);
    hyperlink_index_.emplace(hyperlinks_[id - 1], id);
    hyperlink_lru_.push_back(id);
    return id;
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
    reset_grapheme_state();
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
    reset_grapheme_state();
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

void TerminalEngine::save_dec_mode(int mode, bool value) {
    saved_dec_modes_[mode] = value;
}

std::optional<bool> TerminalEngine::saved_dec_mode(int mode) const noexcept {
    const auto it = saved_dec_modes_.find(mode);
    if (it == saved_dec_modes_.end()) return std::nullopt;
    return it->second;
}

void TerminalEngine::push_window_title() {
    constexpr size_t kMaxTitleStack = 16;
    if (title_stack_.size() >= kMaxTitleStack) title_stack_.erase(title_stack_.begin());
    title_stack_.push_back(window_title_);
}

bool TerminalEngine::pop_window_title() {
    if (title_stack_.empty()) return false;
    window_title_ = std::move(title_stack_.back());
    title_stack_.pop_back();
    return true;
}

} // namespace gritty::core
