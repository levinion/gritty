#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace gritty::core {

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
        return Color{(2u << 24) | (static_cast<uint32_t>(r) << 16) |
                     (static_cast<uint32_t>(g) << 8) | b};
    }

    [[nodiscard]] constexpr ColorType type() const noexcept {
        return static_cast<ColorType>(value >> 24);
    }
    [[nodiscard]] constexpr bool is_default() const noexcept { return type() == ColorType::Default; }
    [[nodiscard]] constexpr bool is_indexed() const noexcept { return type() == ColorType::Indexed; }
    [[nodiscard]] constexpr bool is_rgb() const noexcept { return type() == ColorType::Rgb; }
    [[nodiscard]] constexpr uint8_t index() const noexcept { return static_cast<uint8_t>(value & 0xff); }
    [[nodiscard]] constexpr uint8_t r() const noexcept { return static_cast<uint8_t>((value >> 16) & 0xff); }
    [[nodiscard]] constexpr uint8_t g() const noexcept { return static_cast<uint8_t>((value >> 8) & 0xff); }
    [[nodiscard]] constexpr uint8_t b() const noexcept { return static_cast<uint8_t>(value & 0xff); }

    constexpr bool operator==(const Color&) const noexcept = default;
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
    uint16_t width : 2 {1}; // 1 = normal, 2 = wide leader, 0 = continuation
    uint16_t reserved : 4 {0};

    constexpr bool operator==(const CellAttributes&) const noexcept = default;
};

// Exactly four 32-bit words: [codepoint, fg, bg, attrs|extra_idx]. This layout
// is required by the NEON vld4q_u32/vst4q_u32 grid write path.
struct Cell {
    char32_t codepoint{' '};
    Color fg{Color::make_default()};
    Color bg{Color::make_default()};
    CellAttributes attrs{};
    uint16_t extra_idx{0};

    [[nodiscard]] constexpr bool is_empty() const noexcept {
        return (codepoint == ' ' || codepoint == 0) && extra_idx == 0 &&
               attrs.underline_style == 0 && !attrs.strikethrough && bg.is_default();
    }

    constexpr bool operator==(const Cell&) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<Cell> && std::is_standard_layout_v<Cell>);
static_assert(sizeof(Cell) == 16, "Cell must be exactly 16 bytes for 128-bit SIMD");
static_assert(offsetof(Cell, codepoint) == 0);
static_assert(offsetof(Cell, fg) == 4);
static_assert(offsetof(Cell, bg) == 8);
static_assert(offsetof(Cell, attrs) == 12);
static_assert(offsetof(Cell, extra_idx) == 14);
static_assert(sizeof(CellAttributes) == 2);

// Rare per-cell attributes. extra_idx == 0 means all fields are default and no
// side-table entry exists.
struct CellExtra {
    uint32_t combining_idx{0};
    Color underline_color{Color::make_default()};
    uint16_t hyperlink_id{0};

    [[nodiscard]] constexpr bool is_default() const noexcept {
        return combining_idx == 0 && underline_color.is_default() && hyperlink_id == 0;
    }

    constexpr bool operator==(const CellExtra&) const noexcept = default;
};

// Tail word layout on little-endian ARM64: [extra_idx:16][attrs:16].
[[nodiscard]] inline uint16_t cell_attr_bits(const CellAttributes& attrs) noexcept {
    uint16_t bits{0};
    std::memcpy(&bits, &attrs, sizeof(bits));
    return bits;
}

[[nodiscard]] inline uint32_t cell_tail_word(const Cell& cell) noexcept {
    return (static_cast<uint32_t>(cell.extra_idx) << 16) | cell_attr_bits(cell.attrs);
}

} // namespace gritty::core
