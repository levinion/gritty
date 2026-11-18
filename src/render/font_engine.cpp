#include "render/font_engine.hpp"
#include <ft2build.h>
#include FT_SYNTHESIS_H
#include <algorithm>
#include <cmath>

namespace gritty::render {

FontEngine::FontEngine(AntialiasMode antialias, GlyphCacheLimits limits)
    : antialias_mode_(antialias), cache_limits_(limits) {
    if (ft_lib_.valid()) {
        FT_Library_SetLcdFilter(ft_lib_.get(), FT_LCD_FILTER_DEFAULT);
    }
}

bool FontEngine::load_font(std::string_view font_path, double pt_size, unsigned int dpi) {
    if (!ft_lib_.valid()) return false;

    pt_size_ = pt_size;
    dpi_ = dpi;

    FT_Face raw_face = nullptr;
    FT_Error err = FT_New_Face(ft_lib_.get(), std::string(font_path).c_str(), 0, &raw_face);
    if (err || !raw_face) return false;

    util::FtFace ft_face(raw_face);
    FT_F26Dot6 char_size = static_cast<FT_F26Dot6>(pt_size_ * 64.0);
    FT_Set_Char_Size(ft_face.get(), 0, char_size, dpi_, dpi_);

    hb_font_t* hb_font = hb_ft_font_create_referenced(ft_face.get());

    faces_.clear();
    faces_.push_back(LoadedFace{
        .face = std::move(ft_face),
        .hb_font = util::HbFont(hb_font),
        .path = std::string(font_path),
        .attempted = true
    });
    face_load_attempts_ = 1;

    compute_metrics();
    clear_cache();
    return true;
}

bool FontEngine::add_fallback_font(std::string_view font_path) {
    if (!ft_lib_.valid() || faces_.empty() || font_path.empty()) return false;
    // uint8_t GlyphKey::face_idx must never wrap to a different face.
    for (size_t i = 1; i < faces_.size(); ++i)
        if (faces_[i].path == font_path) return true;
    if (faces_.size() >= 256) return false;
    faces_.push_back(LoadedFace{.face = {}, .hb_font = {}, .path = std::string(font_path)});
    char_index_cache_.clear();
    return true;
}

size_t FontEngine::loaded_face_count() const noexcept {
    return std::count_if(faces_.begin(), faces_.end(), [](const auto& face) { return face.face.valid(); });
}

bool FontEngine::open_fallback(LoadedFace& pending) {
    if (pending.attempted) return pending.face.valid();
    pending.attempted = true; // Failed files are not reopened for every missing character.
    ++face_load_attempts_;
    FT_Face raw = nullptr;
    if (FT_New_Face(ft_lib_.get(), pending.path.c_str(), 0, &raw) != 0 || !raw) return false;
    util::FtFace face(raw);
    if (FT_Set_Char_Size(raw, 0, static_cast<FT_F26Dot6>(pt_size_ * 64.0), dpi_, dpi_) != 0)
        return false;
    pending.hb_font.reset(hb_ft_font_create_referenced(raw));
    pending.face = std::move(face);
    // A new face can cover codepoints that were previously resolved as missing.
    char_index_cache_.clear();
    return true;
}

void FontEngine::compute_metrics() {
    if (faces_.empty()) return;
    FT_Face face = faces_[0].face.get();

    // Compute cell dimensions matching fcft / foot
    int ascent = static_cast<int>(std::ceil(face->size->metrics.ascender / 64.0));
    int descent = static_cast<int>(std::ceil(-face->size->metrics.descender / 64.0));
    int height = static_cast<int>(std::ceil(face->size->metrics.height / 64.0));
    if (height < ascent + descent) {
        height = ascent + descent;
    }

    // Advance width of 'M' or 'W'
    int cell_width = 8;
    FT_UInt glyph_m = FT_Get_Char_Index(face, 'M');
    if (glyph_m != 0 && FT_Load_Glyph(face, glyph_m, FT_LOAD_DEFAULT) == 0) {
        cell_width = static_cast<int>(std::ceil(face->glyph->advance.x / 64.0));
    }
    if (cell_width <= 0) cell_width = 8;
    if (height <= 0) height = 16;

    int underline_pos = static_cast<int>(std::round(-face->underline_position * (face->size->metrics.y_ppem / static_cast<double>(face->units_per_EM))));
    int underline_thick = static_cast<int>(std::round(face->underline_thickness * (face->size->metrics.y_ppem / static_cast<double>(face->units_per_EM))));
    if (underline_thick < 1) underline_thick = 1;
    if (underline_pos < 1) underline_pos = 1;

    metrics_.cell_width = cell_width;
    metrics_.cell_height = height;
    metrics_.ascent = ascent;
    metrics_.descent = descent;
    metrics_.underline_pos = underline_pos;
    metrics_.underline_thickness = underline_thick;
}

FontEngine::CharIndex FontEngine::resolve_char(char32_t cp) {
    CharIndex result{};
    auto cached = char_index_cache_.find(cp);
    if (cached != char_index_cache_.end()) return cached->second;

    for (size_t i = 0; i < faces_.size(); ++i) {
        if (!faces_[i].face.valid() && !open_fallback(faces_[i])) continue;
        const uint32_t glyph = FT_Get_Char_Index(faces_[i].face.get(), cp);
        if (glyph != 0) {
            result.face_idx = static_cast<uint8_t>(i);
            result.glyph_index = glyph;
            break;
        }
    }
    result.resolved = true;
    if (char_index_cache_.size() >= 8192) char_index_cache_.clear();
    char_index_cache_.emplace(cp, result);
    return result;
}

const CachedGlyph* FontEngine::cached_or_rasterize(const GlyphKey& key) {
    auto it = glyph_cache_.find(key);
    if (it != glyph_cache_.end()) {
        ++cache_hits_;
        glyph_lru_.splice(glyph_lru_.begin(), glyph_lru_, it->second.recency);
        return &it->second.glyph;
    }
    ++cache_misses_;
    return rasterize_glyph(key.face_idx, key.glyph_index, key.bold, key.italic);
}

const CachedGlyph* FontEngine::get_glyph(char32_t cp, bool bold, bool italic) {
    if (faces_.empty()) return nullptr;
    transient_glyph_ = CachedGlyph{};
    transient_bitmap_bytes_ = 0;

    const CharIndex resolved = resolve_char(cp);
    return cached_or_rasterize(GlyphKey{
        .glyph_index = resolved.glyph_index,
        .face_idx = resolved.face_idx,
        .bold = bold,
        .italic = italic
    });
}

const CachedGlyph* FontEngine::glyph_for_index(uint8_t face_idx, uint32_t glyph_index, bool bold, bool italic) {
    if (face_idx >= faces_.size()) return nullptr;
    transient_glyph_ = CachedGlyph{};
    transient_bitmap_bytes_ = 0;
    return cached_or_rasterize(GlyphKey{
        .glyph_index = glyph_index,
        .face_idx = face_idx,
        .bold = bold,
        .italic = italic
    });
}

const ShapedCluster* FontEngine::shape_cluster(std::span<const char32_t> cluster, bool bold, bool italic) {
    if (cluster.empty() || faces_.empty()) return nullptr;

    ShapedClusterKey key{};
    key.length = static_cast<uint8_t>(std::min<size_t>(cluster.size(), key.text.size()));
    std::copy_n(cluster.begin(), key.length, key.text.begin());
    key.bold = bold;
    key.italic = italic;
    if (auto it = shaped_cache_.find(key); it != shaped_cache_.end()) return &it->second;

    // Shape with the face that covers the base codepoint (combining marks are
    // positioned by HarfBuzz anchors, which is what makes them land correctly).
    const CharIndex base = resolve_char(cluster.front());
    const uint8_t face_idx = base.face_idx < faces_.size() ? base.face_idx : 0;
    const auto& face = faces_[face_idx];
    if (!face.hb_font.valid() || !face.hb_font.get()) return nullptr;

    static_assert(sizeof(char32_t) == sizeof(uint32_t));
    hb_buffer_t* buffer = hb_buffer_create();
    if (!buffer) return nullptr;
    hb_buffer_add_utf32(buffer, reinterpret_cast<const uint32_t*>(cluster.data()),
        static_cast<int>(cluster.size()), 0, static_cast<int>(cluster.size()));
    hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(face.hb_font.get(), buffer, nullptr, 0);

    unsigned count = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer, &count);
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);
    ShapedCluster shaped{.face_idx = face_idx, .glyphs = {}};
    if (info && positions) {
        shaped.glyphs.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            shaped.glyphs.push_back(ShapedGlyph{
                .glyph_index = info[i].codepoint,
                .x_offset = positions[i].x_offset / 64,
                .y_offset = positions[i].y_offset / 64,
                .x_advance = positions[i].x_advance / 64,
                .y_advance = positions[i].y_advance / 64
            });
        }
    }
    hb_buffer_destroy(buffer);
    if (shaped.glyphs.empty()) return nullptr;

    if (shaped_cache_.size() >= 512) shaped_cache_.clear();
    auto [it, inserted] = shaped_cache_.emplace(std::move(key), std::move(shaped));
    return &it->second;
}

