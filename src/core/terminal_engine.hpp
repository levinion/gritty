#pragma once

#include <array>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>
#include <string>
#include <string_view>
#include <span>
#include <cstdint>
#include "core/screen_grid.hpp"
#include "core/kitty_graphics.hpp"
#include "util/raii.hpp"
#include "core/palette.hpp"
#include "platform/pal_interfaces.hpp"

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
    bool pending_wrap{false};
    std::array<bool, 4> graphic_charsets{};
    uint8_t selected_charset{0};
};

// A raster image anchored to cell coordinates on one screen. Kitty graphics
// placements land here; the renderer draws them in z-order (negative below
// text, non-negative above) and scroll/erase hooks keep them attached to the
// cells they were placed over.
struct PlacedImage {
    util::PixmanImage image;
    uint64_t uid{0};           // Per-placement token distinguishing anonymous placements.
    uint32_t image_id{0};      // Kitty image id (0 for anonymous transfers).
    uint32_t placement_id{0};  // Kitty placement id.
    int col{0};
    int row{0};
    int cols{1};
    int rows{1};
    int z_index{0};
    int src_x{0}, src_y{0}, src_w{0}, src_h{0}; // 0 size means the whole bitmap.
    int x_offset{0}, y_offset{0};
    int draw_w{0}, draw_h{0}; // Exact pixel target dimensions when non-zero; else cols*cell_w / rows*cell_h.
    int clip_top_row{0};
    int clip_bottom_row{-1}; // -1 means unclamped.
    int cursor_policy{1};
    bool alt_screen{false};

    // Render-side scaling cache. Pixman glyph/image targets are immutable while
    // attached; these fields are mutated only by CpuRenderer through a const
    // engine view.
    mutable util::PixmanImage scaled_cache;
    mutable const void* cache_source{nullptr};
    mutable int cache_src_x{0}, cache_src_y{0}, cache_src_w{0}, cache_src_h{0};
    mutable int cache_dst_w{0}, cache_dst_h{0};
};

class TerminalEngine final {
public:
    TerminalEngine(int cols, int rows);
    ~TerminalEngine() = default;

    TerminalEngine(const TerminalEngine&) = delete;
    TerminalEngine& operator=(const TerminalEngine&) = delete;

    void resize(int cols, int rows);

    [[nodiscard]] int cols() const noexcept { return cols_; }
    [[nodiscard]] int rows() const noexcept { return rows_; }
    void set_cell_size(int w, int h) noexcept { cell_width_ = w; cell_height_ = h; }
    [[nodiscard]] int cell_width() const noexcept { return cell_width_; }
    [[nodiscard]] int cell_height() const noexcept { return cell_height_; }

    [[nodiscard]] ScreenGrid& grid() noexcept { return *active_grid_; }
    [[nodiscard]] const ScreenGrid& grid() const noexcept { return *active_grid_; }
    [[nodiscard]] ScreenGrid& primary_grid() noexcept { return primary_grid_; }
    [[nodiscard]] const ScreenGrid& primary_grid() const noexcept { return primary_grid_; }
    [[nodiscard]] ScreenGrid& alt_grid() noexcept { return alt_grid_; }
    [[nodiscard]] const ScreenGrid& alt_grid() const noexcept { return alt_grid_; }

    [[nodiscard]] bool is_alt_screen() const noexcept { return is_alt_screen_; }
    void set_alt_screen(bool enable, int mode = 1049);
    void reset();
    void designate_charset(unsigned int index, bool graphics) noexcept;
    void select_charset(unsigned int index) noexcept;
    void repeat_last_char(int count);
    void scroll_up(int count);
    void scroll_down(int count);

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

    // OSC 22 pointer shape; reset to Default by RIS.
    [[nodiscard]] platform::MouseCursorShape mouse_cursor_shape() const noexcept {
        return mouse_cursor_shape_;
    }
    void set_mouse_cursor_shape(platform::MouseCursorShape shape) noexcept {
        mouse_cursor_shape_ = shape;
    }

    // Cursor movement
    void set_cursor_pos(int col, int row);

