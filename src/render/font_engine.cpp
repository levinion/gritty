#include "render/font_engine.hpp"
#include <ft2build.h>
#include FT_SYNTHESIS_H
#include <algorithm>
#include <cmath>

namespace gritty::render {

FontEngine::FontEngine(AntialiasMode antialias) : antialias_mode_(antialias) {
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
        .path = std::string(font_path)
    });

    compute_metrics();
    clear_cache();
    return true;
}

bool FontEngine::add_fallback_font(std::string_view font_path) {
    if (!ft_lib_.valid()) return false;

    FT_Face raw_face = nullptr;
    FT_Error err = FT_New_Face(ft_lib_.get(), std::string(font_path).c_str(), 0, &raw_face);
    if (err || !raw_face) return false;

    util::FtFace ft_face(raw_face);
    FT_F26Dot6 char_size = static_cast<FT_F26Dot6>(pt_size_ * 64.0);
    FT_Set_Char_Size(ft_face.get(), 0, char_size, dpi_, dpi_);

    hb_font_t* hb_font = hb_ft_font_create_referenced(ft_face.get());

    faces_.push_back(LoadedFace{
        .face = std::move(ft_face),
        .hb_font = util::HbFont(hb_font),
        .path = std::string(font_path)
    });

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

void FontEngine::clear_cache() noexcept {
    glyph_cache_.clear();
}

const CachedGlyph* FontEngine::get_glyph(char32_t cp, bool bold, bool italic) {
    if (faces_.empty()) return nullptr;

    uint8_t face_idx = 0;
    uint32_t glyph_index = 0;

    for (size_t i = 0; i < faces_.size(); ++i) {
        uint32_t g = FT_Get_Char_Index(faces_[i].face.get(), cp);
        if (g != 0) {
            face_idx = static_cast<uint8_t>(i);
            glyph_index = g;
            break;
        }
    }

    if (glyph_index == 0) {
        // Not found, use .notdef of primary face
        face_idx = 0;
        glyph_index = 0;
    }

    GlyphKey key{
        .glyph_index = glyph_index,
        .face_idx = face_idx,
        .bold = bold,
        .italic = italic
    };

    auto it = glyph_cache_.find(key);
    if (it != glyph_cache_.end()) {
        return &it->second;
    }

    return rasterize_glyph(face_idx, glyph_index, bold, italic);
}

const CachedGlyph* FontEngine::rasterize_glyph(uint8_t face_idx, uint32_t glyph_index, bool bold, bool italic) {
    if (face_idx >= faces_.size()) return nullptr;
    FT_Face face = faces_[face_idx].face.get();

    FT_Int32 load_flags = FT_LOAD_DEFAULT;
    FT_Render_Mode render_mode = FT_RENDER_MODE_LIGHT;

    switch (antialias_mode_) {
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
        auto [it, _] = glyph_cache_.emplace(key, std::move(cached));
        return &it->second;
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
    auto [it, _] = glyph_cache_.emplace(key, std::move(cached));
    return &it->second;
}

} // namespace gritty::render
