#include "render/cpu_renderer.hpp"
#include <algorithm>

namespace squirt::render {

CpuRenderer::CpuRenderer(FontEngine& font_engine, RenderConfig config)
    : font_engine_(font_engine), config_(config) {}

pixman_color_t CpuRenderer::to_pixman_color(uint32_t argb) noexcept {
    uint8_t a = (argb >> 24) & 0xFF;
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >> 8) & 0xFF;
    uint8_t b = argb & 0xFF;

    return pixman_color_t{
        .red = static_cast<uint16_t>((r * 65535) / 255),
        .green = static_cast<uint16_t>((g * 65535) / 255),
        .blue = static_cast<uint16_t>((b * 65535) / 255),
        .alpha = static_cast<uint16_t>((a * 65535) / 255)
    };
}

uint32_t CpuRenderer::resolve_color(const core::Color& c, const core::TerminalEngine& engine, bool is_bg) const noexcept {
    if (c.is_default()) {
        return is_bg ? config_.default_bg : config_.default_fg;
    }
    if (c.is_indexed()) {
        uint32_t rgb = engine.palette_color(c.index());
        return 0xFF000000 | rgb;
    }
    if (c.is_rgb()) {
        return 0xFF000000 | (static_cast<uint32_t>(c.r()) << 16) | (static_cast<uint32_t>(c.g()) << 8) | c.b();
    }
    return is_bg ? config_.default_bg : config_.default_fg;
}

void CpuRenderer::render(
    core::TerminalEngine& engine,
    pixman_image_t* target,
    util::PixmanRegion* damage_out,
    bool full_redraw
) {
    if (!target) return;

    auto& grid = engine.grid();
    const auto& metrics = font_engine_.metrics();
    int cell_w = metrics.cell_width;
    int cell_h = metrics.cell_height;

    if (full_redraw) {
        grid.mark_all_dirty();
        if (damage_out) {
            damage_out->clear();
            damage_out->union_rect(0, 0, grid.cols() * cell_w, grid.rows() * cell_h);
        }
    }

    int rows = grid.rows();
    int cols = grid.cols();

    for (int y = 0; y < rows; ++y) {
        auto& row = grid.row(y);
        if (!row.dirty) continue;

        int start_col = std::max(0, row.damage_start);
        int end_col = (row.damage_end >= 0) ? std::min(cols - 1, row.damage_end) : (cols - 1);

        if (damage_out && !full_redraw) {
            damage_out->union_rect(start_col * cell_w, y * cell_h, (end_col - start_col + 1) * cell_w, cell_h);
        }

        for (int x = start_col; x <= end_col; ++x) {
            const auto& cell = row.cells[x];
            render_cell(cell, x, y, cell_w, cell_h, metrics, engine, target);
        }

        row.reset_damage();
    }

    if (config_.render_cursor && engine.cursor_visible()) {
        render_cursor(engine, cell_w, cell_h, target);
        if (damage_out) {
            damage_out->union_rect(engine.cursor_col() * cell_w, engine.cursor_row() * cell_h, cell_w, cell_h);
        }
    }
}

