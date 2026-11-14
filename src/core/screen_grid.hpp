#pragma once

#include <cstdint>
#include <vector>
#include <span>
#include <string>
#include <algorithm>
#include "util/ring_buffer.hpp"

namespace squirt::core {

enum class ColorType : uint8_t {
    Default = 0,
    Indexed = 1,
    Rgb = 2,
};

struct Color {
    uint32_t value{0};

    static constexpr Color make_default() noexcept { return Color{0}; }
    static constexpr Color make_indexed(uint8_t idx) noexcept {
        return Color{(1u << 24) | idx};
    }
    static constexpr Color make_rgb(uint8_t r, uint8_t g, uint8_t b) noexcept {
        return Color{(2u << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b};
    }

    [[nodiscard]] constexpr ColorType type() const noexcept {
        return static_cast<ColorType>(value >> 24);
    }
    [[nodiscard]] constexpr bool is_default() const noexcept { return type() == ColorType::Default; }
    [[nodiscard]] constexpr bool is_indexed() const noexcept { return type() == ColorType::Indexed; }
    [[nodiscard]] constexpr bool is_rgb() const noexcept { return type() == ColorType::Rgb; }

    [[nodiscard]] constexpr uint8_t index() const noexcept { return static_cast<uint8_t>(value & 0xFF); }
    [[nodiscard]] constexpr uint8_t r() const noexcept { return static_cast<uint8_t>((value >> 16) & 0xFF); }
    [[nodiscard]] constexpr uint8_t g() const noexcept { return static_cast<uint8_t>((value >> 8) & 0xFF); }
    [[nodiscard]] constexpr uint8_t b() const noexcept { return static_cast<uint8_t>(value & 0xFF); }

    constexpr bool operator==(const Color& other) const noexcept = default;
};

enum class UnderlineStyle : uint8_t {
    None = 0,
    Single = 1,
    Double = 2,
    Curly = 3,
    Dotted = 4,
    Dashed = 5,
};

struct CellAttributes {
    uint16_t bold : 1 {0};
    uint16_t italic : 1 {0};
    uint16_t dim : 1 {0};
    uint16_t blink : 1 {0};
    uint16_t inverse : 1 {0};
    uint16_t invisible : 1 {0};
    uint16_t strikethrough : 1 {0};
    uint16_t underline_style : 3 {0};
    uint16_t width : 2 {1}; // 1 = normal, 2 = wide leader, 0 = wide continuation
    uint16_t reserved : 4 {0};

    constexpr bool operator==(const CellAttributes&) const noexcept = default;
};

struct Cell {
    char32_t codepoint{' '};
    uint32_t combining_idx{0};
    Color fg{Color::make_default()};
    Color bg{Color::make_default()};
    Color underline_color{Color::make_default()};
    CellAttributes attrs{};
    uint16_t hyperlink_id{0};

    [[nodiscard]] constexpr bool is_empty() const noexcept {
        return (codepoint == ' ' || codepoint == 0) && combining_idx == 0 &&
               attrs.underline_style == 0 && !attrs.strikethrough &&
               bg.is_default();
    }

    constexpr bool operator==(const Cell& other) const noexcept = default;
};

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

class ScreenGrid final {
public:
    ScreenGrid(int cols, int rows, size_t scrollback_capacity = 2000);
    ~ScreenGrid() = default;

    ScreenGrid(const ScreenGrid&) = default;
    ScreenGrid& operator=(const ScreenGrid&) = default;
    ScreenGrid(ScreenGrid&&) noexcept = default;
    ScreenGrid& operator=(ScreenGrid&&) noexcept = default;

    void resize(int cols, int rows);
    void set_scrollback_capacity(size_t capacity);

    [[nodiscard]] int cols() const noexcept { return cols_; }
    [[nodiscard]] int rows() const noexcept { return rows_; }
    [[nodiscard]] size_t scrollback_capacity() const noexcept { return scrollback_.capacity(); }
    [[nodiscard]] size_t scrollback_size() const noexcept { return scrollback_.size(); }

    [[nodiscard]] const Row& row(int y) const noexcept { return visible_rows_[y]; }
    [[nodiscard]] Row& row(int y) noexcept { return visible_rows_[y]; }

    [[nodiscard]] const Cell& cell(int x, int y) const noexcept { return visible_rows_[y].cells[x]; }
    [[nodiscard]] Cell& cell(int x, int y) noexcept { return visible_rows_[y].cells[x]; }

    void set_cell(int x, int y, const Cell& c) noexcept;

    // Scrollback retrieval: index 0 is oldest line in scrollback,
    // (scrollback_size - 1) is line immediately above visible row 0.
    [[nodiscard]] const Row& scrollback_row(size_t idx) const noexcept {
        return scrollback_[idx];
    }

    // Scroll region manipulation
    void scroll_up(int top, int bottom, int count, const Cell& empty_cell = Cell{});
    void scroll_down(int top, int bottom, int count, const Cell& empty_cell = Cell{});

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

    // View offset into scrollback (0 = live screen, > 0 = viewing history)
    [[nodiscard]] size_t view_offset() const noexcept { return view_offset_; }
    void scroll_view(int delta) noexcept;
    void reset_view() noexcept { view_offset_ = 0; }

    // Grapheme cluster store
    uint32_t store_combining_codepoints(std::span<const char32_t> cps);
    [[nodiscard]] std::span<const char32_t> get_combining_codepoints(uint32_t idx) const noexcept;

private:
    int cols_;
    int rows_;
    size_t view_offset_{0};
    std::vector<Row> visible_rows_;
    util::RingBuffer<Row> scrollback_;
    std::vector<std::vector<char32_t>> combining_table_;
};

} // namespace squirt::core
