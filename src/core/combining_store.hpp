#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace gritty::core {

// Grid-owned, collision-checked interning. IDs remain stable while referenced
// by a live cell. Collection frees unreachable entries without renumbering cells.
class CombiningStore final {
public:
    CombiningStore() = default;
    CombiningStore(const CombiningStore& other);
    CombiningStore& operator=(const CombiningStore& other);
    CombiningStore(CombiningStore&& other) noexcept;
    CombiningStore& operator=(CombiningStore&& other) noexcept;
    uint32_t intern(std::span<const char32_t> text);
    uint32_t append(uint32_t prefix, char32_t cp);
    [[nodiscard]] std::span<const char32_t> get(uint32_t id) const noexcept;
    [[nodiscard]] size_t size() const noexcept { return active_; }
    [[nodiscard]] size_t storage_bytes() const noexcept;
    [[nodiscard]] bool needs_collection() const noexcept {
        return inserted_entries_ >= collection_entries_ || inserted_bytes_ >= collection_bytes_;
    }
    void begin_collection() noexcept;
    void mark(uint32_t id) noexcept;
    void sweep();

private:
    struct Entry {
        std::vector<char32_t> text;
        uint64_t hash{0};
        bool marked{false};
    };
    static constexpr uint64_t hash_seed = 14695981039346656037ull;
    static uint64_t extend_hash(uint64_t hash, char32_t cp) noexcept;
    uint32_t find(uint64_t hash, std::span<const char32_t> prefix, char32_t last, bool append) const noexcept;
    uint32_t insert(uint64_t hash, std::vector<char32_t> text);
    void rehash(size_t capacity);
    void swap(CombiningStore& other) noexcept;

    std::vector<Entry> entries_;
    std::vector<uint32_t> slots_; // Zero = empty, otherwise one-based entry ID.
    std::vector<uint32_t> free_ids_;
    size_t active_{0}, payload_bytes_{0};
    size_t inserted_entries_{0}, inserted_bytes_{0};
    size_t collection_entries_{4096}, collection_bytes_{512 * 1024};
};

} // namespace gritty::core
