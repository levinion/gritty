#include "render/box_drawing.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace gritty::render {

namespace {

enum LineType : uint8_t {
    None = 0,
    Light = 1,
    Heavy = 2,
    Double = 3,
};

struct BoxSides {
    LineType up{None};
    LineType down{None};
    LineType left{None};
    LineType right{None};
    uint8_t dashes{0};    // 0 = solid; otherwise dash count across the cell.
    uint8_t arcs{0};      // 1..4 = U+256D..U+2570 quarter arcs.
    uint8_t diagonals{0}; // bit 0 = '/', bit 1 = '\\'.
};

// The entire U+2500..U+257F block is described here instead of relying on font
// fallback: procedural pixels always align to cell/physical pixel boundaries.
constexpr std::array<BoxSides, 0x80> build_box_table() noexcept {
    std::array<BoxSides, 0x80> table{};
    const auto put = [&table](int cp, BoxSides sides) noexcept {
        table[static_cast<size_t>(cp - 0x2500)] = sides;
    };

    put(0x2500, {.left = Light, .right = Light});
    put(0x2501, {.left = Heavy, .right = Heavy});
    put(0x2502, {.up = Light, .down = Light});
    put(0x2503, {.up = Heavy, .down = Heavy});
    for (int i = 0; i < 2; ++i) {
        const LineType weight = i == 0 ? Light : Heavy;
        put(0x2504 + i, {.left = weight, .right = weight, .dashes = 3});
        put(0x2506 + i, {.up = weight, .down = weight, .dashes = 3});
        put(0x2508 + i, {.left = weight, .right = weight, .dashes = 4});
        put(0x250A + i, {.up = weight, .down = weight, .dashes = 4});
    }

    // Mixed-weight corners: (light,light), (vertical light/horizontal heavy),
    // (vertical heavy/horizontal light), (heavy,heavy).
    const auto corner_weight = [](int index, LineType& vertical, LineType& horizontal) noexcept {
        vertical = index == 2 || index == 3 ? Heavy : Light;
        horizontal = index == 1 || index == 3 ? Heavy : Light;
    };
    for (int i = 0; i < 4; ++i) {
        LineType v = Light, h = Light;
        corner_weight(i, v, h);
        put(0x250C + i, {.down = v, .right = h});
        put(0x2510 + i, {.down = v, .left = h});
        put(0x2514 + i, {.up = v, .right = h});
        put(0x2518 + i, {.up = v, .left = h});
    }

    // T-junctions and crosses with independently weighted arms.
    constexpr BoxSides junctions[6][8] = {
        { // 251C..2523: vertical + right
            {.up = Light, .down = Light, .right = Light},
            {.up = Light, .down = Light, .right = Heavy},
            {.up = Heavy, .down = Light, .right = Light},
            {.up = Light, .down = Heavy, .right = Light},
            {.up = Heavy, .down = Heavy, .right = Light},
            {.up = Heavy, .down = Light, .right = Heavy},
            {.up = Light, .down = Heavy, .right = Heavy},
            {.up = Heavy, .down = Heavy, .right = Heavy},
        },
        { // 2524..252B: vertical + left
            {.up = Light, .down = Light, .left = Light},
            {.up = Light, .down = Light, .left = Heavy},
            {.up = Heavy, .down = Light, .left = Light},
            {.up = Light, .down = Heavy, .left = Light},
            {.up = Heavy, .down = Heavy, .left = Light},
            {.up = Heavy, .down = Light, .left = Heavy},
            {.up = Light, .down = Heavy, .left = Heavy},
            {.up = Heavy, .down = Heavy, .left = Heavy},
        },
        { // 252C..2533: down + horizontal
            {.down = Light, .left = Light, .right = Light},
            {.down = Light, .left = Heavy, .right = Light},
            {.down = Light, .left = Light, .right = Heavy},
            {.down = Light, .left = Heavy, .right = Heavy},
            {.down = Heavy, .left = Light, .right = Light},
            {.down = Heavy, .left = Heavy, .right = Light},
            {.down = Heavy, .left = Light, .right = Heavy},
            {.down = Heavy, .left = Heavy, .right = Heavy},
        },
        { // 2534..253B: up + horizontal
            {.up = Light, .left = Light, .right = Light},
            {.up = Light, .left = Heavy, .right = Light},
            {.up = Light, .left = Light, .right = Heavy},
            {.up = Light, .left = Heavy, .right = Heavy},
            {.up = Heavy, .left = Light, .right = Light},
            {.up = Heavy, .left = Heavy, .right = Light},
            {.up = Heavy, .left = Light, .right = Heavy},
            {.up = Heavy, .left = Heavy, .right = Heavy},
        },
        { // 253C..2543: four-way crosses
            {.up = Light, .down = Light, .left = Light, .right = Light},
            {.up = Light, .down = Light, .left = Heavy, .right = Light},
            {.up = Light, .down = Light, .left = Light, .right = Heavy},
            {.up = Light, .down = Light, .left = Heavy, .right = Heavy},
            {.up = Heavy, .down = Light, .left = Light, .right = Light},
            {.up = Light, .down = Heavy, .left = Light, .right = Light},
            {.up = Heavy, .down = Heavy, .left = Light, .right = Light},
            {.up = Heavy, .down = Light, .left = Heavy, .right = Light},
        },
        { // 2544..254B: remaining asymmetric crosses
            {.up = Heavy, .down = Light, .left = Light, .right = Heavy},
            {.up = Light, .down = Heavy, .left = Heavy, .right = Light},
            {.up = Light, .down = Heavy, .left = Light, .right = Heavy},
            {.up = Heavy, .down = Light, .left = Heavy, .right = Heavy},
            {.up = Light, .down = Heavy, .left = Heavy, .right = Heavy},
            {.up = Heavy, .down = Heavy, .left = Heavy, .right = Light},
            {.up = Heavy, .down = Heavy, .left = Light, .right = Heavy},
            {.up = Heavy, .down = Heavy, .left = Heavy, .right = Heavy},
        },
    };
    for (int group = 0; group < 6; ++group)
        for (int i = 0; i < 8; ++i)
            put(0x251C + group * 8 + i, junctions[group][i]);

    // 254C..254F: double-dash horizontal/vertical lines.
    put(0x254C, {.left = Light, .right = Light, .dashes = 2});
    put(0x254D, {.left = Heavy, .right = Heavy, .dashes = 2});
    put(0x254E, {.up = Light, .down = Light, .dashes = 2});
    put(0x254F, {.up = Heavy, .down = Heavy, .dashes = 2});

    // Double-line family, including every mixed single/double corner.
    put(0x2550, {.left = Double, .right = Double});
    put(0x2551, {.up = Double, .down = Double});
    const BoxSides double_corners[12] = {
        {.down = Light, .right = Double}, {.down = Double, .right = Light},
        {.down = Double, .right = Double}, {.down = Light, .left = Double},
        {.down = Double, .left = Light},  {.down = Double, .left = Double},
        {.up = Light, .right = Double},   {.up = Double, .right = Light},
        {.up = Double, .right = Double},  {.up = Light, .left = Double},
        {.up = Double, .left = Light},    {.up = Double, .left = Double},
    };
    for (int i = 0; i < 12; ++i) put(0x2552 + i, double_corners[i]);
    const BoxSides double_tees[12] = {
        {.up = Light, .down = Light, .right = Double},
        {.up = Double, .down = Double, .right = Light},
        {.up = Double, .down = Double, .right = Double},
        {.up = Light, .down = Light, .left = Double},
        {.up = Double, .down = Double, .left = Light},
        {.up = Double, .down = Double, .left = Double},
        {.down = Light, .left = Double, .right = Double},
        {.down = Double, .left = Light, .right = Light},
        {.down = Double, .left = Double, .right = Double},
        {.up = Light, .left = Double, .right = Double},
        {.up = Double, .left = Light, .right = Light},
        {.up = Double, .left = Double, .right = Double},
    };
    for (int i = 0; i < 12; ++i) put(0x255E + i, double_tees[i]);
    put(0x256A, {.up = Light, .down = Light, .left = Double, .right = Double});
    put(0x256B, {.up = Double, .down = Double, .left = Light, .right = Light});
    put(0x256C, {.up = Double, .down = Double, .left = Double, .right = Double});

    // Rounded corners and diagonals.
    put(0x256D, {.arcs = 1});
    put(0x256E, {.arcs = 2});
    put(0x256F, {.arcs = 3});
    put(0x2570, {.arcs = 4});
    put(0x2571, {.diagonals = 1});
    put(0x2572, {.diagonals = 2});
    put(0x2573, {.diagonals = 3});

    // Half lines, including the light/heavy mixed variants.
    put(0x2574, {.left = Light});
    put(0x2575, {.up = Light});
    put(0x2576, {.right = Light});
    put(0x2577, {.down = Light});
    put(0x2578, {.left = Heavy});
    put(0x2579, {.up = Heavy});
    put(0x257A, {.right = Heavy});
    put(0x257B, {.down = Heavy});
    put(0x257C, {.left = Light, .right = Heavy});
    put(0x257D, {.up = Light, .down = Heavy});
    put(0x257E, {.left = Heavy, .right = Light});
    put(0x257F, {.up = Heavy, .down = Light});
    return table;
}

constexpr auto kBoxTable = build_box_table();

BoxSides decode_box_char(char32_t cp) noexcept {
    if (cp < 0x2500 || cp > 0x257F) return {};
    return kBoxTable[static_cast<size_t>(cp - 0x2500)];
}

// Opaque a8r8g8b8 cells are the overwhelmingly common case for box drawing.
// Pixman's 16-bit color channels convert to bytes by taking the high byte.
bool fill_opaque_rect(pixman_image_t* dst, const pixman_color_t& color,
                      int x, int y, int width, int height) noexcept {
    if (!dst || pixman_image_get_format(dst) != PIXMAN_a8r8g8b8 ||
        color.alpha != 0xffff || width <= 0 || height <= 0) return false;

    const int image_w = pixman_image_get_width(dst);
    const int image_h = pixman_image_get_height(dst);
    x = std::max(x, 0);
    y = std::max(y, 0);
    width = std::min(width, image_w - x);
    height = std::min(height, image_h - y);
    if (width <= 0 || height <= 0) return false;

    const uint32_t pixel = (static_cast<uint32_t>(color.alpha >> 8) << 24) |
                           (static_cast<uint32_t>(color.red >> 8) << 16) |
                           (static_cast<uint32_t>(color.green >> 8) << 8) |
                           static_cast<uint32_t>(color.blue >> 8);
    auto* data = reinterpret_cast<uint8_t*>(pixman_image_get_data(dst));
    if (!data) return false;
    const int stride = pixman_image_get_stride(dst);
    for (int row = 0; row < height; ++row) {
        auto* pixels = reinterpret_cast<uint32_t*>(data + static_cast<size_t>(y + row) * stride +
                                                   static_cast<size_t>(x) * 4);
        std::fill_n(pixels, static_cast<size_t>(width), pixel);
    }
    return true;
}

void fill_rect(pixman_image_t* dst, const pixman_color_t& color,
               int x, int y, int width, int height) noexcept {
    if (width <= 0 || height <= 0 || !dst) return;
    if (fill_opaque_rect(dst, color, x, y, width, height)) return;
    const pixman_rectangle16_t rect{
        static_cast<int16_t>(x), static_cast<int16_t>(y),
        static_cast<uint16_t>(std::min(width, 65535)),
        static_cast<uint16_t>(std::min(height, 65535)),
    };
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, dst, &color, 1, &rect);
}

