// IOSurface layout and attached/in-use lifecycle adapted from:
// gobijan/foot-macos, commit 41e4fc0764312ca75b15029a2d316f302d8c33d4,
// macos/shm.c. Copyright (c) 2019 Daniel Eklöf. MIT license:
// third-party/foot-LICENSE.txt. Rendering remains entirely on the CPU.
#include "platform/macos/iosurface_pool.hpp"
#include <CoreGraphics/CoreGraphics.h>
#include <IOSurface/IOSurface.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <vector>

namespace gritty::platform::macos {
namespace {
struct Statistics {
    std::atomic_size_t bytes{0}, peak{0}, frames{0};
};

IOSurfaceRef create_surface(int width, int height) {
    if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() / 4) return nullptr;
    const size_t stride = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, static_cast<size_t>(width) * 4);
    if (stride == 0 || stride > std::numeric_limits<int>::max() ||
        static_cast<size_t>(height) > std::numeric_limits<size_t>::max() / stride) return nullptr;
    const void* keys[] = {kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement,
                         kIOSurfaceBytesPerRow, kIOSurfacePixelFormat};
    const int64_t numbers[] = {width, height, 4, static_cast<int64_t>(stride), 0x42475241}; // 'BGRA'
    CFNumberRef values[5]{};
    for (size_t i = 0; i < 5; ++i) {
        values[i] = CFNumberCreate(nullptr, kCFNumberSInt64Type, &numbers[i]);
        if (!values[i]) {
            for (auto value : values) if (value) CFRelease(value);
            return nullptr;
        }
    }
    auto properties = CFDictionaryCreate(nullptr, keys, reinterpret_cast<const void**>(values), 5,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    for (auto value : values) CFRelease(value);
    if (!properties) return nullptr;
    auto surface = IOSurfaceCreate(properties);
    CFRelease(properties);
    if (!surface) return nullptr;
    auto color_space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (color_space) {
        auto profile = CGColorSpaceCopyPropertyList(color_space);
        if (profile) { IOSurfaceSetValue(surface, kIOSurfaceColorSpace, profile); CFRelease(profile); }
        CGColorSpaceRelease(color_space);
    }
    return surface;
}

struct Storage {
    IOSurfaceRef surface;
    std::shared_ptr<Statistics> statistics;
    size_t bytes;
    bool locked{false};
    Storage(IOSurfaceRef value, std::shared_ptr<Statistics> stats)
        : surface(value), statistics(std::move(stats)), bytes(IOSurfaceGetAllocSize(value)) {
        const auto live = statistics->bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        statistics->frames.fetch_add(1, std::memory_order_relaxed);
        auto peak = statistics->peak.load(std::memory_order_relaxed);
        while (live > peak && !statistics->peak.compare_exchange_weak(
            peak, live, std::memory_order_relaxed, std::memory_order_relaxed)) {}
    }
    bool lock() {
        if (locked) return true;
        if (IOSurfaceLock(surface, 0, nullptr) != kIOReturnSuccess) return false;
        locked = true;
        return true;
    }
    void unlock() {
        if (locked) { IOSurfaceUnlock(surface, 0, nullptr); locked = false; }
    }
    ~Storage() {
        unlock();
        CFRelease(surface);
        statistics->bytes.fetch_sub(bytes, std::memory_order_relaxed);
        statistics->frames.fetch_sub(1, std::memory_order_relaxed);
    }
};

struct Frame {
    util::PixmanImage pixels;
    Storage* storage{nullptr}; // Owned by the Pixman destroy callback, not this borrowed pointer.
    bool attached{false};
    size_t serial{0};
    util::PixmanRegion stale; // Differences from the latest presented frame.
    Frame(int width, int height, const std::shared_ptr<Statistics>& statistics, bool clear_pixels) {
        auto surface = create_surface(width, height);
        if (!surface) return;
        auto backing = std::make_unique<Storage>(surface, statistics);
        if (!backing->lock()) return;
        auto* data = static_cast<uint32_t*>(IOSurfaceGetBaseAddress(surface));
        const auto stride = IOSurfaceGetBytesPerRow(surface);
        if (!data) return;
        if (clear_pixels) std::memset(data, 0, stride * height);
        pixels.reset(pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, width, height, data, static_cast<int>(stride)));
        if (!pixels.valid()) return;
        stale.union_rect(0, 0, width, height);
        storage = backing.get();
        // A retained raw Pixman reference must keep its IOSurface storage alive
        // across resize and even pool destruction.
        pixman_image_set_destroy_function(pixels.get(),
            [](pixman_image_t*, void* data) { delete static_cast<Storage*>(data); }, backing.release());
    }
    ~Frame() { if (storage) storage->unlock(); }
    bool matches(int width, int height) const {
        return pixels.valid() && pixels.width() == width && pixels.height() == height;
    }
    bool available() const {
        return storage && !attached && !IOSurfaceIsInUse(storage->surface);
    }
};
}

