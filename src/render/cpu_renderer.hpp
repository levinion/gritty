#pragma once

#include <memory>
#include <unordered_map>
#include <vector>
#include <pixman.h>
#include "core/screen_grid.hpp"
#include "core/terminal_engine.hpp"
#include "render/font_engine.hpp"
#include "render/box_drawing.hpp"
#include "util/raii.hpp"

namespace gritty::render {

struct RenderConfig {
    uint32_t default_fg{0xffdddddd};
    uint32_t default_bg{0xff111111};
    uint32_t cursor_color{0xffffffff};
    bool render_cursor{true};
};

class CpuRenderer final {
public:
    explicit CpuRenderer(FontEngine& font_engine, RenderConfig config = RenderConfig{});
    ~CpuRenderer();

    CpuRenderer(const CpuRenderer&) = delete;
    CpuRenderer& operator=(const CpuRenderer&) = delete;

    [[nodiscard]] FontEngine& font_engine() noexcept { return font_engine_; }
    [[nodiscard]] const FontEngine& font_engine() const noexcept { return font_engine_; }

    [[nodiscard]] const RenderConfig& config() const noexcept { return config_; }
    void set_config(const RenderConfig& cfg) noexcept { config_ = cfg; config_changed_ = true; }

    // Scrolling whole-width regions can move the existing pixels instead of
    // re-rasterizing every row (foot render.c grid_render_scroll). Adaptive
    // (default) compares the cost of a move (read+write every moved pixel) with
    // the cost of repainting the destination cells, which now collapses
    // background runs and skips blank cells. Repaint wins on sparse screens.
    enum class ScrollMode { Adaptive, AlwaysMove, Repaint };
    void set_scroll_mode(ScrollMode mode) noexcept { scroll_mode_ = mode; }
    [[nodiscard]] ScrollMode scroll_mode() const noexcept { return scroll_mode_; }
    // Batch same-colour grayscale glyphs of a row through pixman's glyph atlas
    // (measured 118 -> 87 ns per glyph). Disabled only for A/B and pixel tests.
    void set_glyph_batching(bool enabled) noexcept { glyph_batching_ = enabled; }
    [[nodiscard]] bool glyph_batching() const noexcept { return glyph_batching_; }
    void set_scroll_optimization(bool enabled) noexcept {
        scroll_mode_ = enabled ? ScrollMode::Adaptive : ScrollMode::Repaint;
    }
    void set_preedit(std::string_view utf8, size_t sel_start, size_t sel_end);
    void set_status_banner(std::string_view utf8);
    void reset_glyph_atlas() noexcept;
    struct RenderStats {
        size_t scroll_moves{0};
        size_t scroll_skips{0};    // Move was redundant: destinations are fully repainted anyway.
        size_t scroll_repaints{0}; // Sparse destinations: repainting was cheaper.
        long long scrolled_rows{0};
        size_t full_redraws{0};
    };
    [[nodiscard]] const RenderStats& stats() const noexcept { return stats_; }

    // Render the terminal grid onto target PixmanImage.
    // If damage_out is non-null, populates it with the bounding pixel damage region.
    // full_redraw forces redrawing all cells regardless of row dirty bits.
    void render(
        core::TerminalEngine& engine,
        pixman_image_t* target,
        util::PixmanRegion* damage_out = nullptr,
        bool full_redraw = false
    );

    [[nodiscard]] static pixman_color_t to_pixman_color(uint32_t argb) noexcept;

private:
    uint32_t resolve_color(const core::Color& c, const core::TerminalEngine& engine, bool is_bg) const noexcept;

    void render_cell(
        const core::Cell& cell,
        const core::CellExtra& extra,
        int col,
        int row,
        int cell_w,
        int cell_h,
        const FontMetrics& metrics,
        const core::TerminalEngine& engine,
        pixman_image_t* target
    );

    // Background and foreground are drawn in separate passes per row: adjacent
    // cells sharing a background become one fill_rectangles() call (foot fills
    // each cell individually), and blank cells with no decoration skip the
    // foreground pass entirely.
    [[nodiscard]] uint32_t effective_background(
        const core::Cell& cell, const core::TerminalEngine& engine) const noexcept;
    void draw_row_backgrounds(
        const core::ScreenGrid& grid,
        int row,
        int start_col,
        int end_col,
        int cell_w,
        int cell_h,
        const core::TerminalEngine& engine,
        pixman_image_t* target
    );
    [[nodiscard]] uint32_t resolve_foreground(
        const core::Cell& cell, const core::TerminalEngine& engine) const noexcept;
    [[nodiscard]] const void* glyph_atlas_handle(const CachedGlyph& glyph);
    // Reuse one in-place 1x1 image for every solid color instead of allocating
    // pixman solid fills while rendering high-frequency SGR color changes.
    pixman_image_t* get_solid_fill(uint32_t argb);
    void flush_glyph_batch(pixman_image_t* target);
    void render_cell_foreground(
        const core::Cell& cell,
        const core::CellExtra& extra,
        int col,
        int row,
        int cell_w,
        int cell_h,
        const FontMetrics& metrics,
        const core::TerminalEngine& engine,
        pixman_image_t* target
    );