void CpuRenderer::render_cell(
    const core::Cell& cell,
    int col,
    int row,
    int cell_w,
    int cell_h,
    const FontMetrics& metrics,
    const core::TerminalEngine& engine,
    pixman_image_t* target
) {
    // If continuation cell (width == 0), background and glyph was already handled by leader cell
    if (cell.attrs.width == 0) return;

    int px = col * cell_w;
    int py = row * cell_h;
    int span_w = cell_w * (cell.attrs.width > 0 ? cell.attrs.width : 1);

    uint32_t fg_argb = resolve_color(cell.fg, engine, false);
    uint32_t bg_argb = resolve_color(cell.bg, engine, true);

    // SGR Inverse
    if (cell.attrs.inverse) {
        std::swap(fg_argb, bg_argb);
    }

    // SGR Dim
    if (cell.attrs.dim) {
        uint8_t a = (fg_argb >> 24) & 0xFF;
        uint8_t r = ((fg_argb >> 16) & 0xFF) * 2 / 3;
        uint8_t g = ((fg_argb >> 8) & 0xFF) * 2 / 3;
        uint8_t b = (fg_argb & 0xFF) * 2 / 3;
        fg_argb = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
    }

    // Fill background
    pixman_color_t bg_pcol = to_pixman_color(bg_argb);
    pixman_rectangle16_t bg_rect = {
        static_cast<int16_t>(px),
        static_cast<int16_t>(py),
        static_cast<uint16_t>(span_w),
        static_cast<uint16_t>(cell_h)
    };
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, target, &bg_pcol, 1, &bg_rect);

    if (cell.attrs.invisible) return;

    pixman_color_t fg_pcol = to_pixman_color(fg_argb);

    // Procedural box / block rendering
    char32_t cp = cell.codepoint;
    bool rendered_box = false;
    if (BoxDrawingRenderer::is_box_or_block(cp)) {
        rendered_box = BoxDrawingRenderer::render(target, cp, px, py, span_w, cell_h, fg_pcol);
    }

    if (!rendered_box && cp != ' ' && cp != 0) {
        // Render glyph via FontEngine
        const CachedGlyph* glyph = font_engine_.get_glyph(cp, cell.attrs.bold, cell.attrs.italic);
        if (glyph && glyph->mask.valid() && glyph->width > 0 && glyph->height > 0) {
            int gx = px + glyph->bearing_x;
            int gy = py + metrics.ascent - glyph->bearing_y;

            if (current_solid_color_ != fg_argb || !solid_src_.valid()) {
                solid_src_.reset(pixman_image_create_solid_fill(&fg_pcol));
                current_solid_color_ = fg_argb;
            }

            pixman_image_composite32(
                PIXMAN_OP_OVER,
                solid_src_.get(),
                glyph->mask.get(),
                target,
                0, 0,
                0, 0,
                gx, gy,
                glyph->width, glyph->height
            );
        }
    }

    // Underlines
    if (cell.attrs.underline_style != 0) {
        uint32_t ul_argb = cell.underline_color.is_default() ? fg_argb : resolve_color(cell.underline_color, engine, false);
        pixman_color_t ul_pcol = to_pixman_color(ul_argb);
        int ul_y = py + metrics.ascent + metrics.underline_pos;
        int ul_h = std::max(1, metrics.underline_thickness);

        auto ustyle = static_cast<core::UnderlineStyle>(cell.attrs.underline_style);
        switch (ustyle) {
            case core::UnderlineStyle::Single: {
                pixman_rectangle16_t r = {static_cast<int16_t>(px), static_cast<int16_t>(ul_y), static_cast<uint16_t>(span_w), static_cast<uint16_t>(ul_h)};
                pixman_image_fill_rectangles(PIXMAN_OP_SRC, target, &ul_pcol, 1, &r);
                break;
            }
            case core::UnderlineStyle::Double: {
                pixman_rectangle16_t r[2] = {
                    {static_cast<int16_t>(px), static_cast<int16_t>(ul_y - ul_h - 1), static_cast<uint16_t>(span_w), static_cast<uint16_t>(ul_h)},
                    {static_cast<int16_t>(px), static_cast<int16_t>(ul_y + 1), static_cast<uint16_t>(span_w), static_cast<uint16_t>(ul_h)}
                };
                pixman_image_fill_rectangles(PIXMAN_OP_SRC, target, &ul_pcol, 2, r);
                break;
            }
            case core::UnderlineStyle::Dotted: {
                int dot_w = std::max(1, cell_w / 4);
                pixman_rectangle16_t dots[8];
                int num_dots = 0;
                for (int dx = 0; dx < span_w && num_dots < 8; dx += dot_w * 2) {
                    dots[num_dots++] = {static_cast<int16_t>(px + dx), static_cast<int16_t>(ul_y), static_cast<uint16_t>(dot_w), static_cast<uint16_t>(ul_h)};
                }
                pixman_image_fill_rectangles(PIXMAN_OP_SRC, target, &ul_pcol, num_dots, dots);
                break;
            }
            case core::UnderlineStyle::Dashed: {
                int dash_w = std::max(2, span_w / 2);
                pixman_rectangle16_t dashes[2] = {
                    {static_cast<int16_t>(px), static_cast<int16_t>(ul_y), static_cast<uint16_t>(dash_w - 1), static_cast<uint16_t>(ul_h)},
                    {static_cast<int16_t>(px + dash_w + 1), static_cast<int16_t>(ul_y), static_cast<uint16_t>(span_w - dash_w - 1), static_cast<uint16_t>(ul_h)}
                };
                pixman_image_fill_rectangles(PIXMAN_OP_SRC, target, &ul_pcol, 2, dashes);
                break;
            }
            case core::UnderlineStyle::Curly: {
                // Wave zigzag
                pixman_rectangle16_t wave[4] = {
                    {static_cast<int16_t>(px), static_cast<int16_t>(ul_y + 1), static_cast<uint16_t>(span_w / 4), static_cast<uint16_t>(ul_h)},
                    {static_cast<int16_t>(px + span_w / 4), static_cast<int16_t>(ul_y - 1), static_cast<uint16_t>(span_w / 4), static_cast<uint16_t>(ul_h)},
                    {static_cast<int16_t>(px + span_w / 2), static_cast<int16_t>(ul_y + 1), static_cast<uint16_t>(span_w / 4), static_cast<uint16_t>(ul_h)},
                    {static_cast<int16_t>(px + (3 * span_w) / 4), static_cast<int16_t>(ul_y - 1), static_cast<uint16_t>(span_w - (3 * span_w) / 4), static_cast<uint16_t>(ul_h)}
                };
                pixman_image_fill_rectangles(PIXMAN_OP_SRC, target, &ul_pcol, 4, wave);
                break;
            }
            default:
                break;
        }
    }

    // Strikethrough
    if (cell.attrs.strikethrough) {
        int strike_y = py + cell_h / 2;
        int strike_h = std::max(1, metrics.underline_thickness);
        pixman_rectangle16_t r = {static_cast<int16_t>(px), static_cast<int16_t>(strike_y), static_cast<uint16_t>(span_w), static_cast<uint16_t>(strike_h)};
        pixman_image_fill_rectangles(PIXMAN_OP_SRC, target, &fg_pcol, 1, &r);
    }
}