void draw_dashes(pixman_image_t* dst, const pixman_color_t& color,
                 const BoxSides& sides, int x, int y, int w, int h,
                 int light_thick, int heavy_thick) noexcept {
    if (sides.left != None || sides.right != None) {
        const int count = sides.dashes > 0 ? sides.dashes : 3;
        const int thick = sides.left == Heavy || sides.right == Heavy ? heavy_thick : light_thick;
        const int mid = y + h / 2;
        const int dash = std::max(1, w / (2 * count - 1));
        const int gap = count > 1 ? std::max(1, (w - count * dash) / (count - 1)) : 0;
        for (int i = 0; i < count; ++i) {
            const int start = x + i * (dash + gap);
            if (start >= x + w) break;
            const int length = i + 1 == count ? x + w - start : dash;
            fill_rect(dst, color, start, mid - thick / 2, length, thick);
        }
    }
    if (sides.up != None || sides.down != None) {
        const int count = sides.dashes > 0 ? sides.dashes : 3;
        const int thick = sides.up == Heavy || sides.down == Heavy ? heavy_thick : light_thick;
        const int mid = x + w / 2;
        const int dash = std::max(1, h / (2 * count - 1));
        const int gap = count > 1 ? std::max(1, (h - count * dash) / (count - 1)) : 0;
        for (int i = 0; i < count; ++i) {
            const int start = y + i * (dash + gap);
            if (start >= y + h) break;
            const int length = i + 1 == count ? y + h - start : dash;
            fill_rect(dst, color, mid - thick / 2, start, thick, length);
        }
    }
}

