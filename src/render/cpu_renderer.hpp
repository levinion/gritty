#pragma once

#include <memory>
#include <pixman.h>
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
    ~CpuRenderer() = default;

    CpuRenderer(const CpuRenderer&) = delete;
    CpuRenderer& operator=(const CpuRenderer&) = delete;

    [[nodiscard]] FontEngine& font_engine() noexcept { return font_engine_; }
    [[nodiscard]] const FontEngine& font_engine() const noexcept { return font_engine_; }

    [[nodiscard]] const RenderConfig& config() const noexcept { return config_; }
    void set_config(const RenderConfig& cfg) noexcept { config_ = cfg; }

    // Render the terminal grid onto target PixmanImage.
    // If damage_out is non-null, populates it with the bounding pixel damage region.
    // full_redraw forces redrawing all cells regardless of row dirty bits.
    void render(
        core::TerminalEngine& engine,
        pixman_image_t* target,
        util::PixmanRegion* damage_out = nullptr,
        bool full_redraw = false
    );

private:
    uint32_t resolve_color(const core::Color& c, const core::TerminalEngine& engine, bool is_bg) const noexcept;
    static pixman_color_t to_pixman_color(uint32_t argb) noexcept;

    void render_cell(
        const core::Cell& cell,
        int col,
        int row,
        int cell_w,
        int cell_h,
        const FontMetrics& metrics,
        const core::TerminalEngine& engine,
        pixman_image_t* target
    );

    void render_cursor(
        const core::TerminalEngine& engine,
        int cell_w,
        int cell_h,
        pixman_image_t* target
    );

    FontEngine& font_engine_;
    RenderConfig config_;

    // Cached solid color source for glyph compositing
    util::PixmanImage solid_src_;
    uint32_t current_solid_color_{0};
};

} // namespace gritty::render
