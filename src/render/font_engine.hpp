#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <memory>
#include <span>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_LCD_FILTER_H
#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>
#include "util/raii.hpp"

namespace gritty::render {

enum class AntialiasMode : uint8_t {
    Grayscale,
    SubpixelLcd,
    None,
};

struct FontMetrics {
    int cell_width{8};
    int cell_height{16};
    int ascent{12};
    int descent{4};
    int underline_pos{2};
    int underline_thickness{1};
};

struct CachedGlyph {
    util::PixmanImage mask;
    int bearing_x{0};
    int bearing_y{0};
    int width{0};
    int height{0};
    bool is_subpixel{false};
};

struct GlyphKey {
    uint32_t glyph_index{0};
    uint8_t face_idx{0};
    bool bold{false};
    bool italic{false};

    bool operator==(const GlyphKey& other) const noexcept {
        return glyph_index == other.glyph_index &&
               face_idx == other.face_idx &&
               bold == other.bold &&
               italic == other.italic;
    }
};

struct GlyphKeyHash {
    size_t operator()(const GlyphKey& k) const noexcept {
        size_t h = k.glyph_index;
        h ^= (static_cast<size_t>(k.face_idx) << 24);
        h ^= (k.bold ? 0x1000000 : 0);
        h ^= (k.italic ? 0x2000000 : 0);
        return h;
    }
};

class FontEngine final {
public:
    explicit FontEngine(AntialiasMode antialias = AntialiasMode::Grayscale);
    ~FontEngine() = default;

    FontEngine(const FontEngine&) = delete;
    FontEngine& operator=(const FontEngine&) = delete;

    // Load primary font from file path
    bool load_font(std::string_view font_path, double pt_size, unsigned int dpi = 96);

    // Add fallback font
    bool add_fallback_font(std::string_view font_path);

    [[nodiscard]] const FontMetrics& metrics() const noexcept { return metrics_; }
    [[nodiscard]] AntialiasMode antialias_mode() const noexcept { return antialias_mode_; }
    void set_antialias_mode(AntialiasMode mode) noexcept { antialias_mode_ = mode; clear_cache(); }

    // Resolve and retrieve rasterized glyph
    const CachedGlyph* get_glyph(char32_t cp, bool bold, bool italic);

    // Clear glyph cache
    void clear_cache() noexcept;

private:
    struct LoadedFace {
        util::FtFace face;
        util::HbFont hb_font;
        std::string path;
    };

    void compute_metrics();
    const CachedGlyph* rasterize_glyph(uint8_t face_idx, uint32_t glyph_index, bool bold, bool italic);

    util::FtLibrary ft_lib_;
    AntialiasMode antialias_mode_{AntialiasMode::Grayscale};
    double pt_size_{12.0};
    unsigned int dpi_{96};
    FontMetrics metrics_{};

    std::vector<LoadedFace> faces_;
    std::unordered_map<GlyphKey, CachedGlyph, GlyphKeyHash> glyph_cache_;
};

} // namespace gritty::render
