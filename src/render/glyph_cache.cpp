#include "render/font_engine.hpp"
#include <utility>

namespace gritty::render {

void FontEngine::clear_cache() noexcept {
    char_index_cache_.clear();
    shaped_cache_.clear();
    glyph_cache_.clear();
    glyph_lru_.clear();
    cache_bitmap_bytes_ = 0;
    transient_glyph_ = CachedGlyph{};
    transient_bitmap_bytes_ = 0;
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

const CachedGlyph* FontEngine::cache_glyph(const GlyphKey& key, CachedGlyph glyph) {
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
        const auto oldest = glyph_cache_.find(glyph_lru_.back());
        cache_bitmap_bytes_ -= oldest->second.bitmap_bytes;
        glyph_cache_.erase(oldest);
        glyph_lru_.pop_back();
        ++cache_evictions_;
    }
    glyph_lru_.push_front(key);
    try {
        auto [it, inserted] = glyph_cache_.emplace(key,
            GlyphCacheEntry{std::move(glyph), glyph_lru_.begin(), bytes});
        if (!inserted) {
            glyph_lru_.pop_front();
            return &it->second.glyph;
        }
        cache_bitmap_bytes_ += bytes;
        return &it->second.glyph;
    } catch (...) {
        glyph_lru_.pop_front();
        throw;
    }
}

} // namespace gritty::render