    // Placed raster images (Kitty graphics only).
    [[nodiscard]] const std::vector<PlacedImage>& images() const noexcept { return images_; }
    [[nodiscard]] KittyGraphicsEngine& kitty_graphics() noexcept { return kitty_graphics_; }
    [[nodiscard]] const KittyGraphicsEngine& kitty_graphics() const noexcept { return kitty_graphics_; }
    void place_image(PlacedImage placement);
    void clear_images() noexcept;
    void scroll_images(int top, int bottom, int rows) noexcept;
    void remove_image_by_uid(uint64_t uid) noexcept;
    void remove_images_by_id(uint32_t image_id) noexcept;
    void remove_images_by_placement(uint32_t placement_id) noexcept;
    void remove_images_by_placement(uint32_t image_id, uint32_t placement_id) noexcept;
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
    void print_ascii_char(uint8_t ch);
    // Fast path for a run of printable single-width ASCII with no active
    // charset translation or insert mode. Falls back to print_char otherwise.
    void print_ascii_run(std::string_view ascii);
    // Fast path for a run of common double-width CJK codepoints. Falls back
    // to print_char() when charset translation or insert mode is active.
    void print_wide_run(std::span<const char32_t> cps);
    void print_utf8(std::string_view utf8_str);

    // Clipboard/IME insertions are formatted here so every paste path obeys
    // DECSET 2004 and cannot smuggle an early bracketed-paste terminator.
    [[nodiscard]] std::string format_paste_payload(std::string_view text) const;

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
    // DECSET 1004: report window focus as CSI I / CSI O (tmux focus-events).
    [[nodiscard]] bool focus_events() const noexcept { return focus_events_; }
    void set_focus_events(bool enable) noexcept { focus_events_ = enable; }
    [[nodiscard]] bool insert_mode() const noexcept { return insert_mode_; }
    void set_insert_mode(bool enable) noexcept { insert_mode_ = enable; }
    [[nodiscard]] bool cursor_key_mode() const noexcept { return cursor_key_mode_; }
    void set_cursor_key_mode(bool enable) noexcept { cursor_key_mode_ = enable; }
    [[nodiscard]] int modify_other_keys() const noexcept { return modify_other_keys_; }
    void set_modify_other_keys(int mode) noexcept { modify_other_keys_ = mode; }

    // Synchronized updates (DECSET 2026)
    [[nodiscard]] bool synchronized_updates() const noexcept { return synchronized_updates_; }
    void set_synchronized_updates(bool enable) noexcept { synchronized_updates_ = enable; }

    // DECSCNM (DECSET 5), Alternate Scroll (1007), Grapheme Clustering (2027),
    // Color Scheme Updates (2031), In-band Resize (2048).
    [[nodiscard]] bool reverse_video() const noexcept { return reverse_video_; }
    void set_reverse_video(bool enable) noexcept {
        if (reverse_video_ == enable) return;
        reverse_video_ = enable;
        active_grid_->mark_all_dirty();
    }
    [[nodiscard]] bool alternate_scroll() const noexcept { return alternate_scroll_; }
    void set_alternate_scroll(bool enable) noexcept { alternate_scroll_ = enable; }
    [[nodiscard]] bool grapheme_clustering() const noexcept { return grapheme_clustering_; }
    void set_grapheme_clustering(bool enable) noexcept {
        grapheme_clustering_ = enable;
        reset_grapheme_state();
    }
    [[nodiscard]] bool color_scheme_updates() const noexcept { return color_scheme_updates_; }
    void set_color_scheme_updates(bool enable) noexcept { color_scheme_updates_ = enable; }
    [[nodiscard]] bool in_band_resize() const noexcept { return in_band_resize_; }
    void set_in_band_resize(bool enable) noexcept { in_band_resize_ = enable; }

    // XTSAVE / XTRESTORE saved private DEC modes.
    void save_dec_mode(int mode, bool value);
    [[nodiscard]] std::optional<bool> saved_dec_mode(int mode) const noexcept;

    // Mouse tracking
    [[nodiscard]] MouseTrackingMode mouse_tracking() const noexcept { return mouse_tracking_; }
    void set_mouse_tracking(MouseTrackingMode mode) noexcept { mouse_tracking_ = mode; }
    [[nodiscard]] MouseFormat mouse_format() const noexcept { return mouse_format_; }
    void set_mouse_format(MouseFormat fmt) noexcept { mouse_format_ = fmt; }