const CachedGlyph* FontEngine::rasterize_glyph(uint8_t face_idx, uint32_t glyph_index, bool bold, bool italic) {
    if (face_idx >= faces_.size()) return nullptr;
    FT_Face face = faces_[face_idx].face.get();

    FT_Int32 load_flags = FT_LOAD_DEFAULT;
    FT_Render_Mode render_mode = FT_RENDER_MODE_LIGHT;

    switch (effective_antialias_mode()) {
        case AntialiasMode::Grayscale:
            load_flags |= FT_LOAD_TARGET_LIGHT;
            render_mode = FT_RENDER_MODE_LIGHT;
            break;
        case AntialiasMode::SubpixelLcd:
            load_flags |= FT_LOAD_TARGET_LCD;
            render_mode = FT_RENDER_MODE_LCD;
            break;
        case AntialiasMode::None:
            load_flags |= FT_LOAD_TARGET_MONO | FT_LOAD_MONOCHROME;
            render_mode = FT_RENDER_MODE_MONO;
            break;
    }

    if (FT_Load_Glyph(face, glyph_index, load_flags) != 0) {
        return nullptr;
    }

    if (bold && !(face->style_flags & FT_STYLE_FLAG_BOLD)) {
        FT_GlyphSlot_Embolden(face->glyph);
    }
    if (italic && !(face->style_flags & FT_STYLE_FLAG_ITALIC)) {
        FT_GlyphSlot_Oblique(face->glyph);
    }

    if (FT_Render_Glyph(face->glyph, render_mode) != 0) {
        return nullptr;
    }

    const FT_Bitmap& bitmap = face->glyph->bitmap;
    CachedGlyph cached;
    cached.bearing_x = face->glyph->bitmap_left;
    cached.bearing_y = face->glyph->bitmap_top;

    if (bitmap.width == 0 || bitmap.rows == 0) {
        // Empty glyph (e.g. space)
        cached.width = 0;
        cached.height = 0;
        cached.is_subpixel = false;
        GlyphKey key{
            .glyph_index = glyph_index,
            .face_idx = face_idx,
            .bold = bold,
            .italic = italic
        };
        return cache_glyph(key, std::move(cached));
    }

    if (bitmap.pixel_mode == FT_PIXEL_MODE_LCD) {
        // Horizontal RGB: bitmap.width is 3 * pixel_width
        int pixel_width = bitmap.width / 3;
        int pixel_height = bitmap.rows;
        cached.width = pixel_width;
        cached.height = pixel_height;
        cached.is_subpixel = true;

        pixman_image_t* raw_img = pixman_image_create_bits(
            PIXMAN_a8r8g8b8, pixel_width, pixel_height, nullptr, pixel_width * 4);
        if (!raw_img) return nullptr;

        cached.mask.reset(raw_img);
        pixman_image_set_component_alpha(cached.mask.get(), true);

        uint32_t* dst = cached.mask.data();
        int dst_stride = cached.mask.stride() / 4;

        for (int y = 0; y < pixel_height; ++y) {
            const uint8_t* src_row = bitmap.buffer + y * bitmap.pitch;
            uint32_t* dst_row = dst + y * dst_stride;
            for (int x = 0; x < pixel_width; ++x) {
                uint8_t r = src_row[x * 3 + 0];
                uint8_t g = src_row[x * 3 + 1];
                uint8_t b = src_row[x * 3 + 2];
                // Component alpha mask: A is max component (or 0xFF), RGB are subpixel alphas
                uint8_t a = std::max({r, g, b});
                dst_row[x] = (static_cast<uint32_t>(a) << 24) |
                             (static_cast<uint32_t>(r) << 16) |
                             (static_cast<uint32_t>(g) << 8) | b;
            }
        }
    } else if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
        int pixel_width = bitmap.width;
        int pixel_height = bitmap.rows;
        cached.width = pixel_width;
        cached.height = pixel_height;
        cached.is_subpixel = false;

        pixman_image_t* raw_img = pixman_image_create_bits(
            PIXMAN_a8, pixel_width, pixel_height, nullptr, pixel_width);
        if (!raw_img) return nullptr;

        cached.mask.reset(raw_img);
        uint8_t* dst = reinterpret_cast<uint8_t*>(cached.mask.data());
        int dst_stride = cached.mask.stride();

        for (int y = 0; y < pixel_height; ++y) {
            const uint8_t* src_row = bitmap.buffer + y * bitmap.pitch;
            uint8_t* dst_row = dst + y * dst_stride;
            std::copy_n(src_row, pixel_width, dst_row);
        }
    } else if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
        int pixel_width = bitmap.width;
        int pixel_height = bitmap.rows;
        cached.width = pixel_width;
        cached.height = pixel_height;
        cached.is_subpixel = false;

        pixman_image_t* raw_img = pixman_image_create_bits(
            PIXMAN_a8, pixel_width, pixel_height, nullptr, pixel_width);
        if (!raw_img) return nullptr;

        cached.mask.reset(raw_img);
        uint8_t* dst = reinterpret_cast<uint8_t*>(cached.mask.data());
        int dst_stride = cached.mask.stride();

        for (int y = 0; y < pixel_height; ++y) {
            const uint8_t* src_row = bitmap.buffer + y * bitmap.pitch;
            uint8_t* dst_row = dst + y * dst_stride;
            for (int x = 0; x < pixel_width; ++x) {
                bool bit = (src_row[x / 8] & (0x80 >> (x % 8))) != 0;
                dst_row[x] = bit ? 0xFF : 0x00;
            }
        }
    }

    GlyphKey key{
        .glyph_index = glyph_index,
        .face_idx = face_idx,
        .bold = bold,
        .italic = italic
    };
    return cache_glyph(key, std::move(cached));
}

} // namespace gritty::render
