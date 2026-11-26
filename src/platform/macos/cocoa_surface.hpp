#pragma once

#include <functional>
#include "platform/pal_interfaces.hpp"
#include "platform/macos/iosurface_pool.hpp"
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
    pixman_image_t* get_back_buffer_for_frame(bool full_redraw) override;
    [[nodiscard]] bool is_ready() const noexcept override { return native_view_ != nullptr; }
    void present(const util::PixmanRegion& damage) override;
    bool request_frame() override;
    [[nodiscard]] bool supports_frame_pacing() const noexcept override { return true; }
    [[nodiscard]] bool frame_in_flight() const noexcept override { return frame_in_flight_; }
    // Commit CATransaction and let the run loop flush; explicit flush adds a sync round trip.
    void set_present_flush(bool enabled) noexcept { present_flush_ = enabled; }
    [[nodiscard]] bool present_flush() const noexcept { return present_flush_; }

    [[nodiscard]] int pixel_width() const noexcept override { return pixel_width_; }
    [[nodiscard]] int pixel_height() const noexcept override { return pixel_height_; }
    [[nodiscard]] double scale() const noexcept override { return scale_; }

    void show_window(const std::string& title = "Gritty");
    void focus_window();
    void set_title(const std::string& title) override;
    void set_transparent(bool transparent) override;
    void set_decorations(WindowDecorations decorations) override;
    [[nodiscard]] WindowDecorations decorations() const noexcept override { return decorations_; }
    void set_cursor_shape(MouseCursorShape shape) override;
    [[nodiscard]] MouseCursorShape cursor_shape() const noexcept { return cursor_shape_; }
    void send_notification(std::string_view title, std::string_view body) override;
    [[nodiscard]] bool mouse_reporting() const noexcept {
        return mouse_reporting_cb_ && mouse_reporting_cb_();
    }
    [[nodiscard]] bool is_transparent() const noexcept { return transparent_; }
    [[nodiscard]] size_t buffer_allocation_count() const noexcept { return buffers_.allocation_count(); }
    [[nodiscard]] size_t buffer_live_bytes() const noexcept { return buffers_.live_bytes(); }
    [[nodiscard]] size_t buffer_peak_bytes() const noexcept { return buffers_.peak_bytes(); }
    [[nodiscard]] size_t buffer_live_frames() const noexcept { return buffers_.live_frames(); }
    [[nodiscard]] size_t buffer_blocked_acquisitions() const noexcept { return buffers_.blocked_acquisitions(); }
    [[nodiscard]] size_t buffer_copied_bytes() const noexcept { return buffers_.copied_bytes(); }
    [[nodiscard]] size_t presented_frame_count() const noexcept { return presented_frames_; }

    using KeyCallback = std::function<void(const input::KeyEvent&)>;
    using MouseCallback = std::function<void(const input::MouseEvent&)>;
    using ResizeCallback = std::function<void(int px_w, int px_h, double scale)>;

    void set_key_callback(KeyCallback cb) override { key_cb_ = std::move(cb); }
    void set_text_commit_callback(TextCommitCallback cb) override { text_commit_cb_ = std::move(cb); }
    void set_preedit_callback(PreeditCallback cb) override { preedit_cb_ = std::move(cb); }
    void set_paste_callback(PasteCallback cb) override { paste_cb_ = std::move(cb); }
    void set_mouse_callback(MouseCallback cb) override { mouse_cb_ = std::move(cb); }
    void set_resize_callback(ResizeCallback cb) override { resize_cb_ = std::move(cb); }
    void set_close_callback(CloseCallback cb) override { close_cb_ = std::move(cb); }
    void set_frame_ready_callback(FrameReadyCallback cb) override;
    void set_focus_callback(FocusCallback cb) override { focus_cb_ = std::move(cb); }
    void set_quit_request_callback(std::function<void()> cb) { quit_request_cb_ = std::move(cb); }
    void set_new_window_callback(std::function<void()> cb) { new_window_cb_ = std::move(cb); }
    void set_activity_callback(std::function<void()> cb) { activity_cb_ = std::move(cb); }
    void set_cursor_rect_provider(CursorRectProvider cb) override { cursor_rect_cb_ = std::move(cb); }
    void set_bracketed_paste_provider(BracketedPasteProvider cb) override { bracketed_paste_cb_ = std::move(cb); }
    void set_mouse_reporting_provider(MouseReportingProvider cb) override {
        mouse_reporting_cb_ = std::move(cb);
    }

    // Called by GrittyView's NSTextInputClient implementation.
    [[nodiscard]] bool input_wants_bracketed_paste() const noexcept {
        return bracketed_paste_cb_ && bracketed_paste_cb_();
    }
    [[nodiscard]] CursorRect cursor_rect() const noexcept {
        return cursor_rect_cb_ ? cursor_rect_cb_() : CursorRect{};
    }
    void commit_text(std::string_view utf8);
    void on_preedit(std::string_view utf8, size_t sel_start, size_t sel_end);
    // Invoked by GrittyView for Cmd+V. Reads the system pasteboard and hands
    // the raw UTF-8 to the session's paste callback.
    void paste_from_pasteboard();
    void open_url(std::string_view url) override;

    // Internal bridge for Obj-C view & window
    void on_key_event(const input::KeyEvent& ev);
    void on_mouse_event(const input::MouseEvent& ev);
    void on_view_resize(int px_w, int px_h, double scale);
    void on_live_resize(bool active);
    void flush_pending_resize();
    void on_window_close();
    void on_frame_tick();
    void on_visibility_changed(bool visible);
    void on_window_focus(bool focused);
    void on_quit_requested();
    void on_new_window_requested();
    void on_user_activity();

