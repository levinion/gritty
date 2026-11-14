#include "core/overlays.hpp"

namespace squirt::core {

bool SearchOverlay::handle_key(const input::KeyEvent& ev) {
    if (!active_) return false;

    if (ev.key == input::KeyCode::Escape) {
        deactivate();
        return true;
    }

    if (ev.key == input::KeyCode::Backspace) {
        if (!query_.empty()) query_.pop_back();
        return true;
    }

    if (ev.key == input::KeyCode::Return) {
        // Jump / confirm
        return true;
    }

    if (!ev.text.empty() && !ev.mods.ctrl && !ev.mods.alt) {
        query_.append(ev.text);
        return true;
    }

    return true;
}

void SearchOverlay::render(
    pixman_image_t* dst,
    render::FontEngine& font_engine,
    int screen_cols,
    int screen_rows
) {
    if (!active_ || !dst) return;

    const auto& m = font_engine.metrics();
    int cell_w = m.cell_width;
    int cell_h = m.cell_height;

    // Draw search bar at bottom row
    int bar_y = (screen_rows - 1) * cell_h;
    int bar_w = screen_cols * cell_w;

    pixman_color_t bg_col = {0x2222, 0x3333, 0x4444, 0xffff};
    pixman_rectangle16_t rect = {0, static_cast<int16_t>(bar_y), static_cast<uint16_t>(bar_w), static_cast<uint16_t>(cell_h)};
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, dst, &bg_col, 1, &rect);

    std::string prompt = "Search: " + query_ + "_";
    pixman_color_t fg_col = {0xffff, 0xffff, 0x8888, 0xffff};
    pixman_image_t* solid_fg = pixman_image_create_solid_fill(&fg_col);

    for (size_t i = 0; i < prompt.size() && static_cast<int>(i) < screen_cols; ++i) {
        const auto* glyph = font_engine.get_glyph(prompt[i], true, false);
        if (glyph && glyph->mask.valid()) {
            int gx = static_cast<int>(i) * cell_w + glyph->bearing_x;
            int gy = bar_y + m.ascent - glyph->bearing_y;
            pixman_image_composite32(
                PIXMAN_OP_OVER, solid_fg, glyph->mask.get(), dst,
                0, 0, 0, 0, gx, gy, glyph->width, glyph->height);
        }
    }
    pixman_image_unref(solid_fg);
}

void UrlOverlay::add_hint(int col, int row, std::string hint, std::string url) {
    hints_.push_back(UrlHint{
        .col = col,
        .row = row,
        .hint = std::move(hint),
        .url = std::move(url)
    });
}

bool UrlOverlay::handle_key(const input::KeyEvent& ev) {
    if (!active_) return false;

    if (ev.key == input::KeyCode::Escape) {
        deactivate();
        return true;
    }

    if (!ev.text.empty() && !ev.mods.ctrl && !ev.mods.alt) {
        input_tag_.append(ev.text);
        for (const auto& h : hints_) {
            if (h.hint == input_tag_) {
                // Matched URL!
                deactivate();
                return true;
            }
        }
        return true;
    }

    return true;
}

void UrlOverlay::render(
    pixman_image_t* dst,
    render::FontEngine& font_engine,
    int screen_cols,
    int screen_rows
) {
    (void)screen_cols;
    (void)screen_rows;
    if (!active_ || !dst) return;

    const auto& m = font_engine.metrics();
    int cell_w = m.cell_width;
    int cell_h = m.cell_height;

    pixman_color_t tag_bg = {0xeeff, 0x2222, 0x2222, 0xffff}; // Red tag
    pixman_color_t tag_fg = {0xffff, 0xffff, 0xffff, 0xffff};
    pixman_image_t* solid_fg = pixman_image_create_solid_fill(&tag_fg);

    for (const auto& h : hints_) {
        int px = h.col * cell_w;
        int py = h.row * cell_h;
        int tag_w = static_cast<int>(h.hint.size()) * cell_w;

        pixman_rectangle16_t r = {static_cast<int16_t>(px), static_cast<int16_t>(py), static_cast<uint16_t>(tag_w), static_cast<uint16_t>(cell_h)};
        pixman_image_fill_rectangles(PIXMAN_OP_SRC, dst, &tag_bg, 1, &r);

        for (size_t i = 0; i < h.hint.size(); ++i) {
            const auto* glyph = font_engine.get_glyph(h.hint[i], true, false);
            if (glyph && glyph->mask.valid()) {
                int gx = px + static_cast<int>(i) * cell_w + glyph->bearing_x;
                int gy = py + m.ascent - glyph->bearing_y;
                pixman_image_composite32(
                    PIXMAN_OP_OVER, solid_fg, glyph->mask.get(), dst,
                    0, 0, 0, 0, gx, gy, glyph->width, glyph->height);
            }
        }
    }
    pixman_image_unref(solid_fg);
}

} // namespace squirt::core
