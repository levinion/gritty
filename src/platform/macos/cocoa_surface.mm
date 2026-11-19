#include "platform/macos/cocoa_surface.hpp"
#include "platform/macos/cocoa_view.hpp"
#include "platform/macos/application_icon.hpp"
#include "platform/macos/clipboard.hpp"
#include <utf8proc.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

namespace gritty::platform::macos {

namespace {

NSWindowStyleMask style_mask_for_decorations(WindowDecorations decorations) noexcept {
    switch (decorations) {
        case WindowDecorations::None:
            return NSWindowStyleMaskBorderless;
        case WindowDecorations::Transparent:
        case WindowDecorations::Buttonless:
            return NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                   NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable |
                   NSWindowStyleMaskFullSizeContentView;
        case WindowDecorations::Full:
        default:
            return NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                   NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    }
}

} // namespace

CocoaLayerSurface::CocoaLayerSurface(int width, int height, double scale)
    : pixel_width_(width), pixel_height_(height), scale_(scale) {
    @autoreleasepool {
        if (scale_ <= 0) scale_ = [NSScreen mainScreen].backingScaleFactor;
        if (scale_ <= 0) scale_ = 1.0;
    }
    reallocate_buffer();
}

CocoaLayerSurface::~CocoaLayerSurface() {
    set_frame_ready_callback({});
    cancel_resize_timer();
    cancel_buffer_trim_timer();
    cancel_frame_retry_timer();
    @autoreleasepool {
        if (native_view_) {
            GrittyView* view = (GrittyView*)native_view_;
            [view detachSurface];
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            view.layer.contents = nil;
            [CATransaction commit];
            [CATransaction flush];
        }
        buffers_.detach_displayed();
        if (native_window_) {
            NSWindow* win = (NSWindow*)native_window_;
            [win setDelegate:nil];
            [win close];
            [win release];
            native_window_ = nullptr;
            native_view_ = nullptr;
        }
        if (native_delegate_) {
            [(id)native_delegate_ release];
            native_delegate_ = nullptr;
        }
    }
}

void CocoaLayerSurface::reallocate_buffer() {
    if (pixel_width_ <= 0 || pixel_height_ <= 0) return;
    buffers_.resize(pixel_width_, pixel_height_);
}

void CocoaLayerSurface::resize(int pixel_width, int pixel_height, double scale) {
    if (pixel_width <= 0 || pixel_height <= 0 || scale <= 0) return;
    if (pixel_width == pixel_width_ && pixel_height == pixel_height_ && scale == scale_) return;
    pixel_width_ = pixel_width;
    pixel_height_ = pixel_height;
    scale_ = scale;
    reallocate_buffer();
}

pixman_image_t* CocoaLayerSurface::get_back_buffer() {
    auto* buffer = buffers_.get_back_buffer();
    if (buffer) cancel_frame_retry_timer();
    else if (frame_ready_cb_) request_frame();
    return buffer;
}

void CocoaLayerSurface::present(const util::PixmanRegion& damage) {
    if (!buffers_.valid() || !native_view_) return;

    @autoreleasepool {
        GrittyView* view = (GrittyView*)native_view_;
        IOSurfaceRef surface = buffers_.prepare_present();
        if (!surface) return;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        // CPU Pixman pixels and the WindowServer's contents are the same
        // shared IOSurface: no CGImage/provider or intermediate raster copy.
        view.layer.contents = (id)surface;
        view.layer.contentsScale = scale_;
        [CATransaction commit];
        if (present_flush_) [CATransaction flush];
        buffers_.did_present(&damage); // Only now detach the previous surface.
        ++presented_frames_;
        // The compositor samples this commit on the next vsync tick. Until the
        // frame clock fires, the session only updates state and coalesces.
        frame_in_flight_ = true;
        // This tick only controls pacing; it does NOT release the committed
        // surface. The pool separately checks attachment and IOSurfaceIsInUse
        // before allowing a buffer to be written again.
        schedule_frame_done_tick();
        schedule_buffer_trim();
    }
}

void CocoaLayerSurface::show_window(const std::string& title) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        // Set application icon from resources
        NSString* iconPath = [[NSBundle mainBundle] pathForResource:@"gritty" ofType:@"icns"];
        if (!iconPath || ![[NSFileManager defaultManager] fileExistsAtPath:iconPath]) {
            NSArray* searchPaths = @[
                @"resources/gritty.icns",
                @"../resources/gritty.icns",
                @"resources/gritty.png",
                @"../resources/gritty.png"
            ];
            for (NSString* p in searchPaths) {
                if ([[NSFileManager defaultManager] fileExistsAtPath:p]) {
                    iconPath = p;
                    break;
                }
            }
        }
        if (iconPath) {
            // Keep a 128-point Retina Dock icon, not every representation of
            // the 1024px ICNS (including its expensive decoded image cache).
            CGImageRef pixels = copy_application_icon(iconPath.fileSystemRepresentation);
            if (pixels) {
                NSImage* icon = [[NSImage alloc] initWithCGImage:pixels size:NSMakeSize(128, 128)];
                CGImageRelease(pixels);
                if (icon) {
                    [NSApp setApplicationIconImage:icon];
                    [icon release];
                }
            }
        }

