#include "render/cpu_renderer.hpp"
#include "core/kitty_placeholder.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>

namespace gritty::render {
namespace {
uint32_t next_atlas_generation() noexcept {
    static std::atomic<uint32_t> generation{1};
    return generation.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

CpuRenderer::CpuRenderer(FontEngine& font_engine, RenderConfig config)
    : font_engine_(font_engine), config_(config), atlas_generation_(next_atlas_generation()) {}

pixman_color_t CpuRenderer::to_pixman_color(uint32_t argb) noexcept {
    uint8_t a = (argb >> 24) & 0xFF;
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >> 8) & 0xFF;
    uint8_t b = argb & 0xFF;

    return pixman_color_t{
        // Pixman and CGImage premultiplied-first both require RGB <= alpha.
        // Match foot's color_hex_to_pixman_with_alpha(), not straight RGBA.
        .red = static_cast<uint16_t>((static_cast<uint32_t>(r) * 257 * a) / 255),
        .green = static_cast<uint16_t>((static_cast<uint32_t>(g) * 257 * a) / 255),
        .blue = static_cast<uint16_t>((static_cast<uint32_t>(b) * 257 * a) / 255),
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
    if (!target || engine.synchronized_updates()) return;
    if (damage_out) damage_out->clear();
    full_redraw |= config_changed_;
    config_changed_ = false;
    // LCD masks assume a known opaque backdrop. Keep the user's requested AA
    // mode, but use grayscale while the window can expose the desktop behind it.
    font_engine_.set_subpixel_allowed((config_.default_bg >> 24) == 255);

    auto& grid = engine.grid();
    const auto& metrics = font_engine_.metrics();
    int cell_w = metrics.cell_width;
    int cell_h = metrics.cell_height;
    const int target_w = pixman_image_get_width(target);
    const int target_h = pixman_image_get_height(target);

    // Move already-rendered rows before drawing this frame's damage. A pending
    // shift only describes whole-width row motion, so every moved pixel keeps
    // its exact position relative to the new grid; rows cleared by the scroll
    // already carry full damage and are repainted below.
    if (full_redraw) grid.clear_pending_scroll();
    else apply_pending_scrolls(engine, grid, target, cell_w, cell_h, damage_out);

    // Restore the cells underneath both the old and new cursor, even if
    // terminal content did not change (cursor motion/visibility-only updates).
    auto damage_cursor = [&](int col, int row) {
        if (col < 0 || col >= grid.cols() || row < 0 || row >= grid.rows()) return;
        if (grid.cell(col, row).attrs.width == 0 && col > 0) --col;
        grid.row(row).mark_dirty(col);
        if (grid.cell(col, row).attrs.width == 2 && col + 1 < grid.cols())
            grid.row(row).mark_dirty(col + 1);
    };
    damage_cursor(previous_cursor_col_, previous_cursor_row_);
    const bool draw_cursor = config_.render_cursor && engine.cursor_visible();
    if (draw_cursor) damage_cursor(engine.cursor_col(), engine.cursor_row());

    if (full_redraw) {
        ++stats_.full_redraws;
        const auto bg = to_pixman_color(config_.default_bg);
        const pixman_rectangle16_t rect{0, 0,
            static_cast<uint16_t>(target_w), static_cast<uint16_t>(target_h)};
        pixman_image_fill_rectangles(PIXMAN_OP_SRC, target, &bg, 1, &rect);
        grid.mark_all_dirty();
        if (damage_out) {
            damage_out->clear();
            damage_out->union_rect(0, 0, target_w, target_h);
        }
    }

    int rows = grid.rows();
    int cols = grid.cols();

    // A window's pixel size need not be an exact multiple of the cell size.
    // Rows only cover cols*cell_w x rows*cell_h; the right/bottom gutters must
    // be repainted and reported as damage on every frame. Otherwise a reused
    // IOSurface carries stale pixels there when the display buffer rotates.
    if (cell_w > 0 && cell_h > 0) {
        const int grid_w = std::min(target_w, cols * cell_w);
        const int grid_h = std::min(target_h, rows * cell_h);
        if (target_w > grid_w || target_h > grid_h) {
            const auto bg = to_pixman_color(config_.default_bg);
            pixman_rectangle16_t gutters[2];
            int count = 0;
            if (target_w > grid_w) {
                gutters[count++] = {static_cast<int16_t>(grid_w), 0,
                    static_cast<uint16_t>(target_w - grid_w), static_cast<uint16_t>(target_h)};
            }
            if (target_h > grid_h) {
                gutters[count++] = {0, static_cast<int16_t>(grid_h),
                    static_cast<uint16_t>(target_w), static_cast<uint16_t>(target_h - grid_h)};
            }
            pixman_image_fill_rectangles(PIXMAN_OP_SRC, target, &bg, count, gutters);
            if (damage_out && !full_redraw) {
                for (int i = 0; i < count; ++i) {
                    damage_out->union_rect(gutters[i].x, gutters[i].y,
                        gutters[i].width, gutters[i].height);
                }
            }
        }
    }

    auto damage_range = [&](int y, int& start_col, int& end_col) {
        auto& row = grid.row(y);
        start_col = std::max(0, row.damage_start);
        end_col = (row.damage_end >= 0) ? std::min(cols - 1, row.damage_end) : (cols - 1);
        if (row.cells[start_col].attrs.width == 0 && start_col > 0) --start_col;
        if (row.cells[end_col].attrs.width == 2 && end_col + 1 < cols) ++end_col;
    };

    // Placeholder images use OVER compositing. Restrict them to cells whose
    // background was rebuilt this frame: recompositing a clean row on every
    // cursor blink would compound alpha and visibly darken transparent PNGs.
    util::PixmanRegion placeholder_clip;
    if (full_redraw) {
        placeholder_clip.union_rect(0, 0,
            static_cast<unsigned>(target_w), static_cast<unsigned>(target_h));
    } else {
        for (int y = 0; y < rows; ++y) {
            if (!grid.row(y).dirty) continue;
            int start_col = 0, end_col = 0;
            damage_range(y, start_col, end_col);
            placeholder_clip.union_rect(start_col * cell_w, y * cell_h,
                static_cast<unsigned>((end_col - start_col + 1) * cell_w),
                static_cast<unsigned>(cell_h));
        }
    }

    // Backgrounds first for every dirty row. Virtual placeholder images must
    // sit above these fills but below the glyphs, so their aggregated pass runs
    // between the background and foreground passes.
    for (int y = 0; y < rows; ++y) {
        if (!grid.row(y).dirty) continue;
        int start_col = 0, end_col = 0;
        damage_range(y, start_col, end_col);
        if (damage_out && !full_redraw) {
            damage_out->union_rect(start_col * cell_w, y * cell_h,
                (end_col - start_col + 1) * cell_w, cell_h);
        }
        draw_row_backgrounds(grid, y, start_col, end_col, cell_w, cell_h, engine, target);
    }

    if (!engine.images().empty())
        draw_images(engine, target, cell_w, cell_h, true, -1, damage_out, placeholder_clip.get());

    draw_placeholders(engine, target, true, placeholder_clip.get(), cell_w, cell_h, metrics, damage_out);

    for (int y = 0; y < rows; ++y) {
        auto& row = grid.row(y);
        if (!row.dirty) continue;
        int start_col = 0, end_col = 0;
        damage_range(y, start_col, end_col);
        for (int x = start_col; x <= end_col; ++x) {
            const auto& cell = row.cells[x];
            const core::CellExtra extra = grid.get_cell_extra(cell.extra_idx);
            if (cell.attrs.width == 0 || cell.attrs.invisible) continue;
            const char32_t cp = cell.codepoint;
            const bool has_glyph = cp != ' ' && cp != 0;
            if (!has_glyph && cell.attrs.underline_style == 0 && !cell.attrs.strikethrough) continue;

            const bool plain_glyph = glyph_batching_ && has_glyph && cell.extra_idx == 0 &&
                cp != 0x10EEEE &&
                cell.attrs.underline_style == 0 && !cell.attrs.strikethrough &&
                !BoxDrawingRenderer::is_box_or_block(cp);
            if (plain_glyph) {
                const CachedGlyph* glyph = font_engine_.get_glyph(cp, cell.attrs.bold, cell.attrs.italic);
                if (glyph && glyph->mask.valid() && glyph->width > 0 && glyph->height > 0 &&
                    pixman_image_get_format(glyph->mask.get()) == PIXMAN_a8) {
                    const uint32_t fg = resolve_foreground(cell, engine);
                    if (glyph_batch_.empty() || fg != glyph_batch_color_) {
                        flush_glyph_batch(target);
                        glyph_batch_color_ = fg;
                        glyph_batch_x0_ = glyph_batch_y0_ = std::numeric_limits<int>::max();
                        glyph_batch_x1_ = glyph_batch_y1_ = std::numeric_limits<int>::min();
                    }
                    if (const void* handle = glyph_atlas_handle(*glyph)) {
                        const int gx = x * cell_w + glyph->bearing_x;
                        const int gy = y * cell_h + metrics.ascent - glyph->bearing_y;
                        glyph_batch_.push_back(BatchGlyph{gx, gy, glyph->width, glyph->height,
                            handle, glyph->mask.get()});
                        glyph_batch_x0_ = std::min(glyph_batch_x0_, gx);
                        glyph_batch_y0_ = std::min(glyph_batch_y0_, gy);
                        glyph_batch_x1_ = std::max(glyph_batch_x1_, gx + glyph->width);
                        glyph_batch_y1_ = std::max(glyph_batch_y1_, gy + glyph->height);
                        continue;
                    }
                }
            }

            flush_glyph_batch(target);
            render_cell_foreground(cell, extra, x, y, cell_w, cell_h, metrics, engine, target);
        }
        flush_glyph_batch(target);
        row.reset_damage();
    }

    draw_images(engine, target, cell_w, cell_h, false, -1, damage_out, placeholder_clip.get());
    draw_placeholders(engine, target, false, placeholder_clip.get(), cell_w, cell_h, metrics, damage_out);

    previous_cursor_col_ = draw_cursor ? engine.cursor_col() : -1;
    previous_cursor_row_ = draw_cursor ? engine.cursor_row() : -1;
    if (draw_cursor) {
        render_cursor(engine, cell_w, cell_h, target);
        if (damage_out) {
            damage_out->union_rect(engine.cursor_col() * cell_w, engine.cursor_row() * cell_h, cell_w, cell_h);
        }
    }
}

void CpuRenderer::apply_pending_scrolls(
    const core::TerminalEngine& engine,
    core::ScreenGrid& grid,
    pixman_image_t* target,
    int cell_w,
    int cell_h,
    util::PixmanRegion* damage_out
) {
    grid.take_pending_scrolls(scroll_scratch_);
    if (scroll_scratch_.empty()) return;
    if (grid.cols() <= 0 || cell_h <= 0) { scroll_scratch_.clear(); return; }

    auto repaint_region = [&](const core::ScrollOp& op) {
        for (int y = std::max(0, op.top); y <= std::min(grid.rows() - 1, op.bottom); ++y)
            grid.row(y).mark_all_dirty(grid.cols());
    };
    // All-or-nothing: applying some shifts and repainting others could move
    // pixels that the repaint has not written yet. Validate every shift first;
    // any failure repaints the union of all queued regions instead.
    auto repaint_all = [&] {
        for (const auto& op : scroll_scratch_) repaint_region(op);
        ++stats_.scroll_repaints;
        scroll_scratch_.clear();
    };

    // Pixel memmove cannot move Kitty/Sixel layers consistently: an image's
    // bitmap is not part of the grid rows and virtual placeholders are scaled
    // from their full rectangle. Repaint instead of risking torn overlays.
    if (!engine.images().empty() || !engine.kitty_graphics().virtual_placements().empty()) {
        bool intersects = !engine.kitty_graphics().virtual_placements().empty();
        for (const auto& image : engine.images()) {
            if (image.alt_screen != engine.is_alt_screen()) continue;
            for (const auto& op : scroll_scratch_) {
                if (image.row <= op.bottom && image.row + image.rows - 1 >= op.top) {
                    intersects = true;
                    break;
                }
            }
            if (intersects) break;
        }
        if (intersects) {
            repaint_all();
            return;
        }
    }

    uint8_t* base = reinterpret_cast<uint8_t*>(pixman_image_get_data(target));
    const int stride = pixman_image_get_stride(target);
    const int target_height = pixman_image_get_height(target);
    const int target_width = pixman_image_get_width(target);
    if (scroll_mode_ == ScrollMode::Repaint || !base || stride <= 0 ||
        grid.cols() * cell_w > target_width) {
        repaint_all();
        return;
    }

    struct Move {
        const core::ScrollOp* op;
        int shift, moved_rows, region_h, moved_px;
    };
    static thread_local std::vector<Move> moves;
    moves.clear();

    for (const auto& op : scroll_scratch_) {
        const int shift = std::abs(op.rows);
        const int moved_rows = op.region_rows() - shift;
        if (!op.valid() || op.top < 0 || op.bottom >= grid.rows() ||
            shift <= 0 || moved_rows <= 0 || op.region_rows() * cell_h > target_height) {
            repaint_all();
            return;
        }
        const int first_dst = op.rows > 0 ? op.top : op.top + shift;
        bool destinations_covered = true;
        for (int y = first_dst; y < first_dst + moved_rows && destinations_covered; ++y) {
            const auto& row = grid.row(y);
            destinations_covered = row.dirty && row.damage_start == 0 && row.damage_end >= grid.cols() - 1;
        }
        if (destinations_covered) {
            repaint_all();
            return;
        }
        if (scroll_mode_ == ScrollMode::Adaptive) {
            long long glyph_cells = 0;
            for (int y = first_dst; y < first_dst + moved_rows; ++y) {
                const auto& row = grid.row(y);
                for (int x = 0; x < grid.cols(); ++x) {
                    const auto& cell = row.cells[x];
                    if (cell.attrs.width == 0) continue;
                    if (cell.codepoint != U' ' && cell.codepoint != 0) ++glyph_cells;
                }
            }
            if (glyph_cells * 5 <= static_cast<long long>(moved_rows) * grid.cols()) {
                repaint_all();
                return;
            }
        }
        moves.push_back({&op, shift, moved_rows, op.region_rows() * cell_h, moved_rows * cell_h});
    }

    for (const auto& move : moves) {
        const core::ScrollOp& op = *move.op;
        const size_t bytes = static_cast<size_t>(move.moved_px) * static_cast<size_t>(stride);
        if (op.rows > 0) {
            std::memmove(base + static_cast<size_t>(op.top * cell_h) * stride,
                         base + static_cast<size_t>((op.top + move.shift) * cell_h) * stride,
                         bytes);
        } else {
            std::memmove(base + static_cast<size_t>((op.top + move.shift) * cell_h) * stride,
                         base + static_cast<size_t>(op.top * cell_h) * stride,
                         bytes);
        }

        // The cursor's previous pixels moved with its row; repaint the row that
        // now carries them before the new cursor position is drawn.
        if (previous_cursor_row_ >= 0 && previous_cursor_row_ >= op.top && previous_cursor_row_ <= op.bottom) {
            const int shifted = previous_cursor_row_ - op.rows;
            if (shifted >= op.top && shifted <= op.bottom) {
                grid.row(shifted).mark_all_dirty(grid.cols());
                previous_cursor_row_ = shifted;
            } else {
                previous_cursor_row_ = -1;
            }
        }

        if (damage_out) damage_out->union_rect(0, op.top * cell_h, target_width, move.region_h);
        ++stats_.scroll_moves;
        stats_.scrolled_rows += move.shift;
    }
    scroll_scratch_.clear();
}

uint32_t CpuRenderer::effective_background(
    const core::Cell& cell, const core::TerminalEngine& engine
) const noexcept {
    // SGR inverse swaps fg/bg and, as in foot, makes the swapped background
    // opaque; dim only affects the foreground.
    const uint32_t bg_argb = resolve_color(cell.attrs.inverse ? cell.fg : cell.bg, engine, !cell.attrs.inverse);
    return cell.attrs.inverse ? (bg_argb | 0xff000000) : bg_argb;
}

void CpuRenderer::draw_row_backgrounds(
    const core::ScreenGrid& grid,
    int row,
    int start_col,
    int end_col,
    int cell_w,
    int cell_h,
    const core::TerminalEngine& engine,
    pixman_image_t* target
) {
    const auto& cells = grid.row(row).cells;
    int run_px = 0, run_width = 0;
    uint32_t run_color = 0;
    bool run_open = false;

    auto flush = [&] {
        if (!run_open) return;
        pixman_color_t pcol = to_pixman_color(run_color);
        pixman_rectangle16_t rect = {
            static_cast<int16_t>(run_px), static_cast<int16_t>(row * cell_h),
            static_cast<uint16_t>(run_width), static_cast<uint16_t>(cell_h)
        };
        pixman_image_fill_rectangles(PIXMAN_OP_SRC, target, &pcol, 1, &rect);
        run_open = false;
    };

    for (int x = start_col; x <= end_col; ++x) {
        const auto& cell = cells[x];
        if (cell.attrs.width == 0) continue; // Span of the preceding leader cell.
        const int px = x * cell_w;
        const int span_w = cell_w * (cell.attrs.width > 1 ? cell.attrs.width : 1);
        const uint32_t bg = effective_background(cell, engine);
        if (run_open && bg == run_color && px == run_px + run_width) {
            run_width += span_w;
            continue;
        }
        flush();
        run_open = true;
        run_color = bg;
        run_px = px;
        run_width = span_w;
    }
    flush();
}

CpuRenderer::~CpuRenderer() {
    if (glyph_atlas_) pixman_glyph_cache_destroy(glyph_atlas_);
}

uint32_t CpuRenderer::resolve_foreground(
    const core::Cell& cell, const core::TerminalEngine& engine
) const noexcept {
    uint32_t fg_argb = cell.attrs.inverse
        ? (resolve_color(cell.bg, engine, true) | 0xff000000)
        : resolve_color(cell.fg, engine, false);
    if (cell.attrs.dim) {
        const uint8_t a = (fg_argb >> 24) & 0xFF;
        const uint8_t r = ((fg_argb >> 16) & 0xFF) * 2 / 3;
        const uint8_t g = ((fg_argb >> 8) & 0xFF) * 2 / 3;
        const uint8_t b = (fg_argb & 0xFF) * 2 / 3;
        fg_argb = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
                  (static_cast<uint32_t>(g) << 8) | b;
    }
    return fg_argb;
}

pixman_image_t* CpuRenderer::solid_fill(uint32_t argb) {
    for (auto& entry : solid_cache_)
        if (entry.image.valid() && entry.argb == argb) return entry.image.get();
    auto& entry = solid_cache_[solid_next_];
    solid_next_ = (solid_next_ + 1) % solid_cache_.size();
    const pixman_color_t color = to_pixman_color(argb);
    entry.argb = argb;
    entry.image.reset(pixman_image_create_solid_fill(&color));
    return entry.image.get();
}

const void* CpuRenderer::glyph_atlas_handle(const CachedGlyph& glyph) {
    auto& mutable_glyph = const_cast<CachedGlyph&>(glyph);
    pixman_image_t* mask = glyph.mask.get();
    if (!mask) return nullptr;
    if (!glyph_atlas_) {
        glyph_atlas_ = pixman_glyph_cache_create();
        if (!glyph_atlas_) return nullptr;
        ++atlas_generation_;
        atlas_entries_ = 0;
    }
    if (glyph.atlas_generation == atlas_generation_ && glyph.atlas_handle)
        return glyph.atlas_handle;
    if (atlas_entries_ >= 1024) {
        // Bounded: rebuild and invalidate all embedded handles by generation.
        pixman_glyph_cache_destroy(glyph_atlas_);
        glyph_atlas_ = pixman_glyph_cache_create();
        if (!glyph_atlas_) return nullptr;
        ++atlas_generation_;
        atlas_entries_ = 0;
    }
    pixman_glyph_cache_freeze(glyph_atlas_);
    const void* handle = pixman_glyph_cache_insert(glyph_atlas_, mask, mask, 0, 0, mask);
    pixman_glyph_cache_thaw(glyph_atlas_);
    if (handle) {
        mutable_glyph.atlas_handle = handle;
        mutable_glyph.atlas_generation = atlas_generation_;
        ++atlas_entries_;
    }
    return handle;
}

void CpuRenderer::flush_glyph_batch(pixman_image_t* target) {
    if (glyph_batch_.empty()) return;
    // Short runs (interleaved with box glyphs or decorations) are cheaper as
    // plain per-glyph composites; the atlas only pays off for longer runs.
    constexpr size_t min_batch = 4;
    pixman_image_t* batch_solid = solid_fill(glyph_batch_color_);
    if (glyph_batch_.size() >= min_batch && glyph_atlas_ && batch_solid &&
        glyph_batch_x1_ > glyph_batch_x0_ && glyph_batch_y1_ > glyph_batch_y0_) {
        glyph_write_.clear();
        glyph_write_.reserve(glyph_batch_.size());
        for (const auto& glyph : glyph_batch_)
            glyph_write_.push_back(pixman_glyph_t{glyph.x - glyph_batch_x0_, glyph.y - glyph_batch_y0_, glyph.handle});
        const int width = glyph_batch_x1_ - glyph_batch_x0_;
        const int height = glyph_batch_y1_ - glyph_batch_y0_;
        const auto format = pixman_glyph_get_mask_format(
            glyph_atlas_, static_cast<int>(glyph_write_.size()), glyph_write_.data());
        pixman_composite_glyphs(PIXMAN_OP_OVER, batch_solid, target, format,
            0, 0, 0, 0, glyph_batch_x0_, glyph_batch_y0_, width, height,
            glyph_atlas_, static_cast<int>(glyph_write_.size()), glyph_write_.data());
    } else if (batch_solid) {
        for (const auto& glyph : glyph_batch_) {
            pixman_image_composite32(PIXMAN_OP_OVER, batch_solid, glyph.mask, target,
                0, 0, 0, 0, glyph.x, glyph.y, glyph.width, glyph.height);
        }
    }
    glyph_batch_.clear();
}

void CpuRenderer::blit_image(
    pixman_image_t* target,
    pixman_image_t* source,
    int dx, int dy, int dw, int dh,
    const pixman_region32_t* clip,
    util::PixmanRegion* damage_out,
    int src_x, int src_y, int src_w, int src_h,
    const core::PlacedImage* placed
) {
    if (!target || !source || dw <= 0 || dh <= 0) return;
    const int image_w = pixman_image_get_width(source);
    const int image_h = pixman_image_get_height(source);
    if (image_w <= 0 || image_h <= 0) return;
    if (src_w <= 0) src_w = image_w;
    if (src_h <= 0) src_h = image_h;
    src_x = std::clamp(src_x, 0, std::max(0, image_w - 1));
    src_y = std::clamp(src_y, 0, std::max(0, image_h - 1));
    src_w = std::clamp(src_w, 1, image_w - src_x);
    src_h = std::clamp(src_h, 1, image_h - src_y);

    // Build a zero-copy subimage for the crop. Sampling the full image with a
    // translated transform would let bilinear filtering bleed in pixels just
    // outside the requested source rectangle.
    pixman_image_t* view = source;
    bool owns_view = false;
    if (src_x != 0 || src_y != 0 || src_w != image_w || src_h != image_h) {
        const int stride = pixman_image_get_stride(source);
        auto* pixels = reinterpret_cast<uint8_t*>(pixman_image_get_data(source)) +
            static_cast<size_t>(src_y) * stride + src_x * 4;
        view = pixman_image_create_bits_no_clear(pixman_image_get_format(source),
            src_w, src_h, reinterpret_cast<uint32_t*>(pixels), stride);
        if (!view) return;
        owns_view = true;
    }

    pixman_image_t* draw_source = view;
    pixman_image_t* scaled = nullptr;
    const bool cache_hit = placed && placed->cache_source == source &&
        placed->cache_src_x == src_x && placed->cache_src_y == src_y &&
        placed->cache_src_w == src_w && placed->cache_src_h == src_h &&
        placed->cache_dst_w == dw && placed->cache_dst_h == dh &&
        placed->scaled_cache.valid();
    if (src_w != dw || src_h != dh) {
        if (cache_hit) {
            draw_source = placed->scaled_cache.get();
        } else {
            auto* scaled_image = pixman_image_create_bits(PIXMAN_a8r8g8b8, dw, dh, nullptr, 0);
            if (!scaled_image) { if (owns_view) pixman_image_unref(view); return; }
            scaled = scaled_image;
            pixman_transform_t transform;
            pixman_transform_init_identity(&transform);
            transform.matrix[0][0] = pixman_double_to_fixed(static_cast<double>(src_w) / dw);
            transform.matrix[1][1] = pixman_double_to_fixed(static_cast<double>(src_h) / dh);
            pixman_image_set_transform(view, &transform);
            pixman_image_set_filter(view, PIXMAN_FILTER_BILINEAR, nullptr, 0);
            pixman_image_set_repeat(view, PIXMAN_REPEAT_PAD);
            pixman_image_composite32(PIXMAN_OP_SRC, view, nullptr, scaled,
                0, 0, 0, 0, 0, 0, dw, dh);
            pixman_image_set_transform(view, nullptr);
            pixman_image_set_filter(view, PIXMAN_FILTER_NEAREST, nullptr, 0);
            pixman_image_set_repeat(view, PIXMAN_REPEAT_NONE);
            draw_source = scaled;
            if (placed) {
                placed->scaled_cache.reset(scaled);
                scaled = nullptr; // Owned by placed->scaled_cache now.
                placed->cache_source = source;
                placed->cache_src_x = src_x; placed->cache_src_y = src_y;
                placed->cache_src_w = src_w; placed->cache_src_h = src_h;
                placed->cache_dst_w = dw; placed->cache_dst_h = dh;
                draw_source = placed->scaled_cache.get();
            }
        }
    }

    if (clip) pixman_image_set_clip_region32(target, const_cast<pixman_region32_t*>(clip));
    pixman_image_composite32(PIXMAN_OP_OVER, draw_source, nullptr, target,
        0, 0, 0, 0, dx, dy, dw, dh);
    if (clip) pixman_image_set_clip_region32(target, nullptr);

    if (damage_out) {
        int x0 = dx, y0 = dy, x1 = dx + dw, y1 = dy + dh;
        if (clip) {
            const auto* extents = pixman_region32_extents(const_cast<pixman_region32_t*>(clip));
            x0 = std::max(x0, extents->x1);
            y0 = std::max(y0, extents->y1);
            x1 = std::min(x1, extents->x2);
            y1 = std::min(y1, extents->y2);
        }
        if (x1 > x0 && y1 > y0)
            damage_out->union_rect(x0, y0, static_cast<unsigned>(x1 - x0), static_cast<unsigned>(y1 - y0));
    }
    if (scaled) pixman_image_unref(scaled);
    if (owns_view) pixman_image_unref(view);
}

void CpuRenderer::draw_placeholders(
    const core::TerminalEngine& engine,
    pixman_image_t* target,
    bool below_text,
    const pixman_region32_t* clip,
    int cell_w,
    int cell_h,
    const FontMetrics& metrics,
    util::PixmanRegion* damage_out
) {
    (void)metrics;
    if (engine.kitty_graphics().virtual_placements().empty() || cell_w <= 0 || cell_h <= 0) return;
    if (clip && !pixman_region32_not_empty(const_cast<pixman_region32_t*>(clip))) return;
    const auto& grid = engine.grid();
    const int rows = grid.rows(), cols = grid.cols();
    const int target_w = pixman_image_get_width(target);
    const int target_h = pixman_image_get_height(target);

    placeholder_cells_.assign(static_cast<size_t>(rows) * cols, PlaceholderCell{});
    placeholder_groups_.clear();
    placeholder_parent_.assign(placeholder_cells_.size(), -1);
    placeholder_group_of_.assign(placeholder_cells_.size(), -1);

    auto index = [cols](int row, int col) { return static_cast<size_t>(row) * cols + col; };
    auto find_root = [&](int value) {
        int root = value;
        while (placeholder_parent_[static_cast<size_t>(root)] != root)
            root = placeholder_parent_[static_cast<size_t>(root)];
        while (placeholder_parent_[static_cast<size_t>(value)] != root) {
            const int next = placeholder_parent_[static_cast<size_t>(value)];
            placeholder_parent_[static_cast<size_t>(value)] = root;
            value = next;
        }
        return root;
    };
    auto merge_group = [](PlaceholderGroup& dst, const PlaceholderGroup& src) {
        dst.min_grid_col = std::min(dst.min_grid_col, src.min_grid_col);
        dst.max_grid_col = std::max(dst.max_grid_col, src.max_grid_col);
        dst.min_grid_row = std::min(dst.min_grid_row, src.min_grid_row);
        dst.max_grid_row = std::max(dst.max_grid_row, src.max_grid_row);
        dst.min_img_col = std::min(dst.min_img_col, src.min_img_col);
        dst.min_img_row = std::min(dst.min_img_row, src.min_img_row);
    };
    auto unite_roots = [&](int ra, int rb) {
        if (ra == rb) return ra;
        placeholder_parent_[static_cast<size_t>(rb)] = ra;
        merge_group(placeholder_groups_[static_cast<size_t>(placeholder_group_of_[static_cast<size_t>(ra)])],
            placeholder_groups_[static_cast<size_t>(placeholder_group_of_[static_cast<size_t>(rb)])]);
        return ra;
    };
    auto compatible = [](const PlaceholderCell& a, const PlaceholderCell& b, bool horizontal) {
        if (!a.valid || !b.valid || a.image_id != b.image_id || a.placement_id != b.placement_id) return false;
        return horizontal
            ? a.image_row == b.image_row && a.image_col + 1 == b.image_col
            : a.image_col == b.image_col && a.image_row + 1 == b.image_row;
    };

    struct Previous {
        bool valid{false};
        uint32_t fg{0}, underline{0};
        int row{0}, col{0}, msb{0};
    } previous;

    for (int row = 0; row < rows; ++row) {
        previous = Previous{};
        for (int col = 0; col < cols; ++col) {
            const auto& cell = grid.cell(col, row);
            const core::CellExtra extra = grid.get_cell_extra(cell.extra_idx);
            const auto at = index(row, col);
            if (cell.codepoint != 0x10EEEE) {
                previous.valid = false;
                continue;
            }
            uint32_t image_id = 0;
            if (cell.fg.is_indexed()) image_id = cell.fg.index();
            else if (cell.fg.is_rgb()) {
                image_id = (static_cast<uint32_t>(cell.fg.r()) << 16) |
                    (static_cast<uint32_t>(cell.fg.g()) << 8) | cell.fg.b();
            } else {
                previous.valid = false;
                continue;
            }
            uint32_t placement_id = 0;
            if (extra.underline_color.is_indexed()) placement_id = extra.underline_color.index();
            else if (extra.underline_color.is_rgb()) {
                placement_id = (static_cast<uint32_t>(extra.underline_color.r()) << 16) |
                    (static_cast<uint32_t>(extra.underline_color.g()) << 8) | extra.underline_color.b();
            }

            int values[3] = {0, 0, 0};
            int count = 0;
            for (const char32_t mark : grid.get_combining_codepoints(extra.combining_idx)) {
                const int number = core::placeholder_number(mark);
                if (number < 0 || count == 3) break;
                values[count++] = number;
            }
            int image_row = values[0], image_col = values[1], msb = values[2];
            const uint32_t fg_value = cell.fg.value, underline_value = extra.underline_color.value;
            if (count == 0 && previous.valid && previous.fg == fg_value &&
                previous.underline == underline_value) {
                image_row = previous.row; image_col = previous.col + 1; msb = previous.msb;
            } else if (count == 1 && previous.valid && previous.row == image_row &&
                       previous.fg == fg_value && previous.underline == underline_value) {
                image_col = previous.col + 1; msb = previous.msb;
            } else if (count == 2 && previous.valid && previous.row == image_row &&
                       previous.fg == fg_value && previous.underline == underline_value &&
                       previous.col + 1 == image_col) {
                msb = previous.msb;
            }
            image_id |= static_cast<uint32_t>(msb) << 24;
            previous = Previous{true, fg_value, underline_value, image_row, image_col, msb};

            const auto* vp = engine.kitty_graphics().find_virtual_placement(image_id, placement_id);
            if (!vp || (vp->z_index < 0) != below_text ||
                !engine.kitty_graphics().get_image(image_id)) continue;
            PlaceholderCell current{true, image_id, placement_id, image_row, image_col};
            placeholder_cells_[at] = current;

            int root = -1;
            if (col > 0 && placeholder_parent_[index(row, col - 1)] >= 0 &&
                compatible(placeholder_cells_[index(row, col - 1)], current, true)) {
                root = find_root(index(row, col - 1));
            }
            if (row > 0 && placeholder_parent_[index(row - 1, col)] >= 0 &&
                compatible(placeholder_cells_[index(row - 1, col)], current, false)) {
                const int up_root = find_root(index(row - 1, col));
                root = root < 0 ? up_root : unite_roots(root, up_root);
            }
            if (root < 0) {
                root = static_cast<int>(at);
                placeholder_parent_[at] = root;
                placeholder_group_of_[at] = static_cast<int>(placeholder_groups_.size());
                placeholder_groups_.push_back(PlaceholderGroup{image_id, placement_id, col, col, row, row, image_col, image_row});
            } else {
                placeholder_parent_[at] = root;
                auto& group = placeholder_groups_[static_cast<size_t>(placeholder_group_of_[static_cast<size_t>(root)])];
                group.min_grid_col = std::min(group.min_grid_col, col);
                group.max_grid_col = std::max(group.max_grid_col, col);
                group.min_grid_row = std::min(group.min_grid_row, row);
                group.max_grid_row = std::max(group.max_grid_row, row);
                group.min_img_col = std::min(group.min_img_col, image_col);
                group.min_img_row = std::min(group.min_img_row, image_row);
            }
        }
    }

    for (const auto& group : placeholder_groups_) {
        const auto* image = engine.kitty_graphics().get_image(group.image_id);
        const auto* vp = engine.kitty_graphics().find_virtual_placement(group.image_id, group.placement_id);
        if (!image || !image->image.valid() || !vp) continue;
        const int image_w = image->image.width(), image_h = image->image.height();
        if (image_w <= 0 || image_h <= 0) continue;
        const int vp_cols = vp->cols > 0 ? vp->cols : std::max(1, (image_w + cell_w - 1) / cell_w);
        const int vp_rows = vp->rows > 0 ? vp->rows : std::max(1, (image_h + cell_h - 1) / cell_h);
        const double fit = std::min(static_cast<double>(vp_cols * cell_w) / image_w,
                                    static_cast<double>(vp_rows * cell_h) / image_h);
        const int draw_w = std::max(1, static_cast<int>(image_w * fit));
        const int draw_h = std::max(1, static_cast<int>(image_h * fit));

        // Reconstruct the rectangle's origin from a visible cell and its
        // absolute image-grid coordinate, so a scrolled top row still draws.
        const int origin_x = (group.min_grid_col - group.min_img_col) * cell_w;
        const int origin_y = (group.min_grid_row - group.min_img_row) * cell_h;
        int x0 = std::max({0, group.min_grid_col * cell_w, origin_x});
        int y0 = std::max({0, group.min_grid_row * cell_h, origin_y});
        int x1 = std::min({target_w, (group.max_grid_col + 1) * cell_w, origin_x + draw_w});
        int y1 = std::min({target_h, (group.max_grid_row + 1) * cell_h, origin_y + draw_h});
        if (x1 <= x0 || y1 <= y0) continue;

        pixman_region32_t render_clip;
        pixman_region32_init_rect(&render_clip, x0, y0,
            static_cast<unsigned>(x1 - x0), static_cast<unsigned>(y1 - y0));
        if (clip) {
            pixman_region32_intersect(&render_clip, &render_clip,
                const_cast<pixman_region32_t*>(clip));
            if (!pixman_region32_not_empty(&render_clip)) {
                pixman_region32_fini(&render_clip);
                continue;
            }
        }
        blit_image(target, image->image.get(), origin_x, origin_y, draw_w, draw_h,
            &render_clip, damage_out);
        pixman_region32_fini(&render_clip);
    }
}

void CpuRenderer::draw_images(
    const core::TerminalEngine& engine,
    pixman_image_t* target,
    int cell_w,
    int cell_h,
    bool below_text,
    int row,
    util::PixmanRegion* damage_out,
    const pixman_region32_t* clip
) {
    if (cell_w <= 0 || cell_h <= 0) return;
    if (clip && !pixman_region32_not_empty(const_cast<pixman_region32_t*>(clip))) return;
    const int target_w = pixman_image_get_width(target);
    const int target_h = pixman_image_get_height(target);
    const int band_top = row >= 0 ? row * cell_h : 0;
    const int band_bottom = row >= 0 ? std::min(target_h, (row + 1) * cell_h) : target_h;
    if (band_bottom <= band_top) return;

    for (const auto& placed : engine.images()) {
        if (placed.alt_screen != engine.is_alt_screen()) continue;
        if ((placed.z_index < 0) != below_text) continue;
        const int src_w = placed.image.width();
        const int src_h = placed.image.height();
        const int dst_w = placed.cols * cell_w;
        const int dst_h = placed.rows * cell_h;
        if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) continue;

        const int dx = placed.col * cell_w;
        const int dy = placed.row * cell_h;
        const int x0 = std::max(0, dx), y0 = std::max(band_top, dy);
        const int x1 = std::min(target_w, dx + dst_w), y1 = std::min(band_bottom, dy + dst_h);
        if (x1 <= x0 || y1 <= y0) continue;

        pixman_region32_t image_clip;
        pixman_region32_init_rect(&image_clip, x0, y0,
            static_cast<unsigned>(x1 - x0), static_cast<unsigned>(y1 - y0));
        if (clip) {
            pixman_region32_intersect(&image_clip, &image_clip,
                const_cast<pixman_region32_t*>(clip));
            if (!pixman_region32_not_empty(&image_clip)) {
                pixman_region32_fini(&image_clip);
                continue;
            }
        }
        blit_image(target, placed.image.get(), dx, dy, dst_w, dst_h, &image_clip, damage_out,
            placed.src_x, placed.src_y, placed.src_w, placed.src_h, &placed);
        pixman_region32_fini(&image_clip);
    }
}

void CpuRenderer::render_cell(
    const core::Cell& cell,
    const core::CellExtra& extra,
    int col,
    int row,
    int cell_w,
    int cell_h,
    const FontMetrics& metrics,
    const core::TerminalEngine& engine,
    pixman_image_t* target
) {
    if (cell.attrs.width == 0) return;
    pixman_color_t bg_pcol = to_pixman_color(effective_background(cell, engine));
    pixman_rectangle16_t bg_rect = {
        static_cast<int16_t>(col * cell_w), static_cast<int16_t>(row * cell_h),
        static_cast<uint16_t>(cell_w * (cell.attrs.width > 1 ? cell.attrs.width : 1)),
        static_cast<uint16_t>(cell_h)
    };
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, target, &bg_pcol, 1, &bg_rect);
    if (!cell.attrs.invisible)
        render_cell_foreground(cell, extra, col, row, cell_w, cell_h, metrics, engine, target);
}

void CpuRenderer::render_cell_foreground(
    const core::Cell& cell,
    const core::CellExtra& extra,
    int col,
    int row,
    int cell_w,
    int cell_h,
    const FontMetrics& metrics,
    const core::TerminalEngine& engine,
    pixman_image_t* target
) {
    if (cell.attrs.width == 0 || cell.attrs.invisible) return;
    // Unicode image placeholder: the image tile is drawn by the placeholder
    // pass; the placeholder character itself must not be rasterized.
    if (cell.codepoint == 0x10EEEE) return;

    int px = col * cell_w;
    int py = row * cell_h;
    int span_w = cell_w * (cell.attrs.width > 1 ? cell.attrs.width : 1);

    const uint32_t fg_argb = resolve_foreground(cell, engine);
    pixman_color_t fg_pcol = to_pixman_color(fg_argb);

    // Procedural box / block rendering
    char32_t cp = cell.codepoint;
    bool rendered_box = false;
    if (BoxDrawingRenderer::is_box_or_block(cp)) {
        rendered_box = BoxDrawingRenderer::render(target, cp, px, py, span_w, cell_h, fg_pcol);
    }

    if (!rendered_box && cp != ' ' && cp != 0) {
        pixman_image_t* glyph_solid = solid_fill(fg_argb);

        // A cell with combining marks is one grapheme cluster: shape it so the
        // marks land on their HarfBuzz anchors instead of being dropped.
        if (extra.combining_idx != 0) {
            // The store holds the marks only; the base codepoint lives in the
            // cell. Shape them together so marks get their anchors.
            const auto marks = engine.grid().get_combining_codepoints(extra.combining_idx);
            std::array<char32_t, 8> cluster{};
            size_t count = 1;
            cluster[0] = cp;
            for (const char32_t mark : marks) {
                if (count == cluster.size()) break;
                cluster[count++] = mark;
            }
            const ShapedCluster* shaped = font_engine_.shape_cluster(
                std::span<const char32_t>(cluster.data(), count), cell.attrs.bold, cell.attrs.italic);
            if (shaped && !shaped->glyphs.empty()) {
                // HarfBuzz offsets are relative to the pen *after* the previous
                // glyph; accumulate advances so zero-advance marks land back
                // over the base instead of one cell to the left. Clip to the
                // cell like foot, so a mark that reaches above the ascent
                // cannot leave ink in the neighbouring row's damage.
                pixman_region32_t clip;
                pixman_region32_init_rect(&clip, px, py, static_cast<unsigned>(span_w),
                    static_cast<unsigned>(cell_h));
                pixman_image_set_clip_region32(target, &clip);
                int pen_x = 0, pen_y = 0;
                for (const auto& part : shaped->glyphs) {
                    const CachedGlyph* glyph = font_engine_.glyph_for_index(
                        shaped->face_idx, part.glyph_index, cell.attrs.bold, cell.attrs.italic);
                    if (glyph && glyph->mask.valid() && glyph->width > 0 && glyph->height > 0) {
                        pixman_image_composite32(PIXMAN_OP_OVER, glyph_solid, glyph->mask.get(), target,
                            0, 0, 0, 0,
                            px + pen_x + part.x_offset + glyph->bearing_x,
                            py + metrics.ascent - pen_y - part.y_offset - glyph->bearing_y,
                            glyph->width, glyph->height);
                    }
                    pen_x += part.x_advance;
                    pen_y += part.y_advance;
                }
                pixman_image_set_clip_region32(target, nullptr);
                pixman_region32_fini(&clip);
                cp = 0; // Already rendered the whole cluster.
            }
        }

        if (cp != 0) {
            const CachedGlyph* glyph = font_engine_.get_glyph(cp, cell.attrs.bold, cell.attrs.italic);
            if (glyph && glyph->mask.valid() && glyph->width > 0 && glyph->height > 0) {
                pixman_image_composite32(PIXMAN_OP_OVER, glyph_solid, glyph->mask.get(), target,
                    0, 0, 0, 0,
                    px + glyph->bearing_x,
                    py + metrics.ascent - glyph->bearing_y,
                    glyph->width, glyph->height);
            }
        }
    }

    // Underlines
    if (cell.attrs.underline_style != 0) {
        uint32_t ul_argb = extra.underline_color.is_default() ? fg_argb : resolve_color(extra.underline_color, engine, false);
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

    // OSC 8 hyperlink: underline it so the link is visible (tmux forwards links).
    if (extra.hyperlink_id != 0 && cell.attrs.underline_style == 0) {
        const int link_y = py + metrics.ascent + metrics.underline_pos;
        const int link_h = std::max(1, metrics.underline_thickness);
        pixman_rectangle16_t link = {
            static_cast<int16_t>(px), static_cast<int16_t>(link_y),
            static_cast<uint16_t>(span_w), static_cast<uint16_t>(link_h)
        };
        pixman_image_fill_rectangles(PIXMAN_OP_SRC, target, &fg_pcol, 1, &link);
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
            // Pixman's XOR is Porter-Duff XOR, not a bitwise color invert:
            // XOR of two opaque pixels becomes transparent. Redraw instead.
            int col = engine.cursor_col();
            if (engine.grid().cell(col, engine.cursor_row()).attrs.width == 0 && col > 0) --col;
            auto cell = engine.grid().cell(col, engine.cursor_row());
            const auto bg = resolve_color(cell.attrs.inverse ? cell.fg : cell.bg, engine, !cell.attrs.inverse);
            cell.fg = core::Color::make_rgb((bg >> 16) & 255, (bg >> 8) & 255, bg & 255);
            cell.bg = core::Color::make_rgb((config_.cursor_color >> 16) & 255,
                (config_.cursor_color >> 8) & 255, config_.cursor_color & 255);
            cell.attrs.inverse = false;
            const core::CellExtra extra = engine.grid().get_cell_extra(cell.extra_idx);
            render_cell(cell, extra, col, engine.cursor_row(), cell_w, cell_h,
                font_engine_.metrics(), engine, target);
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

} // namespace gritty::render
