#pragma once

#include <cstdint>
#include <pixman.h>

namespace gritty::render {

class BoxDrawingRenderer final {
public:
    static bool is_box_or_block(char32_t cp) noexcept {
        return (cp >= 0x2500 && cp <= 0x259F) || (cp >= 0x2800 && cp <= 0x28FF);
    }

    // Render procedural box or block element into dst image at (cell_x, cell_y)
    // Returns true if character was handled, false otherwise.
    static bool render(
        pixman_image_t* dst,
        char32_t cp,
        int cell_x,
        int cell_y,
        int cell_width,
        int cell_height,
        pixman_color_t color
    ) noexcept;
};

} // namespace gritty::render