        NSRect frame = NSMakeRect(100, 100, pixel_width_ / scale_, pixel_height_ / scale_);
        const NSWindowStyleMask mask = style_mask_for_decorations(decorations_);
        window_title_ = title;

        GrittyWindow* window = [[GrittyWindow alloc] initWithContentRect:frame
                                                                styleMask:mask
                                                                  backing:NSBackingStoreBuffered
                                                                    defer:NO];
        [window setReleasedWhenClosed:NO];
        window.opaque = !transparent_;
        window.backgroundColor = transparent_ ? NSColor.clearColor : NSColor.blackColor;
        window.title = [NSString stringWithUTF8String:title.c_str()];
        [window setAcceptsMouseMovedEvents:YES];

        GrittyView* view = [[GrittyView alloc] initWithSurface:this];
        [window setContentView:view];
        [view release];

        GrittyWindowDelegate* delegate = [[GrittyWindowDelegate alloc] initWithSurface:this];
        [window setDelegate:delegate];
        native_delegate_ = (void*)delegate;

        native_window_ = (void*)window;
        native_view_ = (void*)view;
        apply_window_decorations();

        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
}

void CocoaLayerSurface::set_transparent(bool transparent) {
    transparent_ = transparent;
    @autoreleasepool {
        // Do not set NSWindow.alphaValue: that would fade the text as well.
        if (native_window_) {
            NSWindow* window = (NSWindow*)native_window_;
            window.opaque = !transparent;
            window.backgroundColor = transparent ? NSColor.clearColor : NSColor.blackColor;
        }
        if (native_view_) {
            NSView* view = (NSView*)native_view_;
            view.layer.opaque = !transparent;
            view.layer.backgroundColor = (transparent ? NSColor.clearColor : NSColor.blackColor).CGColor;
            [view setNeedsDisplay:YES];
        }
    }
}

void CocoaLayerSurface::set_title(const std::string& title) {
    window_title_ = title;
    @autoreleasepool {
        if (native_window_) {
            NSWindow* win = (NSWindow*)native_window_;
            win.title = [NSString stringWithUTF8String:title.c_str()];
        }
    }
}

void CocoaLayerSurface::set_decorations(WindowDecorations decorations) {
    decorations_ = decorations;
    apply_window_decorations();
}

void CocoaLayerSurface::apply_window_decorations() {
    if (!native_window_) return;
    @autoreleasepool {
        GrittyWindow* window = (GrittyWindow*)native_window_;
        [window setStyleMask:style_mask_for_decorations(decorations_)];
        window.hasShadow = YES;

        const bool full_size = decorations_ == WindowDecorations::Transparent ||
                               decorations_ == WindowDecorations::Buttonless;
        window.titlebarAppearsTransparent = full_size;
        window.titleVisibility = full_size ? NSWindowTitleHidden : NSWindowTitleVisible;

        const bool show_buttons = decorations_ != WindowDecorations::Buttonless &&
                                  decorations_ != WindowDecorations::None;
        [window standardWindowButton:NSWindowCloseButton].hidden = !show_buttons;
        [window standardWindowButton:NSWindowMiniaturizeButton].hidden = !show_buttons;
        [window standardWindowButton:NSWindowZoomButton].hidden = !show_buttons;

        // AppKit can reset the title, content view and first responder when the
        // style mask changes; restore all three after the transition.
        window.title = [NSString stringWithUTF8String:window_title_.c_str()];
        if (native_view_) {
            GrittyView* view = (GrittyView*)native_view_;
            [window setContentView:view];
            [window makeFirstResponder:view];
        }
    }
}

