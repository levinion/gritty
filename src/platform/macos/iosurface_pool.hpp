#pragma once
#include <cstddef>
#include <memory>
#include <IOSurface/IOSurfaceRef.h>
#include "util/raii.hpp"

namespace gritty::platform::macos {

// CPU-rendered shared surface, published directly as CALayer.contents.
// Adapted from foot-macos macos/shm.c (MIT); see third-party/foot-LICENSE.txt.
//
// Like foot's shm buffer chain: never draw into an attached/in-use surface.
// Reuse released buffers and reapply accumulated old damage before rendering
// this frame (including scroll moves). At most three pool-owned frames; if all
// are busy, return nullptr and let the event loop retry without losing damage.
// A display-link tick is pacing, NOT proof that a surface has been released.
class IOSurfaceFramePool final {
public:
    IOSurfaceFramePool();
    ~IOSurfaceFramePool();
    IOSurfaceFramePool(const IOSurfaceFramePool&) = delete;
    IOSurfaceFramePool& operator=(const IOSurfaceFramePool&) = delete;

    void resize(int width, int height);
    pixman_image_t* get_back_buffer(bool skip_baseline_copy = false); // nullptr = no usable surface for this geometry.
    IOSurfaceRef prepare_present();   // Unlocks CPU access; advances the IOSurface seed.
    void did_present(const util::PixmanRegion* damage = nullptr); // nullptr = full damage.
    void detach_displayed();          // Call after clearing the layer contents.
    void release_spare_buffers();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const util::PixmanImage& image() const noexcept;
    [[nodiscard]] size_t allocation_count() const noexcept;
    [[nodiscard]] size_t live_bytes() const noexcept;
    [[nodiscard]] size_t peak_bytes() const noexcept; // Includes retained resize geometry.
    [[nodiscard]] size_t live_frames() const noexcept;
    [[nodiscard]] size_t blocked_acquisitions() const noexcept;
    [[nodiscard]] size_t copied_bytes() const noexcept; // Old-damage baseline repair, not new drawing.
    [[nodiscard]] bool has_pending_releases() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gritty::platform::macos
