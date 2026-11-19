#pragma once

#include <cstdint>
#include <string>
#include <span>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <array>
#include <list>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_LCD_FILTER_H
#include <hb.h>
#include <hb-ft.h>
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

struct ShapedGlyph {
    uint32_t glyph_index{0};
    int x_offset{0};  // HarfBuzz offsets, in pixels, applied on top of the mask bearing.
    int y_offset{0};  // Positive means "up" (screen Y grows down; subtract it).
    int x_advance{0}; // Pen advance after this glyph; marks carry zero advance and
    int y_advance{0}; // are positioned relative to the pen *after* the base glyph.
};

struct ShapedCluster {
    uint8_t face_idx{0};
    std::vector<ShapedGlyph> glyphs;
};

struct CachedGlyph {
    util::PixmanImage mask;
    int bearing_x{0};
    int bearing_y{0};
    int width{0};
    int height{0};
    bool is_subpixel{false};
    // Straight-colour emoji bitmaps are their own source image, not a mask.
    bool is_color{false};

    // Owned by the renderer's glyph atlas, not by the glyph cache. Generation
    // invalidates handles after an atlas rebuild.
    mutable const void* atlas_handle{nullptr};
    mutable uint32_t atlas_generation{0};
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

struct GlyphCacheLimits {
    size_t max_bitmap_bytes{16 * 1024 * 1024}; // Cached stride * height, excluding one oversized transient.
    size_t max_entries{8192}; // Zero disables caching (including empty glyphs).
};

struct GlyphCacheStats {
    size_t entries{0}, bitmap_bytes{0}, transient_bitmap_bytes{0};
    size_t hits{0}, misses{0}, evictions{0};
};

class FontEngine final {
public:
    explicit FontEngine(AntialiasMode antialias = AntialiasMode::Grayscale, GlyphCacheLimits limits = {});
    ~FontEngine() = default;

    FontEngine(const FontEngine&) = delete;
    FontEngine& operator=(const FontEngine&) = delete;

    // Load primary font from file path
    bool load_font(std::string_view font_path, double pt_size, unsigned int dpi = 96);

    // Register a fallback path; FreeType/HarfBuzz are opened only on a primary
    // coverage miss. Success means registration, not validation of the file.
    bool add_fallback_font(std::string_view font_path);
    [[nodiscard]] size_t loaded_face_count() const noexcept;
    [[nodiscard]] size_t face_load_attempt_count() const noexcept { return face_load_attempts_; }

    [[nodiscard]] const FontMetrics& metrics() const noexcept { return metrics_; }
    [[nodiscard]] AntialiasMode antialias_mode() const noexcept { return antialias_mode_; }
    void set_antialias_mode(AntialiasMode mode) noexcept { antialias_mode_ = mode; clear_cache(); }
    [[nodiscard]] AntialiasMode effective_antialias_mode() const noexcept {
        return antialias_mode_ == AntialiasMode::SubpixelLcd && !subpixel_allowed_
            ? AntialiasMode::Grayscale : antialias_mode_;
    }
    void set_subpixel_allowed(bool allowed) noexcept {
        if (subpixel_allowed_ != allowed) { subpixel_allowed_ = allowed; clear_cache(); }
    }

    // Resolve and retrieve a glyph. Consume the result before the next lookup:
    // cache misses may evict old entries; oversized glyphs use one transient slot.
    const CachedGlyph* get_glyph(char32_t cp, bool bold, bool italic);
    // Glyph by index, used for shaped clusters where the codepoint mapping has
    // already been resolved by HarfBuzz.
    const CachedGlyph* glyph_for_index(uint8_t face_idx, uint32_t glyph_index, bool bold, bool italic);
    // Shape a grapheme cluster (base + combining marks / ZWJ sequences) with
    // HarfBuzz so marks land on their anchors instead of being dropped. Cached
    // per cluster text and style; cleared with the glyph cache.
    const ShapedCluster* shape_cluster(std::span<const char32_t> cluster, bool bold, bool italic);
    [[nodiscard]] GlyphCacheStats cache_stats() const noexcept;
    [[nodiscard]] GlyphCacheLimits cache_limits() const noexcept { return cache_limits_; }
    void set_cache_limits(GlyphCacheLimits limits) noexcept;

