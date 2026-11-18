#pragma once
#include <array>
#include <algorithm>
#include <atomic>
#include <memory>
#include <limits>
#include "util/raii.hpp"
#include "util/mapped_buffer.hpp"

namespace gritty::platform {

// Recycle a small number of CPU buffers, but never mutate pixels still leased
// by a CGDataProvider. An explicitly retained old image can outlive the pool.
class CpuFramePool final {
    struct Statistics {
        std::atomic_size_t bytes{0}, peak_bytes{0}, frames{0};
    };
    struct Storage {
        util::MappedBuffer mapping;
        std::shared_ptr<Statistics> statistics;
        Storage(size_t bytes, std::shared_ptr<Statistics> stats)
            : mapping(bytes), statistics(std::move(stats)) {
            if (!mapping.valid()) return;
            const auto live = statistics->bytes.fetch_add(bytes) + bytes;
            statistics->frames.fetch_add(1);
            auto peak = statistics->peak_bytes.load();
            while (live > peak && !statistics->peak_bytes.compare_exchange_weak(peak, live)) {}
        }
        ~Storage() {
            if (!mapping.valid()) return;
            statistics->bytes.fetch_sub(mapping.size());
            statistics->frames.fetch_sub(1);
        }
    };
    struct Frame {
        util::PixmanImage pixels;
        std::atomic_size_t readers{0};
        Frame(int width, int height, const std::shared_ptr<Statistics>& stats) {
            if (width <= 0 || height <= 0 || width > (std::numeric_limits<int>::max() - 63) / 4) return;
            const int stride = (width * 4 + 63) & ~63; // Cache-line aligned rows for CPU/CoreGraphics blits.
            if (static_cast<size_t>(height) > std::numeric_limits<size_t>::max() / stride) return;
            auto storage = std::make_unique<Storage>(static_cast<size_t>(stride) * height, stats);
            if (!storage->mapping.valid()) return;
            pixels.reset(pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height,
                static_cast<uint32_t*>(storage->mapping.data()), stride));
            if (!pixels.valid()) return;
            // Attach ownership to the Pixman image itself: even a raw Pixman
            // reference (not only a CGImage lease) may outlive this Frame/pool.
            pixman_image_set_destroy_function(pixels.get(),
                [](pixman_image_t*, void* data) { delete static_cast<Storage*>(data); }, storage.release());
        }
    };
public:
    class Lease final {
        friend class CpuFramePool;
        std::shared_ptr<Frame> frame_;
        explicit Lease(std::shared_ptr<Frame> frame) : frame_(std::move(frame)) {
            frame_->readers.fetch_add(1, std::memory_order_relaxed);
        }
    public:
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        ~Lease() { frame_->readers.fetch_sub(1, std::memory_order_release); }
    };

    CpuFramePool() = default;
    CpuFramePool(const CpuFramePool&) = delete;
    CpuFramePool& operator=(const CpuFramePool&) = delete;

    void resize(int width, int height) {
        width_ = width;
        height_ = height;
        buffers_ = {};
        if (width <= 0 || height <= 0) { current_.reset(); return; }
        current_ = allocate();
        buffers_[0] = current_;
    }
    // Drop only the pool's ownership. Outstanding provider leases (or raw
    // Pixman references) keep their mappings alive until their last release.
    // The current image is retained so incremental damage keeps its baseline.
    void release_spare_buffers() noexcept {
        for (auto& frame : buffers_)
            if (frame != current_) frame.reset();
    }
    [[nodiscard]] bool valid() const noexcept { return current_ && current_->pixels.valid(); }
    [[nodiscard]] const util::PixmanImage& image() const noexcept { return current_->pixels; }
    [[nodiscard]] size_t allocation_count() const noexcept { return allocations_; }
    [[nodiscard]] size_t live_bytes() const noexcept { return statistics_->bytes.load(); }
    [[nodiscard]] size_t peak_bytes() const noexcept { return statistics_->peak_bytes.load(); }
    [[nodiscard]] size_t live_frames() const noexcept { return statistics_->frames.load(); }

    pixman_image_t* get_back_buffer() {
        if (!valid()) return nullptr;
        if (current_->readers.load(std::memory_order_acquire) == 0) return current_->pixels.get();
        std::shared_ptr<Frame> next;
        for (const auto& frame : buffers_) {
            if (frame && frame != current_ && frame->readers.load(std::memory_order_acquire) == 0) {
                next = frame;
                break;
            }
        }
        if (!next) {
            next = allocate();
            if (!next->pixels.valid()) return nullptr;
            auto slot = std::find(buffers_.begin(), buffers_.end(), nullptr);
            if (slot == buffers_.end())
                slot = std::find_if(buffers_.begin(), buffers_.end(), [&](const auto& frame) { return frame != current_; });
            *slot = next;
        }
        pixman_image_composite32(PIXMAN_OP_SRC, current_->pixels.get(), nullptr, next->pixels.get(),
            0, 0, 0, 0, 0, 0, width_, height_);
        current_ = std::move(next);
        return current_->pixels.get();
    }

    [[nodiscard]] std::unique_ptr<Lease> lease() const {
        return valid() ? std::unique_ptr<Lease>(new Lease(current_)) : nullptr;
    }
    static void release_lease(void* lease, const void*, size_t) {
        delete static_cast<Lease*>(lease);
    }
private:
    std::shared_ptr<Frame> allocate() {
        ++allocations_;
        return std::make_shared<Frame>(width_, height_, statistics_);
    }
    int width_{0}, height_{0};
    size_t allocations_{0};
    std::shared_ptr<Statistics> statistics_{std::make_shared<Statistics>()};
    std::array<std::shared_ptr<Frame>, 3> buffers_;
    std::shared_ptr<Frame> current_;
};

} // namespace gritty::platform