private:
    void reallocate_buffer();
    void apply_window_decorations();
    void schedule_resize(double delay);
    void cancel_resize_timer();
    void schedule_buffer_trim();
    void cancel_buffer_trim_timer();
    bool schedule_frame_retry();
    void schedule_frame_done_tick();
    void cancel_frame_retry_timer();

    int pixel_width_{640};
    int pixel_height_{480};
    double scale_{2.0};

    IOSurfaceFramePool buffers_;
    size_t presented_frames_{0};
    bool transparent_{false};
    WindowDecorations decorations_{WindowDecorations::Full};
    MouseCursorShape cursor_shape_{MouseCursorShape::Default};
    std::string window_title_{"Gritty"};
    void* native_window_{nullptr};
    void* native_view_{nullptr};
    void* native_delegate_{nullptr};
    void* native_resize_timer_{nullptr};
    void* native_buffer_trim_timer_{nullptr};
    void* native_frame_retry_timer_{nullptr};
    bool resize_pending_{false};
    bool frame_requested_{false};
    bool frame_in_flight_{false};
    bool present_flush_{false}; // CoreAnimation commits at runloop end; no vsync round trip.
    bool occluded_{false};
    int pending_width_{0}, pending_height_{0};
    double pending_scale_{1.0};

    KeyCallback key_cb_;
    TextCommitCallback text_commit_cb_;
    PreeditCallback preedit_cb_;
    PasteCallback paste_cb_;
    MouseCallback mouse_cb_;
    ResizeCallback resize_cb_;
    CloseCallback close_cb_;
    FrameReadyCallback frame_ready_cb_;
    FocusCallback focus_cb_;
    std::function<void()> quit_request_cb_;
    std::function<void()> new_window_cb_;
    std::function<void()> activity_cb_;
    CursorRectProvider cursor_rect_cb_;
    BracketedPasteProvider bracketed_paste_cb_;
    MouseReportingProvider mouse_reporting_cb_;
};

struct AppDelegateCallbacks {
    std::function<void()> on_reopen_no_windows;
    std::function<void()> on_quit_requested;
};

void initialize_cocoa_application(AppDelegateCallbacks callbacks);
void shutdown_cocoa_application();

} // namespace gritty::platform::macos
