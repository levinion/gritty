#include "render/box_drawing.hpp"
#include <algorithm>
#include <array>

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
};

// Maps common Unicode box characters (0x2500 - 0x254F) to 4-directional line types
BoxSides decode_box_char(char32_t cp) noexcept {
    switch (cp) {
        // Horizontal & Vertical
        case 0x2500: return {.left = Light, .right = Light};
        case 0x2501: return {.left = Heavy, .right = Heavy};
        case 0x2502: return {.up = Light, .down = Light};
        case 0x2503: return {.up = Heavy, .down = Heavy};

        // Corners
        case 0x250C: return {.down = Light, .right = Light}; // ┌
        case 0x250D: return {.down = Light, .right = Heavy};
        case 0x250E: return {.down = Heavy, .right = Light};
        case 0x250F: return {.down = Heavy, .right = Heavy}; // ┏
        case 0x2510: return {.down = Light, .left = Light};  // ┐
        case 0x2511: return {.down = Light, .left = Heavy};
        case 0x2512: return {.down = Heavy, .left = Light};
        case 0x2513: return {.down = Heavy, .left = Heavy};  // ┓
        case 0x2514: return {.up = Light, .right = Light};   // └
        case 0x2515: return {.up = Light, .right = Heavy};
        case 0x2516: return {.up = Heavy, .right = Light};
        case 0x2517: return {.up = Heavy, .right = Heavy};   // ┗
        case 0x2518: return {.up = Light, .left = Light};    // ┘
        case 0x2519: return {.up = Light, .left = Heavy};
        case 0x251A: return {.up = Heavy, .left = Light};
        case 0x251B: return {.up = Heavy, .left = Heavy};    // ┛

        // T-junctions
        case 0x251C: return {.up = Light, .down = Light, .right = Light}; // ├
        case 0x251D: return {.up = Light, .down = Light, .right = Heavy};
        case 0x2520: return {.up = Heavy, .down = Heavy, .right = Light};
        case 0x2523: return {.up = Heavy, .down = Heavy, .right = Heavy}; // ┣
        case 0x2524: return {.up = Light, .down = Light, .left = Light};  // ┤
        case 0x2525: return {.up = Light, .down = Light, .left = Heavy};
        case 0x2528: return {.up = Heavy, .down = Heavy, .left = Light};
        case 0x252B: return {.up = Heavy, .down = Heavy, .left = Heavy};  // ┫
        case 0x252C: return {.down = Light, .left = Light, .right = Light}; // ┬
        case 0x252F: return {.down = Heavy, .left = Light, .right = Light};
        case 0x2530: return {.down = Light, .left = Heavy, .right = Heavy};
        case 0x2533: return {.down = Heavy, .left = Heavy, .right = Heavy}; // ┳
        case 0x2534: return {.up = Light, .left = Light, .right = Light};   // ┴
        case 0x2537: return {.up = Heavy, .left = Light, .right = Light};
        case 0x2538: return {.up = Light, .left = Heavy, .right = Heavy};
        case 0x253B: return {.up = Heavy, .left = Heavy, .right = Heavy};   // ┻

        // Crosses
        case 0x253C: return {.up = Light, .down = Light, .left = Light, .right = Light}; // ┼
        case 0x253F: return {.up = Heavy, .down = Heavy, .left = Light, .right = Light};
        case 0x2542: return {.up = Light, .down = Light, .left = Heavy, .right = Heavy};
        case 0x254B: return {.up = Heavy, .down = Heavy, .left = Heavy, .right = Heavy}; // ╋

        // Double lines
        case 0x2550: return {.left = Double, .right = Double}; // ═
        case 0x2551: return {.up = Double, .down = Double};   // ║
        case 0x2554: return {.down = Double, .right = Double}; // ╔
        case 0x2557: return {.down = Double, .left = Double};  // ╗
        case 0x255A: return {.up = Double, .right = Double};   // ╚
        case 0x255D: return {.up = Double, .left = Double};    // ╝
        case 0x2560: return {.up = Double, .down = Double, .right = Double}; // ╠
        case 0x2563: return {.up = Double, .down = Double, .left = Double};  // ╣
        case 0x2566: return {.down = Double, .left = Double, .right = Double}; // ╦
        case 0x2569: return {.up = Double, .left = Double, .right = Double};   // ╩
        case 0x256C: return {.up = Double, .down = Double, .left = Double, .right = Double}; // ╬

        // Half lines
        case 0x2574: return {.left = Light};
        case 0x2575: return {.up = Light};
        case 0x2576: return {.right = Light};
        case 0x2577: return {.down = Light};
        case 0x2578: return {.left = Heavy};
        case 0x2579: return {.up = Heavy};
        case 0x257A: return {.right = Heavy};
        case 0x257B: return {.down = Heavy};

        default:
            return {};
    }
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

    // Block elements (0x2580 - 0x259F)
    if (cp >= 0x2580 && cp <= 0x259F) {
        pixman_rectangle16_t rects[4];
        int num_rects = 0;

        if (cp == 0x2588) { // Full block
            rects[0] = {static_cast<int16_t>(cell_x), static_cast<int16_t>(cell_y),
                        static_cast<uint16_t>(cell_width), static_cast<uint16_t>(cell_height)};
            num_rects = 1;
        } else if (cp == 0x2580) { // Upper half
            rects[0] = {static_cast<int16_t>(cell_x), static_cast<int16_t>(cell_y),
                        static_cast<uint16_t>(cell_width), static_cast<uint16_t>(cell_height / 2)};
            num_rects = 1;
        } else if (cp >= 0x2581 && cp <= 0x2587) { // Lower 1/8 to 7/8
            int fraction = cp - 0x2580;
            int h = (cell_height * fraction) / 8;
            rects[0] = {static_cast<int16_t>(cell_x), static_cast<int16_t>(cell_y + cell_height - h),
                        static_cast<uint16_t>(cell_width), static_cast<uint16_t>(h)};
            num_rects = 1;
        } else if (cp >= 0x2589 && cp <= 0x258F) { // Left 7/8 to 1/8
            int fraction = 8 - (cp - 0x2588);
            int w = (cell_width * fraction) / 8;
            rects[0] = {static_cast<int16_t>(cell_x), static_cast<int16_t>(cell_y),
                        static_cast<uint16_t>(w), static_cast<uint16_t>(cell_height)};
            num_rects = 1;
        } else if (cp == 0x2590) { // Right half
            int w = cell_width - cell_width / 2;
            rects[0] = {static_cast<int16_t>(cell_x + cell_width / 2), static_cast<int16_t>(cell_y),
                        static_cast<uint16_t>(w), static_cast<uint16_t>(cell_height)};
            num_rects = 1;
        } else if (cp >= 0x2596 && cp <= 0x259F) { // Quadrants
            int hw = cell_width / 2;
            int hh = cell_height / 2;
            int rw = cell_width - hw;
            int bh = cell_height - hh;

            auto add_quad = [&](bool tl, bool tr, bool bl, bool br) {
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
    }

    // Braille Patterns (0x2800 - 0x28FF)
    if (cp >= 0x2800 && cp <= 0x28FF) {
        uint8_t pattern = static_cast<uint8_t>(cp - 0x2800);
        if (pattern == 0) return true; // Empty braille cell

        int dot_w = std::max(1, cell_width / 5);
        int dot_h = std::max(1, cell_height / 7);

        int col_x[2] = { cell_x + cell_width / 4 - dot_w / 2, cell_x + (3 * cell_width) / 4 - dot_w / 2 };
        int row_y[4] = {
            cell_y + cell_height / 8 - dot_h / 2,
            cell_y + (3 * cell_height) / 8 - dot_h / 2,
            cell_y + (5 * cell_height) / 8 - dot_h / 2,
            cell_y + (7 * cell_height) / 8 - dot_h / 2
        };

        // Standard Braille dot numbering:
        // 1 4
        // 2 5
        // 3 6
        // 7 8
        std::pair<int, int> dot_map[8] = {
            {0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}, {1, 2}, {0, 3}, {1, 3}
        };

        pixman_rectangle16_t rects[8];
        int num_rects = 0;
        for (int i = 0; i < 8; ++i) {
            if (pattern & (1 << i)) {
                auto [cx, ry] = dot_map[i];
                rects[num_rects++] = {
                    static_cast<int16_t>(col_x[cx]),
                    static_cast<int16_t>(row_y[ry]),
                    static_cast<uint16_t>(dot_w),
                    static_cast<uint16_t>(dot_h)
                };
            }
        }
        if (num_rects > 0) {
            pixman_image_fill_rectangles(PIXMAN_OP_SRC, dst, &color, num_rects, rects);
        }
        return true;
    }

    // Box drawing characters (0x2500 - 0x257F)
    BoxSides sides = decode_box_char(cp);
    if (sides.up == None && sides.down == None && sides.left == None && sides.right == None) {
        return false;
    }

    int light_thick = std::max(1, cell_height / 12);
    int heavy_thick = std::max(2, light_thick * 2);
    int dbl_thick = std::max(1, light_thick);
    int dbl_gap = std::max(1, light_thick);

    int mid_x = cell_x + cell_width / 2;
    int mid_y = cell_y + cell_height / 2;

    pixman_rectangle16_t rects[8];
    int num_rects = 0;

    auto get_thickness = [&](LineType lt) {
        return (lt == Heavy) ? heavy_thick : light_thick;
    };

    // Up
    if (sides.up == Light || sides.up == Heavy) {
        int t = get_thickness(sides.up);
        rects[num_rects++] = {static_cast<int16_t>(mid_x - t / 2), static_cast<int16_t>(cell_y), static_cast<uint16_t>(t), static_cast<uint16_t>(mid_y - cell_y + t / 2)};
    } else if (sides.up == Double) {
        int x1 = mid_x - dbl_gap / 2 - dbl_thick;
        int x2 = mid_x + dbl_gap / 2;
        rects[num_rects++] = {static_cast<int16_t>(x1), static_cast<int16_t>(cell_y), static_cast<uint16_t>(dbl_thick), static_cast<uint16_t>(mid_y - cell_y + dbl_thick)};
        rects[num_rects++] = {static_cast<int16_t>(x2), static_cast<int16_t>(cell_y), static_cast<uint16_t>(dbl_thick), static_cast<uint16_t>(mid_y - cell_y + dbl_thick)};
    }

    // Down
    if (sides.down == Light || sides.down == Heavy) {
        int t = get_thickness(sides.down);
        rects[num_rects++] = {static_cast<int16_t>(mid_x - t / 2), static_cast<int16_t>(mid_y - t / 2), static_cast<uint16_t>(t), static_cast<uint16_t>(cell_y + cell_height - (mid_y - t / 2))};
    } else if (sides.down == Double) {
        int x1 = mid_x - dbl_gap / 2 - dbl_thick;
        int x2 = mid_x + dbl_gap / 2;
        rects[num_rects++] = {static_cast<int16_t>(x1), static_cast<int16_t>(mid_y), static_cast<uint16_t>(dbl_thick), static_cast<uint16_t>(cell_y + cell_height - mid_y)};
        rects[num_rects++] = {static_cast<int16_t>(x2), static_cast<int16_t>(mid_y), static_cast<uint16_t>(dbl_thick), static_cast<uint16_t>(cell_y + cell_height - mid_y)};
    }

    // Left
    if (sides.left == Light || sides.left == Heavy) {
        int t = get_thickness(sides.left);
        rects[num_rects++] = {static_cast<int16_t>(cell_x), static_cast<int16_t>(mid_y - t / 2), static_cast<uint16_t>(mid_x - cell_x + t / 2), static_cast<uint16_t>(t)};
    } else if (sides.left == Double) {
        int y1 = mid_y - dbl_gap / 2 - dbl_thick;
        int y2 = mid_y + dbl_gap / 2;
        rects[num_rects++] = {static_cast<int16_t>(cell_x), static_cast<int16_t>(y1), static_cast<uint16_t>(mid_x - cell_x + dbl_thick), static_cast<uint16_t>(dbl_thick)};
        rects[num_rects++] = {static_cast<int16_t>(cell_x), static_cast<int16_t>(y2), static_cast<uint16_t>(mid_x - cell_x + dbl_thick), static_cast<uint16_t>(dbl_thick)};
    }

    // Right
    if (sides.right == Light || sides.right == Heavy) {
        int t = get_thickness(sides.right);
        rects[num_rects++] = {static_cast<int16_t>(mid_x - t / 2), static_cast<int16_t>(mid_y - t / 2), static_cast<uint16_t>(cell_x + cell_width - (mid_x - t / 2)), static_cast<uint16_t>(t)};
    } else if (sides.right == Double) {
        int y1 = mid_y - dbl_gap / 2 - dbl_thick;
        int y2 = mid_y + dbl_gap / 2;
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