// Quarter-circle arcs for U+256D..U+2570. The centre is the corner opposite
// the rounded corner, and the radius is half the shorter cell dimension so the
// arc meets the neighbouring cell's straight segments at their midpoints.
void draw_arc(pixman_image_t* dst, const pixman_color_t& color, uint8_t arc,
              int x, int y, int w, int h, int thick) noexcept {
    const int cx = x + (arc == 1 || arc == 4 ? w - 1 : 0);
    const int cy = y + (arc == 1 || arc == 2 ? h - 1 : 0);
    const double radius = static_cast<double>(std::max(1, std::min(w, h) / 2));
    const double inner = std::max(0.0, radius - thick);
    for (int py = y; py < y + h; ++py) {
        int first = -1, last = -1;
        for (int px = x; px < x + w; ++px) {
            const double dx = static_cast<double>(px - cx);
            const double dy = static_cast<double>(py - cy);
            const double dist = std::sqrt(dx * dx + dy * dy);
            if (dist >= inner && dist <= radius + 0.5) {
                if (first < 0) first = px;
                last = px;
            }
        }
        if (first >= 0) fill_rect(dst, color, first, py, last - first + 1, 1);
    }
}

void draw_diagonal(pixman_image_t* dst, const pixman_color_t& color,
                   uint8_t diagonals, int x, int y, int w, int h,
                   int thick) noexcept {
    const auto line = [&](int x0, int y0, int x1, int y1) noexcept {
        const int dx = std::abs(x1 - x0);
        const int dy = std::abs(y1 - y0);
        const int sx = x0 < x1 ? 1 : -1;
        const int sy = y0 < y1 ? 1 : -1;
        int err = dx - dy;
        const int off = thick / 2;
        while (true) {
            fill_rect(dst, color, x0 - off, y0 - off, thick, thick);
            if (x0 == x1 && y0 == y1) break;
            const int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 < dx) { err += dx; y0 += sy; }
        }
    };
    if (diagonals & 1) line(x, y + h - 1, x + w - 1, y);
    if (diagonals & 2) line(x, y, x + w - 1, y + h - 1);
}

} // namespace