void CpuRenderer::render_cursor(
    const core::TerminalEngine& engine,
    int cell_w,
    int cell_h,
    pixman_image_t* target
) {
    int cx = engine.cursor_col() * cell_w;
    int cy = engine.cursor_row() * cell_h;

    pixman_color_t cursor_pcol = to_pixman_color(config_.cursor_color);

    switch (engine.cursor_shape()) {
        case core::CursorShape::Block: {
            // Draw block with invert / XOR or alpha overlay
            pixman_rectangle16_t r = {static_cast<int16_t>(cx), static_cast<int16_t>(cy), static_cast<uint16_t>(cell_w), static_cast<uint16_t>(cell_h)};
            // Translucent or inverted
            pixman_image_fill_rectangles(PIXMAN_OP_XOR, target, &cursor_pcol, 1, &r);
            break;
        }
        case core::CursorShape::Beam: {
            int beam_w = std::max(2, cell_w / 4);
            pixman_rectangle16_t r = {static_cast<int16_t>(cx), static_cast<int16_t>(cy), static_cast<uint16_t>(beam_w), static_cast<uint16_t>(cell_h)};
            pixman_image_fill_rectangles(PIXMAN_OP_SRC, target, &cursor_pcol, 1, &r);
            break;
        }
        case core::CursorShape::Underline: {
            int under_h = std::max(2, cell_h / 6);
            pixman_rectangle16_t r = {static_cast<int16_t>(cx), static_cast<int16_t>(cy + cell_h - under_h), static_cast<uint16_t>(cell_w), static_cast<uint16_t>(under_h)};
            pixman_image_fill_rectangles(PIXMAN_OP_SRC, target, &cursor_pcol, 1, &r);
            break;
        }
    }
}

} // namespace squirt::render
