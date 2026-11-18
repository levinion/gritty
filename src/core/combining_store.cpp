#include "core/combining_store.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gritty::core {

CombiningStore::CombiningStore(const CombiningStore& other)
    : entries_(other.entries_), slots_(other.slots_), free_ids_(other.free_ids_), active_(other.active_),
      inserted_entries_(other.inserted_entries_), inserted_bytes_(other.inserted_bytes_),
      collection_entries_(other.collection_entries_), collection_bytes_(other.collection_bytes_) {
    // A copied vector need not preserve its source's capacity.
    for (const auto& entry : entries_) payload_bytes_ += entry.text.capacity() * sizeof(char32_t);
}

void CombiningStore::swap(CombiningStore& other) noexcept {
    entries_.swap(other.entries_); slots_.swap(other.slots_); free_ids_.swap(other.free_ids_);
    std::swap(active_, other.active_); std::swap(payload_bytes_, other.payload_bytes_);
    std::swap(inserted_entries_, other.inserted_entries_); std::swap(inserted_bytes_, other.inserted_bytes_);
    std::swap(collection_entries_, other.collection_entries_); std::swap(collection_bytes_, other.collection_bytes_);
}

CombiningStore& CombiningStore::operator=(const CombiningStore& other) {
    if (this != &other) { CombiningStore copy(other); swap(copy); }
    return *this;
}
CombiningStore::CombiningStore(CombiningStore&& other) noexcept { swap(other); }
CombiningStore& CombiningStore::operator=(CombiningStore&& other) noexcept {
    if (this != &other) { CombiningStore empty; empty.swap(other); swap(empty); }
    return *this;
}

uint64_t CombiningStore::extend_hash(uint64_t hash, char32_t cp) noexcept {
    return (hash ^ static_cast<uint32_t>(cp)) * 1099511628211ull;
}

std::span<const char32_t> CombiningStore::get(uint32_t id) const noexcept {
    if (id == 0 || id > entries_.size()) return {};
    return entries_[id - 1].text;
}

uint32_t CombiningStore::find(uint64_t hash, std::span<const char32_t> prefix,
                             char32_t last, bool append_cp) const noexcept {
    if (slots_.empty()) return 0;
    const size_t mask = slots_.size() - 1;
    for (size_t slot = hash & mask; slots_[slot] != 0; slot = (slot + 1) & mask) {
        const auto id = slots_[slot];
        const auto& entry = entries_[id - 1];
        if (entry.hash == hash && entry.text.size() == prefix.size() + append_cp &&
            std::equal(prefix.begin(), prefix.end(), entry.text.begin()) &&
            (!append_cp || entry.text.back() == last)) return id;
    }
    return 0;
}

void CombiningStore::rehash(size_t capacity) {
    std::vector<uint32_t> slots(std::max<size_t>(16, std::bit_ceil(capacity)), 0);
    const size_t mask = slots.size() - 1;
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].text.empty()) continue;
        size_t slot = entries_[i].hash & mask;
        while (slots[slot] != 0) slot = (slot + 1) & mask;
        slots[slot] = static_cast<uint32_t>(i + 1);
    }
    slots_.swap(slots);
}

uint32_t CombiningStore::insert(uint64_t hash, std::vector<char32_t> text) {
    if (slots_.size() < (active_ + 1) * 2) rehash((active_ + 1) * 2);
    uint32_t id;
    if (!free_ids_.empty()) {
        id = free_ids_.back();
        free_ids_.pop_back();
        entries_[id - 1] = Entry{std::move(text), hash, false};
    } else {
        if (entries_.size() == std::numeric_limits<uint32_t>::max())
            throw std::length_error("combining entry ID space exhausted");
        entries_.push_back(Entry{std::move(text), hash, false});
        id = static_cast<uint32_t>(entries_.size());
    }
    const size_t bytes = entries_[id - 1].text.capacity() * sizeof(char32_t);
    payload_bytes_ += bytes;
    inserted_bytes_ += bytes + sizeof(Entry) + 2 * sizeof(uint32_t);
    ++inserted_entries_;
    ++active_;
    const size_t mask = slots_.size() - 1;
    size_t slot = hash & mask;
    while (slots_[slot] != 0) slot = (slot + 1) & mask;
    slots_[slot] = id;
    return id;
}

uint32_t CombiningStore::intern(std::span<const char32_t> text) {
    if (text.empty()) return 0;
    uint64_t hash = hash_seed;
    for (const auto cp : text) hash = extend_hash(hash, cp);
    if (const auto id = find(hash, text, 0, false)) return id;
    return insert(hash, std::vector<char32_t>(text.begin(), text.end()));
}

uint32_t CombiningStore::append(uint32_t prefix, char32_t cp) {
    const auto text = get(prefix);
    const auto hash = extend_hash(text.empty() ? hash_seed : entries_[prefix - 1].hash, cp);
    if (const auto id = find(hash, text, cp, true)) return id;
    std::vector<char32_t> extended;
    extended.reserve(text.size() + 1);
    extended.insert(extended.end(), text.begin(), text.end());
    extended.push_back(cp);
    return insert(hash, std::move(extended));
}

size_t CombiningStore::storage_bytes() const noexcept {
    return entries_.capacity() * sizeof(Entry) + payload_bytes_ +
        (slots_.capacity() + free_ids_.capacity()) * sizeof(uint32_t);
}

void CombiningStore::begin_collection() noexcept {
    for (auto& entry : entries_) entry.marked = false;
}

void CombiningStore::mark(uint32_t id) noexcept {
    if (id != 0 && id <= entries_.size()) entries_[id - 1].marked = true;
}

void CombiningStore::sweep() {
    for (auto& entry : entries_) {
        if (!entry.marked && !entry.text.empty()) {
            payload_bytes_ -= entry.text.capacity() * sizeof(char32_t);
            std::vector<char32_t>().swap(entry.text);
            --active_;
        }
    }
    while (!entries_.empty() && entries_.back().text.empty()) entries_.pop_back();
    free_ids_.clear();
    if (entries_.empty()) {
        payload_bytes_ = 0;
        std::vector<Entry>().swap(entries_);
        std::vector<uint32_t>().swap(slots_);
        std::vector<uint32_t>().swap(free_ids_);
    } else {
        if (entries_.capacity() > std::max<size_t>(1024, entries_.size() * 2)) entries_.shrink_to_fit();
        // Reuse low IDs first so the next collection can trim a formerly high
        // live ID after its cell is overwritten, rather than retaining holes.
        for (size_t i = entries_.size(); i-- > 0;)
            if (entries_[i].text.empty()) free_ids_.push_back(static_cast<uint32_t>(i + 1));
        if (free_ids_.capacity() > std::max<size_t>(1024, free_ids_.size() * 2)) free_ids_.shrink_to_fit();
        rehash(active_ * 2);
    }
    inserted_entries_ = inserted_bytes_ = 0;
    collection_entries_ = std::max<size_t>(4096, active_);
    collection_bytes_ = std::max<size_t>(512 * 1024, payload_bytes_ + active_ * sizeof(Entry));
}

} // namespace gritty::core
