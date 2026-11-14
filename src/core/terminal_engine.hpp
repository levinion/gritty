#pragma once

#include <array>
#include <vector>
#include <string>
#include <string_view>
#include <cstdint>
#include <memory>
#include "core/screen_grid.hpp"

namespace gritty::core {

enum class CursorShape : uint8_t {
    Block = 0,
    Beam = 1,
    Underline = 2,
};

enum class MouseTrackingMode : uint16_t {
    None = 0,
    X10 = 9,
    Normal = 1000,
    ButtonEvent = 1002,
    AnyEvent = 1003,
};

enum class MouseFormat : uint16_t {
    Normal = 0,
    Utf8 = 1005,
    Sgr = 1006,
    Urxvt = 1015,
    SgrPixels = 1016,
};

struct CursorState {
    int col{0};
    int row{0};
    bool visible{true};
    bool blink{false};
    CursorShape shape{CursorShape::Block};

    // Attributes at time of save
    Color fg{Color::make_default()};
    Color bg{Color::make_default()};
    Color underline_color{Color::make_default()};
    CellAttributes attrs{};
    bool origin_mode{false};
    bool auto_wrap{true};
};

class TerminalEngine final {
public:
    TerminalEngine(int cols, int rows, size_t scrollback_capacity = 2000);
    ~TerminalEngine() = default;

    TerminalEngine(const TerminalEngine&) = delete;
    TerminalEngine& operator=(const TerminalEngine&) = delete;

    void resize(int cols, int rows);

    [[nodiscard]] int cols() const noexcept { return cols_; }
    [[nodiscard]] int rows() const noexcept { return rows_; }

    [[nodiscard]] ScreenGrid& grid() noexcept { return *active_grid_; }
    [[nodiscard]] const ScreenGrid& grid() const noexcept { return *active_grid_; }
    [[nodiscard]] ScreenGrid& primary_grid() noexcept { return primary_grid_; }
    [[nodiscard]] const ScreenGrid& primary_grid() const noexcept { return primary_grid_; }
    [[nodiscard]] ScreenGrid& alt_grid() noexcept { return alt_grid_; }
    [[nodiscard]] const ScreenGrid& alt_grid() const noexcept { return alt_grid_; }

    [[nodiscard]] bool is_alt_screen() const noexcept { return is_alt_screen_; }
    void set_alt_screen(bool enable);

    // Cursor access
    [[nodiscard]] int cursor_col() const noexcept { return cursor_.col; }
    [[nodiscard]] int cursor_row() const noexcept { return cursor_.row; }
    [[nodiscard]] bool cursor_visible() const noexcept { return cursor_.visible; }
    [[nodiscard]] CursorShape cursor_shape() const noexcept { return cursor_.shape; }
    [[nodiscard]] bool cursor_blink() const noexcept { return cursor_.blink; }
    void set_cursor_visible(bool v) noexcept { cursor_.visible = v; }
    void set_cursor_shape(CursorShape s, bool blink) noexcept {
        cursor_.shape = s;
        cursor_.blink = blink;
    }

    // Cursor movement
    void set_cursor_pos(int col, int row);
    void move_cursor_up(int count);
    void move_cursor_down(int count);
    void move_cursor_forward(int count);
    void move_cursor_backward(int count);
    void set_cursor_col(int col);
    void set_cursor_row(int row);
    void carriage_return();
    void line_feed();
    void reverse_index(); // RI (ESC M)

    // Cursor save / restore
    void save_cursor();
    void restore_cursor();

    // Text output
    void print_char(char32_t cp);
    void print_utf8(std::string_view utf8_str);

    // Editing operations
    void erase_in_display(int mode);
    void erase_in_line(int mode);
    void insert_lines(int count);
    void delete_lines(int count);
    void insert_chars(int count);
    void delete_chars(int count);
    void erase_chars(int count);

    // Scrolling margins (DECSTBM)
    void set_margins(int top, int bottom);
    [[nodiscard]] int margin_top() const noexcept { return margin_top_; }
    [[nodiscard]] int margin_bottom() const noexcept { return margin_bottom_; }

    // Tabs
    void tab_forward(int count = 1);
    void tab_backward(int count = 1);
    void set_tab_stop();
    void clear_tab_stop(int mode); // 0 = at current col, 3 = clear all

    // SGR (Select Graphic Rendition)
    void sgr_reset();
    void sgr_bold(bool enable) { current_attrs_.bold = enable; }
    void sgr_dim(bool enable) { current_attrs_.dim = enable; }
    void sgr_italic(bool enable) { current_attrs_.italic = enable; }
    void sgr_underline(UnderlineStyle style) { current_attrs_.underline_style = static_cast<uint16_t>(style); }
    void sgr_blink(bool enable) { current_attrs_.blink = enable; }
    void sgr_inverse(bool enable) { current_attrs_.inverse = enable; }
    void sgr_invisible(bool enable) { current_attrs_.invisible = enable; }
    void sgr_strikethrough(bool enable) { current_attrs_.strikethrough = enable; }
    void sgr_fg_color(Color c) { current_fg_ = c; }
    void sgr_bg_color(Color c) { current_bg_ = c; }
    void sgr_underline_color(Color c) { current_underline_color_ = c; }