    // Kitty Keyboard Protocol (CSI u)
    [[nodiscard]] uint32_t kitty_keyboard_flags() const noexcept {
        const auto& stack = kitty_keyboard_stacks_[is_alt_screen_ ? 1 : 0];
        return stack.empty() ? 0 : stack.back();
    }
    void push_kitty_keyboard_flags(uint32_t flags);
    void pop_kitty_keyboard_flags(size_t count = 1);
    void set_kitty_keyboard_flags(uint32_t flags, int mode);

    // Palette management (256 colors)
    [[nodiscard]] uint32_t palette_color(uint8_t index) const noexcept { return palette_[index]; }
    void set_palette_color(uint8_t index, uint32_t rgb24) noexcept;
    void reset_palette_color(uint8_t index) noexcept;
    void configure_palette(const std::array<uint32_t, 256>& colors) noexcept;
    void reset_palette() noexcept;
    // Immutable initial colour snapshot used by OSC resets and RIS.
    void configure_initial_colors(uint32_t foreground, uint32_t background,
                                  uint32_t cursor_color) noexcept;
    [[nodiscard]] uint32_t configured_foreground() const noexcept { return configured_fg_; }
    [[nodiscard]] uint32_t configured_background() const noexcept { return configured_bg_; }
    [[nodiscard]] uint32_t configured_cursor_color() const noexcept { return configured_cursor_; }

    // Window title & OSC properties
    [[nodiscard]] std::string_view window_title() const noexcept { return window_title_; }
    void set_window_title(std::string_view title) { window_title_ = title; }
    void push_window_title();
    bool pop_window_title();
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
    void clear_hyperlink_references(uint16_t id) noexcept;
    void reset_grapheme_state() noexcept {
        if (last_grapheme_valid_) {
            last_grapheme_valid_ = false;
            grapheme_state_ = 0;
        }
    }

    int cols_;
    int rows_;
    int cell_width_{0};
    int cell_height_{0};
    ScreenGrid primary_grid_;
    ScreenGrid alt_grid_;
    ScreenGrid* active_grid_;
    bool is_alt_screen_{false};

    CursorState cursor_;
    CursorState saved_cursor_primary_;
    CursorState saved_cursor_alt_;
    bool wrap_next_{false}; // Pending wrap state
    std::array<bool, 4> graphic_charsets_{};
    uint8_t selected_charset_{0};
    char32_t last_printed_{0};

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
    bool focus_events_{false};
    void damage_image_rows(int row, int rows) noexcept;
    bool insert_mode_{false};
    bool cursor_key_mode_{false};
    bool synchronized_updates_{false};
    bool reverse_video_{false};
    bool alternate_scroll_{true};
    bool grapheme_clustering_{false};
    bool color_scheme_updates_{false};
    bool in_band_resize_{false};
    int modify_other_keys_{0};
    char32_t last_grapheme_cp_{0};
    int32_t grapheme_state_{0};
    bool last_grapheme_valid_{false};
    std::unordered_map<int, bool> saved_dec_modes_;
    std::vector<std::string> title_stack_;

    MouseTrackingMode mouse_tracking_{MouseTrackingMode::None};
    MouseFormat mouse_format_{MouseFormat::Normal};
    platform::MouseCursorShape mouse_cursor_shape_{platform::MouseCursorShape::Default};

    std::array<std::vector<uint32_t>, 2> kitty_keyboard_stacks_;
    std::vector<PlacedImage> images_;
    KittyGraphicsEngine kitty_graphics_{(size_t{32} << 20)}; // Bound image memory with a tmux-sized budget.
    std::vector<bool> tabs_;
    std::array<uint32_t, 256> palette_;
    std::array<uint32_t, 256> configured_palette_{kDefaultPalette};
    uint32_t configured_fg_{0xffdddddd};
    uint32_t configured_bg_{0xff111111};
    uint32_t configured_cursor_{0xffffffff};

    std::string window_title_;
    std::string cwd_;
    // Slot 0 is unused; IDs are stable until the bounded table evicts an entry.
    std::vector<std::string> hyperlinks_;
    std::unordered_map<std::string, uint16_t> hyperlink_index_;
    std::deque<uint16_t> hyperlink_lru_;
    std::vector<uint16_t> hyperlink_free_;
};

} // namespace gritty::core
