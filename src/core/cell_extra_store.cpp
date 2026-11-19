#include "core/cell_extra_store.hpp"
#include "core/combining_store.hpp"
#include <algorithm>
#include <bit>
#include <limits>

namespace gritty::core {

CellExtraStore::CellExtraStore(const CellExtraStore& other)
    : entries_(other.entries_), slots_(other.slots_), free_ids_(other.free_ids_),
      active_(other.active_), inserted_entries_(other.inserted_entries_) {}

void CellExtraStore::swap(CellExtraStore& other) noexcept {
    entries_.swap(other.entries_);
    slots_.swap(other.slots_);
    free_ids_.swap(other.free_ids_);
    std::swap(active_, other.active_);
    std::swap(inserted_entries_, other.inserted_entries_);
}

CellExtraStore& CellExtraStore::operator=(const CellExtraStore& other) {
    if (this != &other) { CellExtraStore copy(other); swap(copy); }
    return *this;
}

CellExtraStore::CellExtraStore(CellExtraStore&& other) noexcept { swap(other); }
CellExtraStore& CellExtraStore::operator=(CellExtraStore&& other) noexcept {
    if (this != &other) { CellExtraStore empty; empty.swap(other); swap(empty); }
    return *this;
}

uint64_t CellExtraStore::hash_extra(const CellExtra& extra) noexcept {
    uint64_t h = 14695981039346656037ull;
    const uint32_t words[3] = {extra.combining_idx, extra.underline_color.value, extra.hyperlink_id};
    for (const uint32_t word : words) {
        h ^= word;
        h *= 1099511628211ull;
    }
    return h;
}

uint16_t CellExtraStore::find(const CellExtra& extra, uint64_t hash) const noexcept {
    if (slots_.empty()) return 0;
    const size_t mask = slots_.size() - 1;
    for (size_t slot = hash & mask; slots_[slot] != 0; slot = (slot + 1) & mask) {
        const uint16_t id = slots_[slot];
        if (entries_[id - 1].hash == hash && entries_[id - 1].extra == extra) return id;
    }
    return 0;
}

void CellExtraStore::rehash(size_t capacity) {
    std::vector<uint16_t> slots(std::max<size_t>(32, std::bit_ceil(capacity)), 0);
    const size_t mask = slots.size() - 1;
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].extra.combining_idx == 0 &&
            entries_[i].extra.underline_color.is_default() &&
            entries_[i].extra.hyperlink_id == 0) continue;
        size_t slot = entries_[i].hash & mask;
        while (slots[slot] != 0) slot = (slot + 1) & mask;
        slots[slot] = static_cast<uint16_t>(i + 1);
    }
    slots_.swap(slots);
}

uint16_t CellExtraStore::insert(const CellExtra& extra, uint64_t hash) {
    if (slots_.size() < (active_ + 1) * 2) rehash((active_ + 1) * 2);
    uint16_t id = 0;
    if (!free_ids_.empty()) {
        id = free_ids_.back();
        free_ids_.pop_back();
        entries_[id - 1] = Entry{extra, hash, false};
    } else {
        // ID 0 means "no extra attributes"; degradation beats terminating
        // the terminal when an extreme output stream exhausts the side table.
        if (entries_.size() >= std::numeric_limits<uint16_t>::max()) return 0;
        entries_.push_back(Entry{extra, hash, false});
        id = static_cast<uint16_t>(entries_.size());
    }
    ++active_;
    ++inserted_entries_;
    const size_t mask = slots_.size() - 1;
    size_t slot = hash & mask;
    while (slots_[slot] != 0) slot = (slot + 1) & mask;
    slots_[slot] = id;
    return id;
}

uint16_t CellExtraStore::intern(const CellExtra& extra) {
    if (extra.is_default()) return 0;
    const uint64_t hash = hash_extra(extra);
    if (const uint16_t id = find(extra, hash)) return id;
    return insert(extra, hash);
}

size_t CellExtraStore::storage_bytes() const noexcept {
    return entries_.capacity() * sizeof(Entry) +
        (slots_.capacity() + free_ids_.capacity()) * sizeof(uint16_t);
}

void CellExtraStore::begin_collection() noexcept {
    for (auto& entry : entries_) entry.marked = false;
}

void CellExtraStore::mark(uint16_t id) noexcept {
    if (id != 0 && id <= entries_.size()) entries_[id - 1].marked = true;
}

void CellExtraStore::mark_combining(CombiningStore& combining) const noexcept {
    for (const auto& entry : entries_)
        if (entry.marked && entry.extra.combining_idx != 0)
            combining.mark(entry.extra.combining_idx);
}

void CellExtraStore::clear_hyperlink(uint16_t hyperlink_id) noexcept {
    bool changed = false;
    for (auto& entry : entries_) {
        if (entry.extra.hyperlink_id != hyperlink_id) continue;
        entry.extra.hyperlink_id = 0;
        entry.hash = hash_extra(entry.extra);
        changed = true;
    }
    if (!changed || slots_.empty()) return;
    std::fill(slots_.begin(), slots_.end(), uint16_t{0});
    const size_t mask = slots_.size() - 1;
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].extra.is_default()) continue;
        size_t slot = entries_[i].hash & mask;
        while (slots_[slot] != 0) slot = (slot + 1) & mask;
        slots_[slot] = static_cast<uint16_t>(i + 1);
    }
}

void CellExtraStore::sweep() {
    for (auto& entry : entries_) {
        if (!entry.marked && !entry.extra.is_default()) entry.extra = CellExtra{};
    }
    while (!entries_.empty() && entries_.back().extra.is_default()) entries_.pop_back();
    free_ids_.clear();
    active_ = 0;
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (!entries_[i].extra.is_default()) ++active_;
        else free_ids_.push_back(static_cast<uint16_t>(i + 1));
    }
    rehash(std::max<size_t>(32, active_ * 2));
    if (entries_.empty()) {
        std::vector<Entry>().swap(entries_);
        std::vector<uint16_t>().swap(slots_);
        std::vector<uint16_t>().swap(free_ids_);
    }
    inserted_entries_ = 0;
}

} // namespace gritty::core