void CocoaLayerSurface::on_key_event(const input::KeyEvent& ev) {
    if (key_cb_) key_cb_(ev);
}

void CocoaLayerSurface::commit_text(std::string_view utf8) {
    if (utf8.empty()) return;
    size_t characters = 0;
    std::string_view rest = utf8;
    while (!rest.empty()) {
        utf8proc_int32_t cp = 0;
        const auto n = utf8proc_iterate(reinterpret_cast<const uint8_t*>(rest.data()), rest.size(), &cp);
        if (n <= 0) break;
        ++characters;
        rest.remove_prefix(n);
    }
    if (characters > 1 && input_wants_bracketed_paste()) {
        std::string wrapped = "\x1b[200~";
        wrapped.append(utf8);
        wrapped += "\x1b[201~";
        if (text_commit_cb_) text_commit_cb_(wrapped);
        return;
    }
    if (text_commit_cb_) text_commit_cb_(utf8);
}

void CocoaLayerSurface::paste_from_pasteboard() {
    PasteboardClipboard clipboard;
    std::string text = clipboard.paste_text();
    if (text.empty()) return;
    if (paste_cb_) paste_cb_(text);
}

void CocoaLayerSurface::on_mouse_event(const input::MouseEvent& ev) {
    if (mouse_cb_) mouse_cb_(ev);
}

void CocoaLayerSurface::on_view_resize(int px_w, int px_h, double scale) {
    if (px_w <= 0 || px_h <= 0 || scale <= 0) return;
    pending_width_ = px_w; pending_height_ = px_h; pending_scale_ = scale;
    resize_pending_ = true;
    // Like foot's frame scheduling: keep only the latest configure, not one
    // full-size bitmap and TIOCSWINSZ for every intermediate drag pixel.
    if (!native_view_) flush_pending_resize();
    else schedule_resize(1.0 / 60.0);
}

void CocoaLayerSurface::cancel_resize_timer() {
    if (!native_resize_timer_) return;
    auto timer = static_cast<CFRunLoopTimerRef>(native_resize_timer_);
    native_resize_timer_ = nullptr;
    CFRunLoopTimerInvalidate(timer);
    CFRelease(timer);
}

void CocoaLayerSurface::cancel_buffer_trim_timer() {
    if (!native_buffer_trim_timer_) return;
    auto timer = static_cast<CFRunLoopTimerRef>(native_buffer_trim_timer_);
    CFRunLoopTimerInvalidate(timer);
    CFRelease(timer);
    native_buffer_trim_timer_ = nullptr;
}

void CocoaLayerSurface::schedule_buffer_trim() {
    const auto deadline = CFAbsoluteTimeGetCurrent() + 2.0;
    if (native_buffer_trim_timer_) {
        CFRunLoopTimerSetNextFireDate(static_cast<CFRunLoopTimerRef>(native_buffer_trim_timer_), deadline);
        return;
    }
    CFRunLoopTimerContext context{0, this, nullptr, nullptr, nullptr};
    auto timer = CFRunLoopTimerCreate(nullptr, deadline, 0, 0, 0,
        [](CFRunLoopTimerRef, void* data) {
            @autoreleasepool {
                auto* self = static_cast<CocoaLayerSurface*>(data);
                self->cancel_buffer_trim_timer();
                self->buffers_.release_spare_buffers();
                // A detached surface can still be pinned at the first idle
                // deadline. Keep polling until release, even without new PTY data.
                if (self->buffers_.has_pending_releases()) self->schedule_buffer_trim();
            }
        }, &context);
    if (!timer) return;
    native_buffer_trim_timer_ = timer;
    CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
    CFRunLoopAddTimer(CFRunLoopGetMain(), timer, (CFStringRef)NSEventTrackingRunLoopMode);
}

void CocoaLayerSurface::set_frame_ready_callback(FrameReadyCallback cb) {
    frame_ready_cb_ = std::move(cb);
    if (!frame_ready_cb_) {
        frame_requested_ = false;
        cancel_frame_retry_timer();
        if (native_view_) [(GrittyView*)native_view_ stopFrames];
    }
}

bool CocoaLayerSurface::request_frame() {
    if (!native_view_ || !frame_ready_cb_) return false;
    if (frame_requested_) return true;
    frame_requested_ = true;
    if (occluded_) return true;
    if (![(GrittyView*)native_view_ requestFrame] && !schedule_frame_retry()) {
        frame_requested_ = false;
        return false; // If timer creation fails, let a ready buffer render synchronously.
    }
    return true;
}

