#include "platform/macos/cocoa_surface.hpp"
#include "platform/macos/mac_input.hpp"
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

@interface GrittyView : NSView <NSTextInputClient> {
    gritty::platform::macos::CocoaLayerSurface* _surface;
}
- (instancetype)initWithSurface:(gritty::platform::macos::CocoaLayerSurface*)surface;
@end

@implementation GrittyView

- (instancetype)initWithSurface:(gritty::platform::macos::CocoaLayerSurface*)surface {
    self = [super initWithFrame:NSZeroRect];
    if (self) {
        _surface = surface;
        [self setWantsLayer:YES];
        self.layer.drawsAsynchronously = NO;
        self.layer.opaque = !surface->is_transparent();
        self.layer.backgroundColor = (surface->is_transparent() ? NSColor.clearColor : NSColor.blackColor).CGColor;
        self.layer.contentsScale = [NSScreen mainScreen].backingScaleFactor;
    }
    return self;
}

- (BOOL)isOpaque { return _surface && !_surface->is_transparent(); }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView { return YES; }

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    CGFloat s = self.window ? self.window.backingScaleFactor : [NSScreen mainScreen].backingScaleFactor;
    self.layer.contentsScale = s;
    NSSize pts = self.bounds.size;
    if (_surface) {
        _surface->on_view_resize(static_cast<int>(pts.width * s), static_cast<int>(pts.height * s), s);
    }
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    CGFloat s = self.window ? self.window.backingScaleFactor : [NSScreen mainScreen].backingScaleFactor;
    if (_surface) {
        _surface->on_view_resize(static_cast<int>(newSize.width * s), static_cast<int>(newSize.height * s), s);
    }
}

- (void)keyDown:(NSEvent *)event {
    if (_surface) {
        auto ev = gritty::platform::macos::MacInput::translate_key_event(event);
        _surface->on_key_event(ev);
    }
}

- (void)keyUp:(NSEvent *)event {
    if (_surface) {
        auto ev = gritty::platform::macos::MacInput::translate_key_event(event);
        ev.action = gritty::input::KeyAction::Release;
        _surface->on_key_event(ev);
    }
}

- (void)flagsChanged:(NSEvent *)event {
    // Modifier only events
    (void)event;
}

- (void)mouseDown:(NSEvent *)event { [self handleMouse:event]; }
- (void)mouseUp:(NSEvent *)event { [self handleMouse:event]; }
- (void)rightMouseDown:(NSEvent *)event { [self handleMouse:event]; }
- (void)rightMouseUp:(NSEvent *)event { [self handleMouse:event]; }
- (void)otherMouseDown:(NSEvent *)event { [self handleMouse:event]; }
- (void)otherMouseUp:(NSEvent *)event { [self handleMouse:event]; }
- (void)mouseMoved:(NSEvent *)event { [self handleMouse:event]; }
- (void)mouseDragged:(NSEvent *)event { [self handleMouse:event]; }
- (void)rightMouseDragged:(NSEvent *)event { [self handleMouse:event]; }
- (void)otherMouseDragged:(NSEvent *)event { [self handleMouse:event]; }
- (void)scrollWheel:(NSEvent *)event { [self handleMouse:event]; }

- (void)handleMouse:(NSEvent *)event {
    if (_surface) {
        auto ev = gritty::platform::macos::MacInput::translate_mouse_event(
            event, 0, 0, self.bounds.size.height);
        _surface->on_mouse_event(ev);
    }
}