bool BoxDrawingRenderer::render(
    pixman_image_t* dst,
    char32_t cp,
    int cell_x,
    int cell_y,
    int cell_width,
    int cell_height,
    pixman_color_t color
) noexcept {
    if (!dst || cell_width <= 0 || cell_height <= 0) return false;

    // Block elements (0x2580 - 0x259F), including shades and 1/8 blocks.
    if (cp >= 0x2580 && cp <= 0x259F) {
        pixman_rectangle16_t rects[4];
        int num_rects = 0;

        if (cp == 0x2588) { // Full block
            if (fill_opaque_rect(dst, color, cell_x, cell_y, cell_width, cell_height)) return true;
            rects[0] = {static_cast<int16_t>(cell_x), static_cast<int16_t>(cell_y),
                        static_cast<uint16_t>(cell_width), static_cast<uint16_t>(cell_height)};
            num_rects = 1;
        } else if (cp == 0x2580) { // Upper half
            rects[0] = {static_cast<int16_t>(cell_x), static_cast<int16_t>(cell_y),
                        static_cast<uint16_t>(cell_width),
                        static_cast<uint16_t>(std::max(1, cell_height / 2))};
            num_rects = 1;
        } else if (cp >= 0x2581 && cp <= 0x2587) { // Lower 1/8 to 7/8
            const int fraction = cp - 0x2580;
            const int h = std::max(1, (cell_height * fraction) / 8);
            rects[0] = {static_cast<int16_t>(cell_x), static_cast<int16_t>(cell_y + cell_height - h),
                        static_cast<uint16_t>(cell_width), static_cast<uint16_t>(h)};
            num_rects = 1;
        } else if (cp >= 0x2589 && cp <= 0x258F) { // Left 7/8 to 1/8
            const int fraction = 8 - (cp - 0x2588);
            const int w = std::max(1, (cell_width * fraction) / 8);
            rects[0] = {static_cast<int16_t>(cell_x), static_cast<int16_t>(cell_y),
                        static_cast<uint16_t>(w), static_cast<uint16_t>(cell_height)};
            num_rects = 1;
        } else if (cp == 0x2590) { // Right half
            const int w = std::max(1, cell_width - cell_width / 2);
            rects[0] = {static_cast<int16_t>(cell_x + cell_width / 2), static_cast<int16_t>(cell_y),
                        static_cast<uint16_t>(w), static_cast<uint16_t>(cell_height)};
            num_rects = 1;
        } else if (cp >= 0x2591 && cp <= 0x2593) { // 25/50/75% ordered dither.
            constexpr uint8_t kThreshold[4] = {0, 2, 3, 1};
            const int level = cp - 0x2590;
            for (int py = 0; py < cell_height; ++py) {
                for (int px = 0; px < cell_width; ++px) {
                    if (kThreshold[(py & 1) * 2 + (px & 1)] < level)
                        fill_rect(dst, color, cell_x + px, cell_y + py, 1, 1);
                }
            }
            return true;
        } else if (cp == 0x2594) { // Upper 1/8
            const int h = std::max(1, cell_height / 8);
            rects[0] = {static_cast<int16_t>(cell_x), static_cast<int16_t>(cell_y),
                        static_cast<uint16_t>(cell_width), static_cast<uint16_t>(h)};
            num_rects = 1;
        } else if (cp == 0x2595) { // Right 1/8
            const int w = std::max(1, cell_width / 8);
            rects[0] = {static_cast<int16_t>(cell_x + cell_width - w), static_cast<int16_t>(cell_y),
                        static_cast<uint16_t>(w), static_cast<uint16_t>(cell_height)};
            num_rects = 1;
        } else if (cp >= 0x2596 && cp <= 0x259F) { // Quadrants
            const int hw = cell_width / 2;
            const int hh = cell_height / 2;
            const int rw = cell_width - hw;
            const int bh = cell_height - hh;

            const auto add_quad = [&](bool tl, bool tr, bool bl, bool br) noexcept {
                if (tl) rects[num_rects++] = {static_cast<int16_t>(cell_x), static_cast<int16_t>(cell_y), static_cast<uint16_t>(hw), static_cast<uint16_t>(hh)};
                if (tr) rects[num_rects++] = {static_cast<int16_t>(cell_x + hw), static_cast<int16_t>(cell_y), static_cast<uint16_t>(rw), static_cast<uint16_t>(hh)};
                if (bl) rects[num_rects++] = {static_cast<int16_t>(cell_x), static_cast<int16_t>(cell_y + hh), static_cast<uint16_t>(hw), static_cast<uint16_t>(bh)};
                if (br) rects[num_rects++] = {static_cast<int16_t>(cell_x + hw), static_cast<int16_t>(cell_y + hh), static_cast<uint16_t>(rw), static_cast<uint16_t>(bh)};
            };

            switch (cp) {
                case 0x2596: add_quad(false, false, true, false); break;
                case 0x2597: add_quad(false, false, false, true); break;
                case 0x2598: add_quad(true, false, false, false); break;
                case 0x2599: add_quad(true, false, true, true); break;
                case 0x259A: add_quad(true, false, false, true); break;
                case 0x259B: add_quad(true, true, true, false); break;
                case 0x259C: add_quad(true, true, false, true); break;
                case 0x259D: add_quad(false, true, false, false); break;
                case 0x259E: add_quad(false, true, true, false); break;
                case 0x259F: add_quad(false, true, true, true); break;
                default: break;
            }
        }

        if (num_rects > 0) {
            pixman_image_fill_rectangles(PIXMAN_OP_SRC, dst, &color, num_rects, rects);
            return true;
        }
        return false;
    }

    // Braille Patterns (0x2800 - 0x28FF)
    if (cp >= 0x2800 && cp <= 0x28FF) {
        const uint8_t pattern = static_cast<uint8_t>(cp - 0x2800);
        if (pattern == 0) return true; // Empty braille cell.

        const int dot_w = std::max(1, cell_width / 5);
        const int dot_h = std::max(1, cell_height / 7);

        const int col_x[2] = {
            cell_x + cell_width / 4 - dot_w / 2,
            cell_x + (3 * cell_width) / 4 - dot_w / 2,
        };
        const int row_y[4] = {
            cell_y + cell_height / 8 - dot_h / 2,
            cell_y + (3 * cell_height) / 8 - dot_h / 2,
            cell_y + (5 * cell_height) / 8 - dot_h / 2,
            cell_y + (7 * cell_height) / 8 - dot_h / 2,
        };

        // Standard Braille dot numbering: 1 4 / 2 5 / 3 6 / 7 8.
        const std::pair<int, int> dot_map[8] = {
            {0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}, {1, 2}, {0, 3}, {1, 3},
        };

        pixman_rectangle16_t rects[8];
        int num_rects = 0;
        for (int i = 0; i < 8; ++i) {
            if (pattern & (1 << i)) {
                const auto [cx, ry] = dot_map[i];
                rects[num_rects++] = {
                    static_cast<int16_t>(col_x[cx]),
                    static_cast<int16_t>(row_y[ry]),
                    static_cast<uint16_t>(dot_w),
                    static_cast<uint16_t>(dot_h)
                };
            }
        }
        if (num_rects > 0)
            pixman_image_fill_rectangles(PIXMAN_OP_SRC, dst, &color, num_rects, rects);
        return true;
    }

    // Box drawing characters (0x2500 - 0x257F)
    const BoxSides sides = decode_box_char(cp);
    const int light_thick = std::max(1, cell_height / 12);
    const int heavy_thick = std::max(2, light_thick * 2);

    if (sides.arcs != 0) {
        draw_arc(dst, color, sides.arcs, cell_x, cell_y,
                 cell_width, cell_height, light_thick);
        return true;
    }
    if (sides.diagonals != 0) {
        draw_diagonal(dst, color, sides.diagonals, cell_x, cell_y,
                      cell_width, cell_height, std::max(1, light_thick));
        return true;
    }
    if (sides.dashes != 0) {
        draw_dashes(dst, color, sides, cell_x, cell_y,
                    cell_width, cell_height, light_thick, heavy_thick);
        return true;
    }
    if (sides.up == None && sides.down == None && sides.left == None && sides.right == None)
        return false;

    const int dbl_thick = std::max(1, light_thick);
    const int dbl_gap = std::max(1, light_thick);
    const int mid_x = cell_x + cell_width / 2;
    const int mid_y = cell_y + cell_height / 2;

    if (cp == 0x2500) {
        if (fill_opaque_rect(dst, color, cell_x, mid_y - light_thick / 2,
                             cell_width, light_thick)) return true;
    } else if (cp == 0x2502) {
        if (fill_opaque_rect(dst, color, mid_x - light_thick / 2, cell_y,
                             light_thick, cell_height)) return true;
    }

    pixman_rectangle16_t rects[8];
    int num_rects = 0;

    const auto get_thickness = [light_thick, heavy_thick](LineType lt) noexcept {
        return lt == Heavy ? heavy_thick : light_thick;
    };

    // Up
    if (sides.up == Light || sides.up == Heavy) {
        const int t = get_thickness(sides.up);
        rects[num_rects++] = {static_cast<int16_t>(mid_x - t / 2), static_cast<int16_t>(cell_y), static_cast<uint16_t>(t), static_cast<uint16_t>(mid_y - cell_y + t / 2)};
    } else if (sides.up == Double) {
        const int x1 = mid_x - dbl_gap / 2 - dbl_thick;
        const int x2 = mid_x + dbl_gap / 2;
        rects[num_rects++] = {static_cast<int16_t>(x1), static_cast<int16_t>(cell_y), static_cast<uint16_t>(dbl_thick), static_cast<uint16_t>(mid_y - cell_y + dbl_thick)};
        rects[num_rects++] = {static_cast<int16_t>(x2), static_cast<int16_t>(cell_y), static_cast<uint16_t>(dbl_thick), static_cast<uint16_t>(mid_y - cell_y + dbl_thick)};
    }

    // Down
    if (sides.down == Light || sides.down == Heavy) {
        const int t = get_thickness(sides.down);
        rects[num_rects++] = {static_cast<int16_t>(mid_x - t / 2), static_cast<int16_t>(mid_y - t / 2), static_cast<uint16_t>(t), static_cast<uint16_t>(cell_y + cell_height - (mid_y - t / 2))};
    } else if (sides.down == Double) {
        const int x1 = mid_x - dbl_gap / 2 - dbl_thick;
        const int x2 = mid_x + dbl_gap / 2;
        rects[num_rects++] = {static_cast<int16_t>(x1), static_cast<int16_t>(mid_y), static_cast<uint16_t>(dbl_thick), static_cast<uint16_t>(cell_y + cell_height - mid_y)};
        rects[num_rects++] = {static_cast<int16_t>(x2), static_cast<int16_t>(mid_y), static_cast<uint16_t>(dbl_thick), static_cast<uint16_t>(cell_y + cell_height - mid_y)};
    }

    // Left
    if (sides.left == Light || sides.left == Heavy) {
        const int t = get_thickness(sides.left);
        rects[num_rects++] = {static_cast<int16_t>(cell_x), static_cast<int16_t>(mid_y - t / 2), static_cast<uint16_t>(mid_x - cell_x + t / 2), static_cast<uint16_t>(t)};
    } else if (sides.left == Double) {
        const int y1 = mid_y - dbl_gap / 2 - dbl_thick;
        const int y2 = mid_y + dbl_gap / 2;
        rects[num_rects++] = {static_cast<int16_t>(cell_x), static_cast<int16_t>(y1), static_cast<uint16_t>(mid_x - cell_x + dbl_thick), static_cast<uint16_t>(dbl_thick)};
        rects[num_rects++] = {static_cast<int16_t>(cell_x), static_cast<int16_t>(y2), static_cast<uint16_t>(mid_x - cell_x + dbl_thick), static_cast<uint16_t>(dbl_thick)};
    }

    // Right
    if (sides.right == Light || sides.right == Heavy) {
        const int t = get_thickness(sides.right);
        rects[num_rects++] = {static_cast<int16_t>(mid_x - t / 2), static_cast<int16_t>(mid_y - t / 2), static_cast<uint16_t>(cell_x + cell_width - (mid_x - t / 2)), static_cast<uint16_t>(t)};
    } else if (sides.right == Double) {
        const int y1 = mid_y - dbl_gap / 2 - dbl_thick;
        const int y2 = mid_y + dbl_gap / 2;
        rects[num_rects++] = {static_cast<int16_t>(mid_x), static_cast<int16_t>(y1), static_cast<uint16_t>(cell_x + cell_width - mid_x), static_cast<uint16_t>(dbl_thick)};
        rects[num_rects++] = {static_cast<int16_t>(mid_x), static_cast<int16_t>(y2), static_cast<uint16_t>(cell_x + cell_width - mid_x), static_cast<uint16_t>(dbl_thick)};
    }

    if (num_rects > 0) {
        pixman_image_fill_rectangles(PIXMAN_OP_SRC, dst, &color, num_rects, rects);
        return true;
    }
    return false;
}

} // namespace gritty::render