void CocoaLayerSurface::on_visibility_changed(bool visible) {
    occluded_ = !visible;
    if (!visible) {
        cancel_frame_retry_timer();
        if (native_view_) [(GrittyView*)native_view_ stopFrames];
    } else if (frame_requested_) {
        frame_requested_ = false;
        if (!request_frame() && frame_ready_cb_) frame_ready_cb_();
    } else if (frame_in_flight_) {
        schedule_frame_done_tick();
    }
}

void CocoaLayerSurface::on_frame_tick() {
    // Permit another commit. Display-link timing is not a buffer-release fence;
    // acquisition still refuses surfaces held by the layer/window server.
    frame_in_flight_ = false;
    if (!frame_requested_) return;
    if (occluded_) { cancel_frame_retry_timer(); return; }
    frame_requested_ = false;
    cancel_frame_retry_timer();
    if (frame_ready_cb_) frame_ready_cb_();
}

void CocoaLayerSurface::cancel_frame_retry_timer() {
    if (!native_frame_retry_timer_) return;
    auto timer = static_cast<CFRunLoopTimerRef>(native_frame_retry_timer_);
    native_frame_retry_timer_ = nullptr;
    CFRunLoopTimerInvalidate(timer);
    CFRelease(timer);
}

void CocoaLayerSurface::schedule_frame_done_tick() {
    if (occluded_) return;
    if (native_view_ && [(GrittyView*)native_view_ requestFrame]) return;
    schedule_frame_retry();
}

bool CocoaLayerSurface::schedule_frame_retry() {
    if (native_frame_retry_timer_) return true;
    CFRunLoopTimerContext context{0, this, nullptr, nullptr, nullptr};
    auto timer = CFRunLoopTimerCreate(nullptr, CFAbsoluteTimeGetCurrent() + 1.0 / 60.0, 0, 0, 0,
        [](CFRunLoopTimerRef, void* data) {
            @autoreleasepool {
                auto* self = static_cast<CocoaLayerSurface*>(data);
                self->on_frame_tick();
            }
        }, &context);
    if (!timer) return false;
    native_frame_retry_timer_ = timer;
    CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
    CFRunLoopAddTimer(CFRunLoopGetMain(), timer, (CFStringRef)NSEventTrackingRunLoopMode);
    return true;
}

void CocoaLayerSurface::schedule_resize(double delay) {
    if (native_resize_timer_) return;
    CFRunLoopTimerContext context{0, this, nullptr, nullptr, nullptr};
    auto timer = CFRunLoopTimerCreate(nullptr, CFAbsoluteTimeGetCurrent() + delay, 0, 0, 0,
        [](CFRunLoopTimerRef, void* data) {
            auto* self = static_cast<CocoaLayerSurface*>(data);
            self->flush_pending_resize();
        }, &context);
    if (!timer) { flush_pending_resize(); return; }
    native_resize_timer_ = timer;
    CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
    CFRunLoopAddTimer(CFRunLoopGetMain(), timer, (CFStringRef)NSEventTrackingRunLoopMode);
}

void CocoaLayerSurface::flush_pending_resize() {
    cancel_resize_timer();
    if (!resize_pending_) return;
    resize_pending_ = false;
    if (pending_width_ == pixel_width_ && pending_height_ == pixel_height_ && pending_scale_ == scale_) return;
    resize(pending_width_, pending_height_, pending_scale_);
    if (resize_cb_) resize_cb_(pixel_width_, pixel_height_, scale_);
}

void CocoaLayerSurface::on_live_resize(bool active) {
    if (!active) {
        flush_pending_resize(); // Always deliver the exact final geometry.
    }
}

void CocoaLayerSurface::on_window_focus(bool focused) {
    if (focus_cb_) focus_cb_(focused);
}

void CocoaLayerSurface::open_url(std::string_view url) {
    @autoreleasepool {
        if (url.empty()) return;
        NSString* text = [[NSString alloc] initWithBytes:url.data() length:url.size()
                                                encoding:NSUTF8StringEncoding];
        if (!text) return;
        NSURL* parsed = [NSURL URLWithString:text];
        if (parsed) [[NSWorkspace sharedWorkspace] openURL:parsed];
    }
}

void CocoaLayerSurface::on_window_close() {
    if (close_cb_) close_cb_();
}

} // namespace gritty::platform::macos