struct IOSurfaceFramePool::Impl {
    // Two frames normally alternate. Allow one more for compositor lag or
    // resize, then apply backpressure instead of allocating without a bound.
    static constexpr size_t max_frames = 3;
    int width{0}, height{0};
    size_t allocations{0}, blocked{0}, serial{0}, copied{0};
    bool drawing{false}, prepared{false};
    std::shared_ptr<Statistics> statistics{std::make_shared<Statistics>()};
    std::vector<std::shared_ptr<Frame>> frames;
    std::shared_ptr<Frame> current, displayed;

    Impl() { frames.reserve(max_frames); }
    void prune(bool idle = false) {
        std::erase_if(frames, [&](const auto& frame) {
            return frame != current && frame != displayed && frame->available() &&
                (idle || !frame->matches(width, height));
        });
    }
    std::shared_ptr<Frame> allocate() {
        if (width <= 0 || height <= 0 || frames.size() >= max_frames) return {};
        // With a presented baseline, the frame's full stale region is copied
        // in get_back_buffer(); zeroing 20 MiB first would be pure waste.
        const bool has_baseline = displayed && displayed->matches(width, height);
        auto frame = std::make_shared<Frame>(width, height, statistics, !has_baseline);
        if (!frame->pixels.valid()) return {};
        ++allocations;
        frames.push_back(frame);
        return frame;
    }
};

IOSurfaceFramePool::IOSurfaceFramePool() : impl_(std::make_unique<Impl>()) {}
IOSurfaceFramePool::~IOSurfaceFramePool() = default;

void IOSurfaceFramePool::resize(int width, int height) {
    auto& p = *impl_;
    if (width == p.width && height == p.height) return;
    p.width = width; p.height = height;
    // The currently committed surface is replaced only once a complete frame
    // has been drawn into the new geometry; the old one stays attached (and
    // pinned by the window server) until that commit releases it.
    p.drawing = p.prepared = false;
    p.current.reset();
    p.prune(true);
    if (width <= 0 || height <= 0) { p.current.reset(); return; }
    if (auto next = p.allocate()) p.current = std::move(next);
}

bool IOSurfaceFramePool::valid() const noexcept {
    return impl_->current && impl_->current->matches(impl_->width, impl_->height);
}
const util::PixmanImage& IOSurfaceFramePool::image() const noexcept { return impl_->current->pixels; }

pixman_image_t* IOSurfaceFramePool::get_back_buffer(bool skip_baseline_copy) {
    auto& p = *impl_;
    p.prune();
    if (p.drawing && valid()) return p.current->pixels.get();
    if (p.prepared || p.width <= 0 || p.height <= 0) return nullptr;
    const std::shared_ptr<Frame>* best = nullptr;
    for (const auto& frame : p.frames) {
        if (frame->matches(p.width, p.height) && frame->available() &&
            (!best || frame->serial > (*best)->serial)) best = &frame;
    }
    std::shared_ptr<Frame> next = best ? *best : p.allocate();
    if (!next || !next->storage->lock()) { ++p.blocked; return nullptr; }

    // foot's reapply_old_damage: repair the selected buffer to the previously
    // presented baseline BEFORE CpuRenderer applies any scroll memmoves. Each
    // buffer accumulates all missed damage, not only the previous commit, so
    // a delayed third buffer cannot resurrect old cursor/text/image pixels.
    if (!skip_baseline_copy && p.displayed && p.displayed->matches(p.width, p.height)) {
        IOSurfaceRef displayed_surface = p.displayed->storage->surface;
        const bool need_read_lock = !p.displayed->storage->locked;
        if (need_read_lock &&
            IOSurfaceLock(displayed_surface, kIOSurfaceLockReadOnly, nullptr) != kIOReturnSuccess) {
            next->storage->unlock();
            ++p.blocked;
            return nullptr;
        }
        int count = 0;
        const auto* boxes = pixman_region32_rectangles(next->stale.get(), &count);
        for (int i = 0; i < count; ++i) {
            const auto& box = boxes[i];
            if (box.y2 <= box.y1 || box.x2 <= box.x1) continue;
            if (box.x1 == 0 && box.x2 == p.width &&
                next->pixels.stride() == p.displayed->pixels.stride()) {
                // Full-width rectangles have no inter-row padding to preserve:
                // copy the whole band in one call (stride * (rows - 1) + row_bytes).
                const size_t stride = static_cast<size_t>(next->pixels.stride());
                const size_t row_bytes = static_cast<size_t>(box.x2 - box.x1) * 4;
                const size_t rows = static_cast<size_t>(box.y2 - box.y1);
                const size_t span_bytes = (rows - 1) * stride + row_bytes;
                const size_t offset = static_cast<size_t>(box.y1) * stride;
                auto* dst = reinterpret_cast<uint8_t*>(next->pixels.data()) + offset;
                const auto* src = reinterpret_cast<const uint8_t*>(p.displayed->pixels.data()) + offset;
                std::memcpy(dst, src, span_bytes);
                p.copied += span_bytes;
                continue;
            }
            const size_t bytes = static_cast<size_t>(box.x2 - box.x1) * 4;
            for (int y = box.y1; y < box.y2; ++y) {
                auto* dst = reinterpret_cast<uint8_t*>(next->pixels.data()) +
                    static_cast<size_t>(y) * next->pixels.stride() + box.x1 * 4;
                const auto* src = reinterpret_cast<const uint8_t*>(p.displayed->pixels.data()) +
                    static_cast<size_t>(y) * p.displayed->pixels.stride() + box.x1 * 4;
                std::memcpy(dst, src, bytes);
                p.copied += bytes;
            }
        }
        if (need_read_lock) IOSurfaceUnlock(displayed_surface, kIOSurfaceLockReadOnly, nullptr);
    }
    next->stale.clear();
    p.current = std::move(next);
    p.drawing = true;
    // During rendering only purge obsolete geometry. Same-size frames stay in
    // the pool up to max_frames, so compositor lag does not destroy/recreate a
    // full-window surface. Idle trim handles long-term reclamation.
    p.prune(false);
    return p.current->pixels.get();
}

