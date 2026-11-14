#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <new>

namespace squirt::util {

class ScratchArena {
public:
    explicit ScratchArena(size_t initial_capacity = 64 * 1024)
        : buffer_(initial_capacity), offset_(0) {}

    ~ScratchArena() = default;
    ScratchArena(const ScratchArena&) = delete;
    ScratchArena& operator=(const ScratchArena&) = delete;

    ScratchArena(ScratchArena&& other) noexcept
        : buffer_(std::move(other.buffer_)), offset_(other.offset_) {
        other.offset_ = 0;
    }

    ScratchArena& operator=(ScratchArena&& other) noexcept {
        if (this != &other) {
            buffer_ = std::move(other.buffer_);
            offset_ = other.offset_;
            other.offset_ = 0;
        }
        return *this;
    }

    void* allocate(size_t bytes, size_t alignment = alignof(std::max_align_t)) {
        size_t current = reinterpret_cast<uintptr_t>(buffer_.data() + offset_);
        size_t aligned = (current + alignment - 1) & ~(alignment - 1);
        size_t new_offset = (aligned - reinterpret_cast<uintptr_t>(buffer_.data())) + bytes;

        if (new_offset > buffer_.size()) {
            size_t new_size = std::max(buffer_.size() * 2, new_offset);
            buffer_.resize(new_size);
            current = reinterpret_cast<uintptr_t>(buffer_.data() + offset_);
            aligned = (current + alignment - 1) & ~(alignment - 1);
            new_offset = (aligned - reinterpret_cast<uintptr_t>(buffer_.data())) + bytes;
        }

        offset_ = new_offset;
        return reinterpret_cast<void*>(aligned);
    }

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        void* mem = allocate(sizeof(T), alignof(T));
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    void reset() noexcept {
        offset_ = 0;
    }

    [[nodiscard]] size_t used_bytes() const noexcept {
        return offset_;
    }

    [[nodiscard]] size_t capacity() const noexcept {
        return buffer_.size();
    }

private:
    std::vector<std::byte> buffer_;
    size_t offset_;
};

} // namespace squirt::util
