#pragma once
#include <cstddef>
#include <sys/mman.h>
#include <sys/types.h>
#include <utility>
#include <unistd.h>

namespace gritty::util {

// Large pixel storage must return pages on release, not accumulate in malloc's
// cache as successive resize geometries stop matching its cached allocations.
class MappedBuffer final {
public:
    MappedBuffer() noexcept = default;
    explicit MappedBuffer(size_t bytes) noexcept {
        if (bytes == 0) return;
        void* mapping = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping != MAP_FAILED) { data_ = mapping; bytes_ = bytes; }
    }
    // Read-only file mapping. The caller keeps the fd alive until construction
    // returns; ownership of the mapping is held by this object.
    MappedBuffer(int fd, size_t bytes, off_t offset = 0) noexcept {
        if (fd < 0 || bytes == 0) return;
        void* mapping = ::mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, offset);
        if (mapping != MAP_FAILED) { data_ = mapping; bytes_ = bytes; }
    }
    ~MappedBuffer() { reset(); }
    MappedBuffer(const MappedBuffer&) = delete;
    MappedBuffer& operator=(const MappedBuffer&) = delete;
    MappedBuffer(MappedBuffer&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)), bytes_(std::exchange(other.bytes_, 0)) {}
    MappedBuffer& operator=(MappedBuffer&& other) noexcept {
        if (this != &other) {
            reset();
            data_ = std::exchange(other.data_, nullptr);
            bytes_ = std::exchange(other.bytes_, 0);
        }
        return *this;
    }
    [[nodiscard]] void* data() const noexcept { return data_; }
    [[nodiscard]] size_t size() const noexcept { return bytes_; }
    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }
private:
    void reset() noexcept {
        if (data_) ::munmap(data_, bytes_);
        data_ = nullptr;
        bytes_ = 0;
    }
    void* data_{nullptr};
    size_t bytes_{0};
};

} // namespace gritty::util
