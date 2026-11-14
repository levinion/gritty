#pragma once

#include <memory>
#include <functional>
#include "platform/pal_interfaces.hpp"
#include "input/input_events.hpp"
#include "util/raii.hpp"

#ifdef __OBJC__
@class NSWindow;
@class NSView;
#else
class NSWindow;
class NSView;
#endif

namespace gritty::platform::macos {

class CocoaLayerSurface final : public IWindowSurface {
public:
    CocoaLayerSurface(int width, int height, double scale = 0.0);
    ~CocoaLayerSurface() override;

    CocoaLayerSurface(const CocoaLayerSurface&) = delete;
    CocoaLayerSurface& operator=(const CocoaLayerSurface&) = delete;

    void resize(int pixel_width, int pixel_height, double scale) override;
    pixman_image_t* get_back_buffer() override;
    void present(const util::PixmanRegion& damage) override;

    [[nodiscard]] int pixel_width() const noexcept override { return pixel_width_; }
    [[nodiscard]] int pixel_height() const noexcept override { return pixel_height_; }
    [[nodiscard]] double scale() const noexcept override { return scale_; }

    void show_window(const std::string& title = "Gritty");
    void set_title(const std::string& title) override;
    void set_transparent(bool transparent) override;
    [[nodiscard]] bool is_transparent() const noexcept { return transparent_; }

    using KeyCallback = std::function<void(const input::KeyEvent&)>;
    using MouseCallback = std::function<void(const input::MouseEvent&)>;
    using ResizeCallback = std::function<void(int px_w, int px_h, double scale)>;

    void set_key_callback(KeyCallback cb) override { key_cb_ = std::move(cb); }
    void set_mouse_callback(MouseCallback cb) override { mouse_cb_ = std::move(cb); }
    void set_resize_callback(ResizeCallback cb) override { resize_cb_ = std::move(cb); }
    void set_close_callback(CloseCallback cb) override { close_cb_ = std::move(cb); }

    // Internal bridge for Obj-C view & window
    void on_key_event(const input::KeyEvent& ev);
    void on_mouse_event(const input::MouseEvent& ev);
    void on_view_resize(int px_w, int px_h, double scale);
    void on_window_close();

private:
    void reallocate_buffer();

    int pixel_width_{640};
    int pixel_height_{480};
    double scale_{2.0};

    util::PixmanImage surface_;
    bool buffer_shared_{false};
    bool transparent_{false};
    void* native_window_{nullptr};
    void* native_view_{nullptr};
    void* native_delegate_{nullptr};

    KeyCallback key_cb_;
    MouseCallback mouse_cb_;
    ResizeCallback resize_cb_;
    CloseCallback close_cb_;
};

} // namespace gritty::platform::macos
