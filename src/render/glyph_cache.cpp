#include "render/font_engine.hpp"
#include <algorithm>
#include <iterator>
#include <utility>

namespace gritty::render {

void FontEngine::clear_cache() noexcept {
    for (auto& cache : char_index_cache_) cache.clear();
    shaped_cache_.clear();
    glyph_cache_.clear();
    glyph_lru_.clear();
    cache_bitmap_bytes_ = 0;
    transient_glyph_ = CachedGlyph{};
    transient_bitmap_bytes_ = 0;
    for (auto& row : ascii_glyph_table_)
        for (auto& slot : row) slot = {};
    for (int style = 0; style < 4; ++style)
        for (size_t slot = 0; slot < 4096; ++slot) {
            non_ascii_l1_[style][slot] = nullptr;
            non_ascii_l1_cp_[style][slot] = 0;
        }
}

void FontEngine::set_cache_limits(GlyphCacheLimits limits) noexcept {
    clear_cache();
    cache_limits_ = limits;
}

GlyphCacheStats FontEngine::cache_stats() const noexcept {
    return {.entries = glyph_cache_.size(), .bitmap_bytes = cache_bitmap_bytes_,
        .transient_bitmap_bytes = transient_bitmap_bytes_, .hits = cache_hits_,
        .misses = cache_misses_, .evictions = cache_evictions_};
}

const CachedGlyph* FontEngine::cache_glyph(const GlyphKey& key, CachedGlyph glyph,
                                           GlyphCacheEntry** entry_out) {
    if (entry_out) *entry_out = nullptr;
    const size_t bytes = static_cast<size_t>(glyph.mask.stride()) * glyph.mask.height();
    if (cache_limits_.max_entries == 0 || bytes > cache_limits_.max_bitmap_bytes) {
        // Do not suppress or degrade a large glyph merely to fit the cache.
        // Keep one explicitly accounted transient result until the next lookup.
        transient_glyph_ = std::move(glyph);
        transient_bitmap_bytes_ = bytes;
        return &transient_glyph_;
    }
    while (glyph_cache_.size() >= cache_limits_.max_entries ||
           cache_bitmap_bytes_ > cache_limits_.max_bitmap_bytes - bytes) {
        auto oldest = glyph_cache_.find(glyph_lru_.back());
        while (glyph_lru_.size() > 1 && oldest->second.referenced) {
            oldest->second.referenced = false;
            glyph_lru_.splice(glyph_lru_.begin(), glyph_lru_, oldest->second.recency);
            oldest = glyph_cache_.find(glyph_lru_.back());
        }
        auto& old_entry = oldest->second;
        const CachedGlyph* evicted = &old_entry.glyph;
        if (old_entry.l1_kind == 1) {
            auto& slot = ascii_glyph_table_[old_entry.l1_style][old_entry.l1_slot];
            if (slot.entry == &old_entry) slot.entry = nullptr;
        } else if (old_entry.l1_kind == 2) {
            if (non_ascii_l1_[old_entry.l1_style][old_entry.l1_slot] == evicted) {
                non_ascii_l1_[old_entry.l1_style][old_entry.l1_slot] = nullptr;
                non_ascii_l1_cp_[old_entry.l1_style][old_entry.l1_slot] = 0;
            }
        } else if (old_entry.l1_kind == 3) {
            for (auto& row : ascii_glyph_table_)
                for (auto& slot : row)
                    if (slot.entry == &old_entry) slot.entry = nullptr;
            for (int style = 0; style < 4; ++style)
                for (size_t slot = 0; slot < 4096; ++slot)
                    if (non_ascii_l1_[style][slot] == evicted) {
                        non_ascii_l1_[style][slot] = nullptr;
                        non_ascii_l1_cp_[style][slot] = 0;
                    }
        }
        cache_bitmap_bytes_ -= old_entry.bitmap_bytes;
        glyph_cache_.erase(oldest);
        glyph_lru_.pop_back();
        ++cache_evictions_;
    }
    glyph_lru_.push_front(key);
    auto [it, inserted] = glyph_cache_.emplace(key,
        GlyphCacheEntry{std::move(glyph), glyph_lru_.begin(), bytes, 0, 0, 0, false});
    if (!inserted) {
        glyph_lru_.pop_front();
        if (entry_out) *entry_out = &it->second;
        return &it->second.glyph;
    }
    cache_bitmap_bytes_ += bytes;
    if (entry_out) *entry_out = &it->second;
    return &it->second.glyph;
}

} // namespace gritty::render
