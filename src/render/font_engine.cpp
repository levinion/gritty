#include "render/font_engine.hpp"
#include <ft2build.h>
#include FT_OUTLINE_H
#include FT_SYNTHESIS_H
#include <algorithm>
#include <cmath>
#include <cstring>

namespace gritty::render {
namespace {
constexpr size_t non_ascii_l1_slot(char32_t cp) noexcept {
    return static_cast<size_t>((static_cast<uint32_t>(cp) * 2654435761u) >> 20) & 0xfffu;
}

constexpr bool is_nerd_font_icon(char32_t cp) noexcept {
    // Powerline separators (U+E0B0..U+E0D7), progress bars (U+EE00..U+EE0B),
    // and Kitty Unicode placeholder (U+10EEEE) must stay at native geometry.
    if ((cp >= 0xE0B0 && cp <= 0xE0D7) || (cp >= 0xEE00 && cp <= 0xEE0B) || cp == 0x10EEEE)
        return false;
    return (cp >= 0xE000 && cp <= 0xF8FF) ||
           (cp >= 0xF0000 && cp <= 0xFFFFD) ||
           (cp >= 0x100000 && cp <= 0x10FFFD);
}

bool apply_face_size(FT_Face face, double pt_size, unsigned int dpi) noexcept {
    if (!face || dpi == 0) return false;
    if (FT_IS_SCALABLE(face)) {
        const FT_F26Dot6 char_size = static_cast<FT_F26Dot6>(pt_size * 64.0);
        return FT_Set_Char_Size(face, 0, char_size, dpi, dpi) == 0;
    }
    if (face->num_fixed_sizes > 0 && face->available_sizes) {
        const double target_px = pt_size * static_cast<double>(dpi) / 72.0;
        int best_idx = 0;
        double best_diff = std::abs(static_cast<double>(face->available_sizes[0].height) - target_px);
        bool best_ge = face->available_sizes[0].height >= target_px;
        for (int i = 1; i < face->num_fixed_sizes; ++i) {
            const double h = static_cast<double>(face->available_sizes[i].height);
            const bool ge = h >= target_px;
            const double diff = std::abs(h - target_px);
            if ((ge && !best_ge) || (ge == best_ge && diff < best_diff)) {
                best_idx = i;
                best_diff = diff;
                best_ge = ge;
            }
        }
        return FT_Select_Size(face, best_idx) == 0;
    }
    const FT_F26Dot6 char_size = static_cast<FT_F26Dot6>(pt_size * 64.0);
    return FT_Set_Char_Size(face, 0, char_size, dpi, dpi) == 0;
}

util::FtFace open_ft_face(FT_Library lib, const std::string& path, const std::string& postscript_name) {
    FT_Face raw = nullptr;
    if (FT_New_Face(lib, path.c_str(), 0, &raw) != 0 || !raw) return {};
    util::FtFace face(raw);
    if (!postscript_name.empty() && raw->num_faces > 1) {
        const char* ps0 = FT_Get_Postscript_Name(raw);
        if (!ps0 || postscript_name != ps0) {
            for (FT_Long idx = 1; idx < raw->num_faces; ++idx) {
                FT_Face cand = nullptr;
                if (FT_New_Face(lib, path.c_str(), idx, &cand) == 0 && cand) {
                    const char* ps = FT_Get_Postscript_Name(cand);
                    if (ps && postscript_name == ps) {
                        face.reset(cand);
                        break;
                    }
                    FT_Done_Face(cand);
                }
            }
        }
    }
    return face;
}
} // namespace

FontEngine::FontEngine(AntialiasMode antialias, GlyphCacheLimits limits)
    : antialias_mode_(antialias), cache_limits_(limits) {
    if (ft_lib_.valid()) {
        FT_Library_SetLcdFilter(ft_lib_.get(), FT_LCD_FILTER_DEFAULT);
    }
}

bool FontEngine::load_font(std::string_view font_path, double pt_size, unsigned int dpi,
                           std::string_view postscript_name) {
    if (!ft_lib_.valid()) return false;

    pt_size_ = pt_size;
    dpi_ = dpi;

    const std::string path_str(font_path);
    const std::string ps_str(postscript_name);
    util::FtFace ft_face = open_ft_face(ft_lib_.get(), path_str, ps_str);
    if (!ft_face.valid()) return false;
    if (!apply_face_size(ft_face.get(), pt_size_, dpi_)) return false;

    hb_font_t* hb_font = hb_ft_font_create_referenced(ft_face.get());

    faces_.clear();
    style_faces_ = {};
    faces_.push_back(LoadedFace{
        .face = std::move(ft_face),
        .hb_font = util::HbFont(hb_font),
        .path = path_str,
        .postscript_name = ps_str,
        .attempted = true
    });
    face_load_attempts_ = 1;

    compute_metrics();
    clear_cache();
    return true;
}

bool FontEngine::update_size_and_dpi(double pt_size, unsigned int dpi) {
    if (faces_.empty() || dpi == 0 || !std::isfinite(pt_size) || pt_size < 1.0 || pt_size > 200.0) {
        return false;
    }
    if (dpi_ == dpi && std::abs(pt_size_ - pt_size) < 1e-6) return true;
    if (faces_[0].face.valid() && !apply_face_size(faces_[0].face.get(), pt_size, dpi)) {
        return false;
    }
    pt_size_ = pt_size;
    dpi_ = dpi;
    if (faces_[0].face.valid() && faces_[0].hb_font.valid()) {
        hb_ft_font_changed(faces_[0].hb_font.get());
    }
    for (size_t i = 1; i < faces_.size(); ++i) {
        if (faces_[i].face.valid()) {
            apply_face_size(faces_[i].face.get(), pt_size_, dpi_);
            if (faces_[i].hb_font.valid()) hb_ft_font_changed(faces_[i].hb_font.get());
        }
    }
    for (auto& face : style_faces_) {
        if (face.face.valid()) {
            apply_face_size(face.face.get(), pt_size_, dpi_);
            if (face.hb_font.valid()) hb_ft_font_changed(face.hb_font.get());
        }
    }
    compute_metrics();
    clear_cache();
    return true;
}

bool FontEngine::set_size(double pt_size) {
    return update_size_and_dpi(pt_size, dpi_);
}

bool FontEngine::set_dpi(unsigned int dpi) {
    return update_size_and_dpi(pt_size_, dpi);
}

bool FontEngine::set_style_font(bool bold, bool italic, std::string_view font_path,
                                std::string_view postscript_name) {
    if (!ft_lib_.valid() || faces_.empty() || font_path.empty() || (!bold && !italic)) return false;
    const size_t style = static_cast<size_t>(bold) * 2 + (italic ? 1 : 0);
    style_faces_[style - 1] = LoadedFace{
        .face = {},
        .hb_font = {},
        .path = std::string(font_path),
        .postscript_name = std::string(postscript_name),
        .attempted = false
    };
    clear_cache();
    return true;
}

bool FontEngine::add_fallback_font(std::string_view font_path, std::string_view postscript_name) {
    if (!ft_lib_.valid() || faces_.empty() || font_path.empty()) return false;
    // uint8_t fallback slots: keep faces_.size() < 256 so style faces stay at >= 256.
    for (size_t i = 1; i < faces_.size(); ++i)
        if (faces_[i].path == font_path && faces_[i].postscript_name == postscript_name) return true;
    if (faces_.size() >= 256) return false;
    faces_.push_back(LoadedFace{
        .face = {},
        .hb_font = {},
        .path = std::string(font_path),
        .postscript_name = std::string(postscript_name)
    });
    clear_cache();
    return true;
}

FontEngine::LoadedFace* FontEngine::face_for_idx(uint16_t face_idx) noexcept {
    if (face_idx < faces_.size()) return &faces_[face_idx];
    if (face_idx >= kStyleFaceOffset && face_idx < kStyleFaceOffset + style_faces_.size()) {
        return &style_faces_[face_idx - kStyleFaceOffset];
    }
    return nullptr;
}

size_t FontEngine::loaded_face_count() const noexcept {
    return std::count_if(faces_.begin(), faces_.end(), [](const auto& face) { return face.face.valid(); });
}

bool FontEngine::open_fallback(LoadedFace& pending) {
    if (pending.attempted) return pending.face.valid();
    pending.attempted = true; // Failed files are not reopened for every missing character.
    ++face_load_attempts_;
    util::FtFace face = open_ft_face(ft_lib_.get(), pending.path, pending.postscript_name);
    if (!face.valid()) return false;
    if (!apply_face_size(face.get(), pt_size_, dpi_)) return false;
    pending.hb_font.reset(hb_ft_font_create_referenced(face.get()));
    pending.face = std::move(face);
    // A new face can cover codepoints that were previously resolved as missing.
    for (auto& cache : char_index_cache_) {
        std::erase_if(cache, [](const auto& kv) { return kv.second.glyph_index == 0; });
    }
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

    // Advance width and cap height of 'M'
    int cell_width = 8;
    int cap_height = std::max(1, (ascent * 3) / 4);
    FT_UInt glyph_m = FT_Get_Char_Index(face, 'M');
    if (glyph_m != 0 && FT_Load_Glyph(face, glyph_m, FT_LOAD_DEFAULT) == 0) {
        cell_width = static_cast<int>(std::ceil(face->glyph->advance.x / 64.0));
        if (face->glyph->metrics.height > 0) {
            cap_height = std::max(1, static_cast<int>(std::ceil(face->glyph->metrics.height / 64.0)));
        }
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
    metrics_.cap_height = cap_height;
    metrics_.underline_pos = underline_pos;
    metrics_.underline_thickness = underline_thick;
}

FontEngine::CharIndex FontEngine::resolve_char(char32_t cp, bool bold, bool italic) {
    const size_t style = static_cast<size_t>(bold) * 2 + (italic ? 1 : 0);
    auto& cache = char_index_cache_[style];
    CharIndex result{};
    if (auto cached = cache.find(cp); cached != cache.end()) return cached->second;

    if (style != 0) {
        auto& style_face = style_faces_[style - 1];
        if (!style_face.path.empty()) {
            const bool just_opened = !style_face.attempted;
            if (style_face.face.valid() || open_fallback(style_face)) {
                if (just_opened) cache.clear();
                const uint32_t glyph = FT_Get_Char_Index(style_face.face.get(), cp);
                if (glyph != 0) {
                    result.face_idx = static_cast<uint16_t>(kStyleFaceOffset + (style - 1));
                    result.glyph_index = glyph;
                    result.resolved = true;
                    if (cache.size() >= 65536) cache.clear();
                    cache.emplace(cp, result);
                    return result;
                }
            }
        }
    }

    for (size_t i = 0; i < faces_.size(); ++i) {
        if (!faces_[i].face.valid() && !open_fallback(faces_[i])) continue;
        const uint32_t glyph = FT_Get_Char_Index(faces_[i].face.get(), cp);
        if (glyph != 0) {
            result.face_idx = static_cast<uint16_t>(i);
            result.glyph_index = glyph;
            break;
        }
    }
    result.resolved = true;
    if (cache.size() >= 65536) cache.clear();
    cache.emplace(cp, result);
    return result;
}

const CachedGlyph* FontEngine::cached_or_rasterize(const GlyphKey& key, GlyphCacheEntry** entry_out,
                                                   bool constrain_icon) {
    if (entry_out) *entry_out = nullptr;
    auto it = glyph_cache_.find(key);
    if (it != glyph_cache_.end()) {
        ++cache_hits_;
        if (!it->second.referenced) it->second.referenced = true;
        if (entry_out) *entry_out = &it->second;
        return &it->second.glyph;
    }
    ++cache_misses_;
    return rasterize_glyph(key.face_idx, key.glyph_index, key.bold, key.italic, entry_out, constrain_icon);
}

const CachedGlyph* FontEngine::get_glyph(char32_t cp, bool bold, bool italic) {
    if (faces_.empty()) return nullptr;
    transient_glyph_ = CachedGlyph{};
    transient_bitmap_bytes_ = 0;

    const auto style = static_cast<size_t>(bold) * 2 + (italic ? 1 : 0);
    if (cp > 0x7e) {
        const auto slot = non_ascii_l1_slot(cp);
        if (non_ascii_l1_[style][slot] && non_ascii_l1_cp_[style][slot] == cp) {
            ++cache_hits_; // Direct L1 hit: no unordered_map hash or LRU splice.
            return non_ascii_l1_[style][slot];
        }
    }
    if (cp >= 0x20 && cp <= 0x7e) {
        const auto slot = static_cast<size_t>(cp - 0x20);
        auto& l1 = ascii_glyph_table_[style][slot];
        if (l1.entry) {
            ++cache_hits_;
            if (!l1.entry->referenced) l1.entry->referenced = true;
            return &l1.entry->glyph;
        }
    }

    const CharIndex resolved = resolve_char(cp, bold, italic);
    GlyphCacheEntry* entry = nullptr;
    const CachedGlyph* glyph = cached_or_rasterize(GlyphKey{
        .glyph_index = resolved.glyph_index,
        .face_idx = resolved.face_idx,
        .bold = bold,
        .italic = italic
    }, &entry, is_nerd_font_icon(cp));
    if (entry) {
        if (cp >= 0x20 && cp <= 0x7e) {
            const auto slot = static_cast<uint16_t>(cp - 0x20);
            ascii_glyph_table_[style][slot].entry = entry;
            if (entry->l1_kind == 0) {
                entry->l1_kind = 1;
                entry->l1_style = static_cast<uint8_t>(style);
                entry->l1_slot = slot;
            } else if (!(entry->l1_kind == 1 && entry->l1_style == style && entry->l1_slot == slot)) {
                entry->l1_kind = 3;
            }
        } else if (cp > 0x7e) {
            const auto slot = static_cast<uint16_t>(non_ascii_l1_slot(cp));
            non_ascii_l1_[style][slot] = &entry->glyph;
            non_ascii_l1_cp_[style][slot] = cp;
            if (entry->l1_kind == 0) {
                entry->l1_kind = 2;
                entry->l1_style = static_cast<uint8_t>(style);
                entry->l1_slot = slot;
            } else if (!(entry->l1_kind == 2 && entry->l1_style == style && entry->l1_slot == slot)) {
                entry->l1_kind = 3;
            }
        }
    }
    return glyph;
}

const CachedGlyph* FontEngine::glyph_for_index(uint16_t face_idx, uint32_t glyph_index, bool bold, bool italic) {
    if (!face_for_idx(face_idx)) return nullptr;
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
    const CharIndex base = resolve_char(cluster.front(), bold, italic);
    const LoadedFace* loaded = face_for_idx(base.face_idx);
    const uint16_t face_idx = loaded ? base.face_idx : 0;
    const auto& face = loaded ? *loaded : faces_[0];
    if (!face.hb_font.valid() || !face.hb_font.get()) return nullptr;

    static_assert(sizeof(char32_t) == sizeof(uint32_t));
    shape_buffer_.clear();
    if (!shape_buffer_.valid()) return nullptr;
    hb_buffer_t* buffer = shape_buffer_.get();
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
    if (shaped.glyphs.empty()) return nullptr;

    if (shaped_cache_.size() >= 512) shaped_cache_.clear();
    auto [it, inserted] = shaped_cache_.emplace(std::move(key), std::move(shaped));
    return &it->second;
}

const CachedGlyph* FontEngine::rasterize_glyph(uint16_t face_idx, uint32_t glyph_index,
                                                 bool bold, bool italic, GlyphCacheEntry** entry_out,
                                                 bool constrain_icon) {
    if (entry_out) *entry_out = nullptr;
    LoadedFace* loaded = face_for_idx(face_idx);
    if (!loaded || !loaded->face.valid()) return nullptr;
    FT_Face face = loaded->face.get();

    // FT_LOAD_COLOR is ignored by monochrome faces. On a colour hit FreeType
    // returns FT_PIXEL_MODE_BGRA, handled below as an independent source page.
    FT_Int32 load_flags = FT_LOAD_DEFAULT | FT_LOAD_COLOR;
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

    // Non-Mono Nerd Font PUA icons are designed up to ~1.54x cell width and
    // ~1.27x cap height. Scale oversized vector outlines down cleanly before
    // rasterization so they match single-cell icon proportions without blur.
    bool constrained = false;
    if (constrain_icon && glyph_index != 0 && face->glyph->format == FT_GLYPH_FORMAT_OUTLINE &&
        metrics_.cell_width > 0 && metrics_.cell_height > 0) {
        FT_BBox cbox{};
        FT_Outline_Get_CBox(&face->glyph->outline, &cbox);
        const double bw = static_cast<double>(cbox.xMax - cbox.xMin) / 64.0;
        const double bh = static_cast<double>(cbox.yMax - cbox.yMin) / 64.0;
        const double max_w = static_cast<double>(metrics_.cell_width);
        const double cap_h = static_cast<double>(metrics_.cap_height > 0 ? metrics_.cap_height : metrics_.ascent);
        const double max_h = std::max(1.0, std::min(static_cast<double>(metrics_.cell_height), cap_h * 1.15));
        if (bw > max_w || bh > max_h) {
            const double scale = std::min(bw > 0.0 ? max_w / bw : 1.0,
                                          bh > 0.0 ? max_h / bh : 1.0);
            if (scale > 0.0 && scale < 1.0) {
                const FT_Fixed fscale = static_cast<FT_Fixed>(std::lround(scale * 65536.0));
                FT_Matrix matrix{fscale, 0, 0, fscale};
                FT_Outline_Transform(&face->glyph->outline, &matrix);
                constrained = true;
            }
        }
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
        cached.is_color = false;
        GlyphKey key{
            .glyph_index = glyph_index,
            .face_idx = face_idx,
            .bold = bold,
            .italic = italic
        };
        return cache_glyph(key, std::move(cached), entry_out);
    }

    if (bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
        // Colour emoji: FreeType already delivers premultiplied BGRA pixels.
        // Scale down oversized bitmap strikes (e.g. Apple Color Emoji 160px)
        // to fit within a two-column cell, and keep out of the A8 atlas.
        cached.width = static_cast<int>(bitmap.width);
        cached.height = static_cast<int>(bitmap.rows);
        cached.is_subpixel = false;
        cached.is_color = true;
        const int w = cached.width, h = cached.height;
        pixman_image_t* raw_img = pixman_image_create_bits(
            PIXMAN_a8r8g8b8, w, h, nullptr, w * 4);
        if (!raw_img) return nullptr;
        cached.mask.reset(raw_img);
        auto* dst = cached.mask.data();
        const int dst_stride = cached.mask.stride() / 4;
        for (int y = 0; y < h; ++y) {
            const uint8_t* src_row = bitmap.buffer + static_cast<size_t>(y) * bitmap.pitch;
            uint32_t* dst_row = dst + static_cast<size_t>(y) * dst_stride;
            std::memcpy(dst_row, src_row, static_cast<size_t>(w) * 4);
        }
        const int max_w = std::max(1, metrics_.cell_width * 2);
        const int max_h = std::max(1, metrics_.cell_height);
        if (w > max_w || h > max_h) {
            const double scale = std::min(static_cast<double>(max_w) / w,
                                          static_cast<double>(max_h) / h);
            const int dst_w = std::max(1, static_cast<int>(std::lround(w * scale)));
            const int dst_h = std::max(1, static_cast<int>(std::lround(h * scale)));
            pixman_image_t* scaled = pixman_image_create_bits(
                PIXMAN_a8r8g8b8, dst_w, dst_h, nullptr, dst_w * 4);
            if (scaled) {
                pixman_transform_t transform;
                pixman_transform_init_identity(&transform);
                transform.matrix[0][0] = pixman_double_to_fixed(static_cast<double>(w) / dst_w);
                transform.matrix[1][1] = pixman_double_to_fixed(static_cast<double>(h) / dst_h);
                pixman_image_set_transform(cached.mask.get(), &transform);
                pixman_image_set_filter(cached.mask.get(), PIXMAN_FILTER_BILINEAR, nullptr, 0);
                pixman_image_set_repeat(cached.mask.get(), PIXMAN_REPEAT_PAD);
                pixman_image_composite32(PIXMAN_OP_SRC, cached.mask.get(), nullptr, scaled,
                    0, 0, 0, 0, 0, 0, dst_w, dst_h);
                cached.mask.reset(scaled);
                cached.width = dst_w;
                cached.height = dst_h;
                cached.bearing_x = static_cast<int>(std::lround(cached.bearing_x * scale));
                cached.bearing_y = static_cast<int>(std::lround(cached.bearing_y * scale));
            }
        }
    } else if (bitmap.pixel_mode == FT_PIXEL_MODE_LCD) {
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

    if (constrained && !cached.is_color && cached.width > 0 && cached.height > 0) {
        cached.bearing_x = (metrics_.cell_width - cached.width) / 2;
        const int cap_h = metrics_.cap_height > 0 ? metrics_.cap_height : metrics_.ascent;
        const int top_y = std::clamp(metrics_.ascent - (cap_h + cached.height) / 2,
                                     0, std::max(0, metrics_.cell_height - cached.height));
        cached.bearing_y = metrics_.ascent - top_y;
    }

    GlyphKey key{
        .glyph_index = glyph_index,
        .face_idx = face_idx,
        .bold = bold,
        .italic = italic
    };
    return cache_glyph(key, std::move(cached), entry_out);
}

} // namespace gritty::render
