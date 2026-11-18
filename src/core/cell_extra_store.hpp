#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include "core/screen_grid_cell.hpp"

namespace gritty::core {

// Grid-owned, collision-checked interning for the rare per-cell attributes
// that do not fit in the 16-byte Cell. IDs stay stable while referenced and
// unreachable entries are reclaimed by mark/sweep together with combining text.
class CellExtraStore final {
public:
    CellExtraStore() = default;
    CellExtraStore(const CellExtraStore& other);
    CellExtraStore& operator=(const CellExtraStore& other);
    CellExtraStore(CellExtraStore&& other) noexcept;
    CellExtraStore& operator=(CellExtraStore&& other) noexcept;

    [[nodiscard]] CellExtra get(uint16_t id) const noexcept {
        if (id == 0 || id > entries_.size()) return {};
        return entries_[id - 1].extra;
    }
    uint16_t intern(const CellExtra& extra);
    [[nodiscard]] size_t size() const noexcept { return active_; }
    [[nodiscard]] size_t storage_bytes() const noexcept;
    [[nodiscard]] bool needs_collection() const noexcept {
        return entries_.size() >= 60000 || inserted_entries_ >= 8192;
    }

    void begin_collection() noexcept;
    void mark(uint16_t id) noexcept;
    void mark_combining(class CombiningStore& combining) const noexcept;
    // Clear a hyperlink ID in-place and repair the hash slots without allocating.
    void clear_hyperlink(uint16_t hyperlink_id) noexcept;
    void sweep();

private:
    struct Entry {
        CellExtra extra{};
        uint64_t hash{0};
        bool marked{false};
    };
    static uint64_t hash_extra(const CellExtra& extra) noexcept;
    uint16_t find(const CellExtra& extra, uint64_t hash) const noexcept;
    uint16_t insert(const CellExtra& extra, uint64_t hash);
    void rehash(size_t capacity);
    void swap(CellExtraStore& other) noexcept;

    std::vector<Entry> entries_; // One-based IDs; index 0 is reserved.
    std::vector<uint16_t> slots_; // Zero = empty.
    std::vector<uint16_t> free_ids_;
    size_t active_{0}, inserted_entries_{0};
};

} // namespace gritty::core