    void apply_pending_scrolls(
        const core::TerminalEngine& engine,
        core::ScreenGrid& grid,
        pixman_image_t* target,
        int cell_w,
        int cell_h,
        util::PixmanRegion* damage_out
    );

    void draw_placeholders(
        const core::TerminalEngine& engine,
        pixman_image_t* target,
        bool below_text,
        const pixman_region32_t* clip,
        int cell_w,
        int cell_h,
        const FontMetrics& metrics,
        util::PixmanRegion* damage_out
    );

    // Composite `source` into (dx,dy,dw,dh), resampling into a temporary when
    // the size differs. Keeps 1:1 blits allocation-free.
    void blit_image(
        pixman_image_t* target,
        pixman_image_t* source,
        int dx, int dy, int dw, int dh,
        const pixman_region32_t* clip,
        util::PixmanRegion* damage_out,
        int src_x = 0, int src_y = 0, int src_w = 0, int src_h = 0,
        const core::PlacedImage* placed = nullptr
    );

    void draw_images(
        const core::TerminalEngine& engine,
        pixman_image_t* target,
        int cell_w,
        int cell_h,
        bool below_text,
        int row, // >= 0: clip to this row band; -1: full target.
        util::PixmanRegion* damage_out,
        const pixman_region32_t* clip = nullptr
    );

    void render_cursor(
        const core::TerminalEngine& engine,
        int cell_w,
        int cell_h,
        pixman_image_t* target
    );

    void render_preedit(
        const core::TerminalEngine& engine,
        int cell_w,
        int cell_h,
        const FontMetrics& metrics,
        pixman_image_t* target,
        util::PixmanRegion* damage_out
    );

    void render_status_banner(
        const core::TerminalEngine& engine,
        int cell_w,
        int cell_h,
        const FontMetrics& metrics,
        pixman_image_t* target,
        util::PixmanRegion* damage_out
    );

    FontEngine& font_engine_;
    RenderConfig config_;
    bool config_changed_{true};

    int previous_cursor_col_{-1};
    int previous_cursor_row_{-1};
    std::string preedit_text_;
    size_t preedit_sel_start_{0};
    size_t preedit_sel_end_{0};
    int previous_preedit_row_{-1};
    int previous_preedit_start_col_{0};
    int previous_preedit_end_col_{-1};
    std::string status_banner_text_;
    int previous_banner_row_{-1};
    int previous_banner_start_col_{0};
    int previous_banner_end_col_{-1};
    ScrollMode scroll_mode_{ScrollMode::Adaptive};
    bool glyph_batching_{true};
    pixman_glyph_cache_t* glyph_atlas_{nullptr};
    uint32_t atlas_generation_;
    size_t atlas_entries_{0};

    uint32_t opaque_solid_pixel_{0xff000000};
    util::PixmanImage opaque_solid_src_;
    uint32_t dynamic_solid_pixel_{0};
    util::PixmanImage dynamic_solid_src_;
    struct BatchGlyph {
        int x{0}, y{0}, width{0}, height{0};
        int row{0};
        const void* handle{nullptr};
        util::PixmanImage mask;
    };
    std::vector<BatchGlyph> glyph_batch_;
    std::vector<pixman_glyph_t> glyph_write_;
    uint32_t glyph_batch_color_{0};
    int glyph_batch_x0_{0}, glyph_batch_y0_{0}, glyph_batch_x1_{0}, glyph_batch_y1_{0};
    std::vector<core::ScrollOp> scroll_scratch_;

    struct PlaceholderCell {
        bool valid{false};
        uint32_t image_id{0}, placement_id{0};
        int image_row{0}, image_col{0};
    };
    struct PlaceholderGroup {
        bool active{true};
        uint32_t image_id{0}, placement_id{0};
        int origin_col{0}, origin_row{0}; // Constant grid_col - img_col, grid_row - img_row for the component.
    };
    std::vector<PlaceholderCell> placeholder_cells_;
    std::vector<PlaceholderGroup> placeholder_groups_;
    std::vector<int> placeholder_parent_;
    std::vector<int> placeholder_group_of_;
    std::vector<std::vector<pixman_box32_t>> placeholder_boxes_;
    RenderStats stats_{};
};

} // namespace gritty::render