IOSurfaceRef IOSurfaceFramePool::prepare_present() {
    auto& p = *impl_;
    if (!valid() || !p.drawing) return nullptr;
    auto& frame = *p.current;
    frame.storage->unlock(); // Publishes CPU writes and updates the surface seed.
    frame.attached = true;   // Protect even before IOSurfaceIsInUse becomes true.
    p.drawing = false;
    p.prepared = true;
    return frame.storage->surface;
}

void IOSurfaceFramePool::did_present(const util::PixmanRegion* damage) {
    auto& p = *impl_;
    if (!p.prepared) return;
    p.prepared = false;
    if (p.displayed && p.displayed != p.current) p.displayed->attached = false;
    p.displayed = p.current;
    p.current->serial = ++p.serial;
    for (const auto& frame : p.frames) {
        if (frame == p.current || !frame->matches(p.width, p.height)) continue;
        if (!damage || !pixman_region32_union(frame->stale.get(), frame->stale.get(),
                const_cast<pixman_region32_t*>(damage->get()))) {
            frame->stale.clear();
            frame->stale.union_rect(0, 0, p.width, p.height);
        }
        // Clip caller damage and bound region complexity/storage under long
        // backpressure. Falling back to a full copy never drops baseline ink.
        if (!pixman_region32_intersect_rect(frame->stale.get(), frame->stale.get(), 0, 0, p.width, p.height) ||
            pixman_region32_n_rects(frame->stale.get()) > 64) {
            frame->stale.clear();
            frame->stale.union_rect(0, 0, p.width, p.height);
        }
    }
    p.prune();
}

void IOSurfaceFramePool::detach_displayed() {
    if (impl_->displayed) impl_->displayed->attached = false;
    impl_->displayed.reset();
    impl_->prune(true);
}
void IOSurfaceFramePool::release_spare_buffers() { impl_->prune(true); }
size_t IOSurfaceFramePool::allocation_count() const noexcept { return impl_->allocations; }
size_t IOSurfaceFramePool::live_bytes() const noexcept { return impl_->statistics->bytes.load(std::memory_order_relaxed); }
size_t IOSurfaceFramePool::peak_bytes() const noexcept { return impl_->statistics->peak.load(std::memory_order_relaxed); }
size_t IOSurfaceFramePool::live_frames() const noexcept { return impl_->statistics->frames.load(std::memory_order_relaxed); }
size_t IOSurfaceFramePool::blocked_acquisitions() const noexcept { return impl_->blocked; }
size_t IOSurfaceFramePool::copied_bytes() const noexcept { return impl_->copied; }
bool IOSurfaceFramePool::has_pending_releases() const noexcept {
    return std::any_of(impl_->frames.begin(), impl_->frames.end(), [&](const auto& frame) {
        return frame != impl_->current && frame != impl_->displayed;
    });
}

} // namespace gritty::platform::macos