    // Clear glyph cache
    void clear_cache() noexcept;

private:
    struct LoadedFace {
        util::FtFace face;
        util::HbFont hb_font;
        std::string path;
        bool attempted{false};
    };

    struct GlyphCacheEntry {
        CachedGlyph glyph;
        std::list<GlyphKey>::iterator recency;
        size_t bitmap_bytes{0};
    };

    struct CharIndex {
        uint8_t face_idx{0};
        uint32_t glyph_index{0};
        bool resolved{false}; // Present but not found in any face (cached miss).
    };
    const CachedGlyph* cache_glyph(const GlyphKey& key, CachedGlyph glyph);
    [[nodiscard]] CharIndex resolve_char(char32_t cp);
    const CachedGlyph* cached_or_rasterize(const GlyphKey& key);
    bool open_fallback(LoadedFace& face);
    void compute_metrics();
    const CachedGlyph* rasterize_glyph(uint8_t face_idx, uint32_t glyph_index, bool bold, bool italic);

    util::FtLibrary ft_lib_;
    AntialiasMode antialias_mode_{AntialiasMode::Grayscale};
    bool subpixel_allowed_{true};
    double pt_size_{12.0};
    unsigned int dpi_{96};
    FontMetrics metrics_{};

    size_t face_load_attempts_{0};
    std::vector<LoadedFace> faces_;
    GlyphCacheLimits cache_limits_;
    size_t cache_bitmap_bytes_{0}, cache_hits_{0}, cache_misses_{0}, cache_evictions_{0};
    size_t transient_bitmap_bytes_{0};
    // Codepoint to (face, glyph index) memo. FreeType cmap lookups otherwise run
    // for every cell of every frame before the glyph bitmap cache is consulted;
    // foot's fcft indexes its cache directly by codepoint.
    std::unordered_map<char32_t, CharIndex> char_index_cache_;
    // Fixed-size key: cluster lookups happen per combined cell per frame and
    // must not allocate (project rule for per-frame cell rendering).
    struct ShapedClusterKey {
        std::array<char32_t, 8> text{};
        uint8_t length{0};
        bool bold{false};
        bool italic{false};
        bool operator==(const ShapedClusterKey& other) const noexcept {
            return length == other.length && bold == other.bold && italic == other.italic &&
                std::equal(text.begin(), text.begin() + length, other.text.begin());
        }
    };
    struct ShapedClusterKeyHash {
        size_t operator()(const ShapedClusterKey& key) const noexcept {
            size_t h = 1469598103934665603ull; // FNV-1a over the codepoints.
            for (uint8_t i = 0; i < key.length; ++i) {
                h ^= static_cast<size_t>(key.text[i]);
                h *= 1099511628211ull;
            }
            h ^= key.bold ? 0x9e3779b9u : 0u;
            h ^= key.italic ? 0x85ebca6bu : 0u;
            return h;
        }
    };
    std::unordered_map<ShapedClusterKey, ShapedCluster, ShapedClusterKeyHash> shaped_cache_;
    // Reused across shaping misses; hb_buffer_clear_contents() keeps the
    // allocation while resetting per-cluster state.
    util::HbBuffer shape_buffer_;
    CachedGlyph transient_glyph_;
    std::list<GlyphKey> glyph_lru_;
    std::unordered_map<GlyphKey, GlyphCacheEntry, GlyphKeyHash> glyph_cache_;
    // L1 lookup for printable ASCII: [bold * 2 + italic][cp - 0x20].
    struct AsciiGlyphSlot {
        const CachedGlyph* glyph{nullptr};
        GlyphKey key{};
    };
    mutable AsciiGlyphSlot ascii_glyph_table_[4][95]{};
    // 4096-slot direct-mapped L1 for non-ASCII codepoints. The folded index can
    // collide, so parallel tags validate the cached pointer.
    mutable const CachedGlyph* non_ascii_l1_[4][4096]{};
    mutable char32_t non_ascii_l1_cp_[4][4096]{};
};

} // namespace gritty::render
