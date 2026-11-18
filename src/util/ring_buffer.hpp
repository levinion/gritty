#pragma once

#include <vector>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <type_traits>
#include <algorithm>

namespace gritty::util {

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t max_capacity = 0)
        : max_capacity_(max_capacity), head_(0), size_(0) {
        if (max_capacity_ > 0) {
            storage_.resize(max_capacity_);
        }
    }

    void set_capacity(size_t new_cap) {
        if (new_cap == max_capacity_) return;

        std::vector<T> new_storage;
        new_storage.resize(new_cap);

        size_t copy_count = std::min(size_, new_cap);
        // Copy most recent items if downsizing
        size_t start_offset = size_ > copy_count ? size_ - copy_count : 0;
        for (size_t i = 0; i < copy_count; ++i) {
            new_storage[i] = std::move((*this)[start_offset + i]);
        }

        storage_ = std::move(new_storage);
        max_capacity_ = new_cap;
        head_ = 0;
        size_ = copy_count;
    }

    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] size_t capacity() const noexcept { return max_capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool full() const noexcept { return size_ == max_capacity_; }

    void clear() noexcept(std::is_nothrow_default_constructible_v<T> && std::is_nothrow_move_assignable_v<T>) {
        for (size_t i = 0; i < size_; ++i) (*this)[i] = T{};
        head_ = 0;
        size_ = 0;
    }

    T& operator[](size_t logical_index) noexcept {
        return storage_[(head_ + logical_index) % max_capacity_];
    }

    const T& operator[](size_t logical_index) const noexcept {
        return storage_[(head_ + logical_index) % max_capacity_];
    }

    T& at(size_t logical_index) {
        if (logical_index >= size_) {
            throw std::out_of_range("RingBuffer::at index out of range");
        }
        return (*this)[logical_index];
    }

    const T& at(size_t logical_index) const {
        if (logical_index >= size_) {
            throw std::out_of_range("RingBuffer::at index out of range");
        }
        return (*this)[logical_index];
    }

    T& front() noexcept {
        return (*this)[0];
    }

    const T& front() const noexcept {
        return (*this)[0];
    }

    T& back() noexcept {
        return (*this)[size_ - 1];
    }

    const T& back() const noexcept {
        return (*this)[size_ - 1];
    }

    // Pushes an item to the back; if full, overwrites the oldest element (advances head)
    void push_back(const T& value) {
        if (max_capacity_ == 0) return;
        if (full()) {
            storage_[head_] = value;
            head_ = (head_ + 1) % max_capacity_;
        } else {
            storage_[(head_ + size_) % max_capacity_] = value;
            ++size_;
        }
    }

    void push_back(T&& value) {
        if (max_capacity_ == 0) return;
        if (full()) {
            storage_[head_] = std::move(value);
            head_ = (head_ + 1) % max_capacity_;
        } else {
            storage_[(head_ + size_) % max_capacity_] = std::move(value);
            ++size_;
        }
    }

    // Donate a row and recover the overwritten slot for reuse. Unlike ordinary
    // push_back, this explicitly returns ownership through the caller's value.
    void exchange_back(T& value) noexcept(std::is_nothrow_swappable_v<T>) {
        if (max_capacity_ == 0) return;
        const auto index = full() ? head_ : (head_ + size_) % max_capacity_;
        using std::swap;
        swap(storage_[index], value);
        if (full()) head_ = (head_ + 1) % max_capacity_;
        else ++size_;
    }

    template <typename... Args>
    T& emplace_back(Args&&... args) {
        if (max_capacity_ == 0) {
            throw std::runtime_error("RingBuffer has zero capacity");
        }
        if (full()) {
            size_t idx = head_;
            head_ = (head_ + 1) % max_capacity_;
            storage_[idx] = T(std::forward<Args>(args)...);
            return storage_[idx];
        } else {
            size_t idx = (head_ + size_) % max_capacity_;
            storage_[idx] = T(std::forward<Args>(args)...);
            ++size_;
            return storage_[idx];
        }
    }

    void pop_front() noexcept(std::is_nothrow_default_constructible_v<T> && std::is_nothrow_move_assignable_v<T>) {
        if (size_ > 0) {
            storage_[head_] = T{};
            head_ = (head_ + 1) % max_capacity_;
            --size_;
        }
    }

    void pop_back() noexcept(std::is_nothrow_default_constructible_v<T> && std::is_nothrow_move_assignable_v<T>) {
        if (size_ > 0) {
            (*this)[size_ - 1] = T{};
            --size_;
        }
    }

private:
    size_t max_capacity_;
    size_t head_;
    size_t size_;
    std::vector<T> storage_;
};

} // namespace gritty::util