// NSTextInputClient stubs
- (void)insertText:(id)string replacementRange:(NSRange)replacementRange { (void)string; (void)replacementRange; }
- (void)doCommandBySelector:(SEL)selector { (void)selector; }
- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange { (void)string; (void)selectedRange; (void)replacementRange; }
- (void)unmarkText {}
- (NSRange)selectedRange { return NSMakeRange(NSNotFound, 0); }
- (NSRange)markedRange { return NSMakeRange(NSNotFound, 0); }
- (BOOL)hasMarkedText { return NO; }
- (nullable NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range actualRange:(nullable NSRangePointer)actualRange { (void)range; (void)actualRange; return nil; }
- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText { return @[]; }
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(nullable NSRangePointer)actualRange { (void)range; (void)actualRange; return NSZeroRect; }
- (NSUInteger)characterIndexForPoint:(NSPoint)point { (void)point; return 0; }

@end

@interface GrittyWindowDelegate : NSObject <NSWindowDelegate> {
    gritty::platform::macos::CocoaLayerSurface* _surface;
}
- (instancetype)initWithSurface:(gritty::platform::macos::CocoaLayerSurface*)surface;
@end

@implementation GrittyWindowDelegate
- (instancetype)initWithSurface:(gritty::platform::macos::CocoaLayerSurface*)surface {
    self = [super init];
    if (self) {
        _surface = surface;
    }
    return self;
}
- (void)windowWillClose:(NSNotification *)notification {
    (void)notification;
    if (_surface) {
        _surface->on_window_close();
    }
}
@end

namespace gritty::platform::macos {

namespace {

void data_provider_release(void* image, const void*, size_t) {
    pixman_image_unref(static_cast<pixman_image_t*>(image));
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
    @autoreleasepool {
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
    int stride = pixel_width_ * 4;
    pixman_image_t* raw = pixman_image_create_bits(
        PIXMAN_a8r8g8b8, pixel_width_, pixel_height_, nullptr, stride);
    surface_.reset(raw);
    buffer_shared_ = false;
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
    if (buffer_shared_) {
        // CALayer may consume the previous frame asynchronously. Keep it
        // immutable and copy its contents so incremental damage stays valid.
        util::PixmanImage next(pixman_image_create_bits(
            PIXMAN_a8r8g8b8, pixel_width_, pixel_height_, nullptr, 0));
        if (!next.valid()) return nullptr;
        pixman_image_composite32(PIXMAN_OP_SRC, surface_.get(), nullptr, next.get(),
            0, 0, 0, 0, 0, 0, pixel_width_, pixel_height_);
        surface_ = std::move(next);
        buffer_shared_ = false;
    }
    return surface_.get();
}

void CocoaLayerSurface::present(const util::PixmanRegion& damage) {
    (void)damage;
    if (!surface_.valid() || !native_view_) return;

    @autoreleasepool {
        GrittyView* view = (GrittyView*)native_view_;
        int w = surface_.width();
        int h = surface_.height();
        int stride = surface_.stride();
        uint32_t* data = surface_.data();

        auto* retained = pixman_image_ref(surface_.get());
        CGDataProviderRef provider = CGDataProviderCreateWithData(
            retained, data, static_cast<size_t>(h) * stride, data_provider_release);
        if (!provider) {
            pixman_image_unref(retained);
            return;
        }

        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        // Zero copy matching PIXMAN_a8r8g8b8 on little-endian ARM64/x86_64
        CGBitmapInfo bitmap_info = kCGBitmapByteOrder32Host | kCGImageAlphaPremultipliedFirst;

        CGImageRef cg_image = CGImageCreate(
            w, h, 8, 32, stride, cs, bitmap_info, provider, nullptr, false, kCGRenderingIntentDefault);

        CGDataProviderRelease(provider);
        CGColorSpaceRelease(cs);

        if (cg_image) {
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            view.layer.contents = (id)cg_image;
            [CATransaction commit];
            buffer_shared_ = true;
            CGImageRelease(cg_image);
        }
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
            NSImage* icon = [[NSImage alloc] initWithContentsOfFile:iconPath];
            if (icon) {
                [NSApp setApplicationIconImage:icon];
                [icon release];
            }
        }

        NSRect frame = NSMakeRect(100, 100, pixel_width_ / scale_, pixel_height_ / scale_);
        NSWindowStyleMask mask = NSWindowStyleMaskTitled |
                                 NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskMiniaturizable |
                                 NSWindowStyleMaskResizable;

        NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
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

        [window makeKeyAndOrderFront:nil];
        [window makeFirstResponder:view];

        native_window_ = (void*)window;
        native_view_ = (void*)view;

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
    @autoreleasepool {
        if (native_window_) {
            NSWindow* win = (NSWindow*)native_window_;
            win.title = [NSString stringWithUTF8String:title.c_str()];
        }
    }
}

void CocoaLayerSurface::on_key_event(const input::KeyEvent& ev) {
    if (key_cb_) key_cb_(ev);
}

void CocoaLayerSurface::on_mouse_event(const input::MouseEvent& ev) {
    if (mouse_cb_) mouse_cb_(ev);
}

void CocoaLayerSurface::on_view_resize(int px_w, int px_h, double scale) {
    resize(px_w, px_h, scale);
    if (resize_cb_) resize_cb_(px_w, px_h, scale);
}

void CocoaLayerSurface::on_window_close() {
    if (close_cb_) close_cb_();
}

} // namespace gritty::platform::macos