    [[nodiscard]] Color current_fg() const noexcept { return current_fg_; }
    [[nodiscard]] Color current_bg() const noexcept { return current_bg_; }
    [[nodiscard]] Color current_underline_color() const noexcept { return current_underline_color_; }
    [[nodiscard]] CellAttributes current_attrs() const noexcept { return current_attrs_; }

    // Modes
    [[nodiscard]] bool auto_wrap() const noexcept { return auto_wrap_; }
    void set_auto_wrap(bool enable) noexcept { auto_wrap_ = enable; }
    [[nodiscard]] bool origin_mode() const noexcept { return origin_mode_; }
    void set_origin_mode(bool enable) noexcept;
    [[nodiscard]] bool bracketed_paste() const noexcept { return bracketed_paste_; }
    void set_bracketed_paste(bool enable) noexcept { bracketed_paste_ = enable; }
    [[nodiscard]] bool insert_mode() const noexcept { return insert_mode_; }
    void set_insert_mode(bool enable) noexcept { insert_mode_ = enable; }
    [[nodiscard]] bool cursor_key_mode() const noexcept { return cursor_key_mode_; }
    void set_cursor_key_mode(bool enable) noexcept { cursor_key_mode_ = enable; }

    // Synchronized updates (DECSET 2026)
    [[nodiscard]] bool synchronized_updates() const noexcept { return synchronized_updates_; }
    void set_synchronized_updates(bool enable) noexcept { synchronized_updates_ = enable; }

    // Mouse tracking
    [[nodiscard]] MouseTrackingMode mouse_tracking() const noexcept { return mouse_tracking_; }
    void set_mouse_tracking(MouseTrackingMode mode) noexcept { mouse_tracking_ = mode; }
    [[nodiscard]] MouseFormat mouse_format() const noexcept { return mouse_format_; }
    void set_mouse_format(MouseFormat fmt) noexcept { mouse_format_ = fmt; }

    // Kitty Keyboard Protocol (CSI u)
    [[nodiscard]] uint32_t kitty_keyboard_flags() const noexcept {
        return kitty_keyboard_stack_.empty() ? 0 : kitty_keyboard_stack_.back();
    }
    void push_kitty_keyboard_flags(uint32_t flags);
    void pop_kitty_keyboard_flags(size_t count = 1);
    void set_kitty_keyboard_flags(uint32_t flags, int mode);

    // Palette management (256 colors)
    [[nodiscard]] uint32_t palette_color(uint8_t index) const noexcept { return palette_[index]; }
    void set_palette_color(uint8_t index, uint32_t rgb24) noexcept { palette_[index] = rgb24; }
    void reset_palette() noexcept;

    // Window title & OSC properties
    [[nodiscard]] std::string_view window_title() const noexcept { return window_title_; }
    void set_window_title(std::string_view title) { window_title_ = title; }
    [[nodiscard]] std::string_view cwd() const noexcept { return cwd_; }
    void set_cwd(std::string_view path) { cwd_ = path; }

    // OSC 8 Hyperlinks
    uint16_t get_or_create_hyperlink_id(std::string_view url);
    [[nodiscard]] std::string_view get_hyperlink_url(uint16_t id) const noexcept;
    void set_current_hyperlink(std::string_view url);
    void clear_current_hyperlink() noexcept { current_hyperlink_id_ = 0; }

private:
    void init_tabs();
    void scroll_if_needed();

    int cols_;
    int rows_;
    ScreenGrid primary_grid_;
    ScreenGrid alt_grid_;
    ScreenGrid* active_grid_;
    bool is_alt_screen_{false};

    CursorState cursor_;
    CursorState saved_cursor_primary_;
    CursorState saved_cursor_alt_;
    bool wrap_next_{false}; // Pending wrap state

    int margin_top_{0};
    int margin_bottom_{0};

    Color current_fg_{Color::make_default()};
    Color current_bg_{Color::make_default()};
    Color current_underline_color_{Color::make_default()};
    CellAttributes current_attrs_{};
    uint16_t current_hyperlink_id_{0};

    bool auto_wrap_{true};
    bool origin_mode_{false};
    bool bracketed_paste_{false};
    bool insert_mode_{false};
    bool cursor_key_mode_{false};
    bool synchronized_updates_{false};

    MouseTrackingMode mouse_tracking_{MouseTrackingMode::None};
    MouseFormat mouse_format_{MouseFormat::Normal};

    std::vector<uint32_t> kitty_keyboard_stack_;
    std::vector<bool> tabs_;
    std::array<uint32_t, 256> palette_;

    std::string window_title_;
    std::string cwd_;
    std::vector<std::string> hyperlinks_;
};

} // namespace gritty::core
