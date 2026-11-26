#include "platform/macos/cocoa_view.hpp"
#include "platform/macos/cocoa_surface.hpp"
#include "platform/macos/mac_input.hpp"
#include <algorithm>
#include <string_view>

@implementation GrittyWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

@implementation GrittyView

- (instancetype)initWithSurface:(gritty::platform::macos::CocoaLayerSurface*)surface {
    self = [super initWithFrame:NSZeroRect];
    if (self) {
        _surface = surface;
        _markedText = nil;
        _markedRange = NSMakeRange(NSNotFound, 0);
        _selectedRange = NSMakeRange(0, 0);
        _imeHandled = NO;
        _scrollAccumY = 0.0;
        // Host our own CPU-image layer. A layer-backed NSView lets AppKit
        // allocate additional resize backing bitmaps we never draw into.
        [self setLayer:[CALayer layer]];
        [self setWantsLayer:YES];
        self.layer.drawsAsynchronously = NO;
        self.layer.magnificationFilter = kCAFilterNearest;
        self.layer.minificationFilter = kCAFilterNearest;
        self.layer.edgeAntialiasingMask = 0;
        self.layer.opaque = !surface->is_transparent();
        self.layer.backgroundColor = (surface->is_transparent() ? NSColor.clearColor : NSColor.blackColor).CGColor;
        self.layer.contentsScale = [NSScreen mainScreen].backingScaleFactor;
        [self rebuildDisplayLink];
    }
    return self;
}

- (void)dealloc {
    [_displayLink invalidate];
    [_displayLink release];
    _displayLink = nil;
    [_markedText release];
    [super dealloc];
}

- (void)rebuildDisplayLink {
    if (!_surface) return;
    if (@available(macOS 14.0, *)) {
        const BOOL was_paused = _displayLink ? _displayLink.paused : YES;
        if (_displayLink) {
            [_displayLink invalidate];
            [_displayLink release];
            _displayLink = nil;
        }
        _displayLink = [[self displayLinkWithTarget:self selector:@selector(frameTick:)] retain];
        _displayLink.paused = was_paused;
        [_displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    }
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window) [self rebuildDisplayLink];
}

- (void)detachSurface {
    [_displayLink invalidate];
    [_displayLink release];
    _displayLink = nil;
    _surface = nullptr;
}
- (BOOL)requestFrame {
    if (!_displayLink) return NO;
    _displayLink.paused = NO;
    return YES;
}
- (void)stopFrames { _displayLink.paused = YES; }
- (void)frameTick:(CADisplayLink*)link {
    @autoreleasepool {
        link.paused = YES; // One-shot: no work means no continuously running frame clock.
        if (_surface) _surface->on_frame_tick();
    }
}
- (void)viewWillStartLiveResize {
    [super viewWillStartLiveResize];
    if (_surface) _surface->on_live_resize(true);
}
- (void)viewDidEndLiveResize {
    [super viewDidEndLiveResize];
    if (_surface) _surface->on_live_resize(false);
}

- (BOOL)isOpaque { return _surface && !_surface->is_transparent(); }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView { return YES; }

- (void)resetCursorRects {
    [super resetCursorRects];
    if (!_surface || _surface->cursor_shape() == gritty::platform::MouseCursorShape::Hidden) return;
    NSCursor* cursor = cursor_for_shape(_surface->cursor_shape());
    if (cursor) [self addCursorRect:self.bounds cursor:cursor];
}

- (void)cursorUpdate:(NSEvent*)event {
    (void)event;
    if (!_surface) return;
    if (_surface->cursor_shape() == gritty::platform::MouseCursorShape::Hidden) {
        [NSCursor setHiddenUntilMouseMoves:YES];
        return;
    }
    NSCursor* cursor = cursor_for_shape(_surface->cursor_shape());
    if (cursor) [cursor set];
}

- (void)viewDidChangeBackingProperties {
    @autoreleasepool {
        [super viewDidChangeBackingProperties];
        [self rebuildDisplayLink];
        CGFloat s = self.window ? self.window.backingScaleFactor : [NSScreen mainScreen].backingScaleFactor;
        self.layer.contentsScale = s;
        NSSize pts = self.bounds.size;
        if (_surface) _surface->on_view_resize(static_cast<int>(pts.width * s), static_cast<int>(pts.height * s), s);
    }
}

- (void)setFrameSize:(NSSize)newSize {
    @autoreleasepool {
        [super setFrameSize:newSize];
        CGFloat s = self.window ? self.window.backingScaleFactor : [NSScreen mainScreen].backingScaleFactor;
        if (_surface) _surface->on_view_resize(static_cast<int>(newSize.width * s), static_cast<int>(newSize.height * s), s);
    }
}

namespace {
NSCursor* cursor_for_shape(gritty::platform::MouseCursorShape shape) {
    using gritty::platform::MouseCursorShape;
    switch (shape) {
        case MouseCursorShape::Text: return [NSCursor IBeamCursor];
        case MouseCursorShape::Pointer: return [NSCursor pointingHandCursor];
        case MouseCursorShape::EwResize: return [NSCursor resizeLeftRightCursor];
        case MouseCursorShape::NsResize: return [NSCursor resizeUpDownCursor];
        // AppKit exposes no public diagonal resize cursor.
        case MouseCursorShape::NeswResize:
        case MouseCursorShape::NwseResize: return [NSCursor crosshairCursor];
        case MouseCursorShape::Crosshair: return [NSCursor crosshairCursor];
        case MouseCursorShape::Move: return [NSCursor openHandCursor];
        case MouseCursorShape::NotAllowed: return [NSCursor operationNotAllowedCursor];
        case MouseCursorShape::Hidden: return nil;
        case MouseCursorShape::Default: return [NSCursor arrowCursor];
    }
    return [NSCursor arrowCursor];
}

bool command_key_is(NSEvent* event, NSString* expected) {
    NSString* chars = [event charactersIgnoringModifiers];
    return chars.length > 0 && [[chars lowercaseString] isEqualToString:expected];
}

bool direct_key_event(NSEvent* event, BOOL has_marked_text) {
    if (has_marked_text) return NO;
    const auto flags = [event modifierFlags];
    if (flags & (NSEventModifierFlagControl | NSEventModifierFlagCommand | NSEventModifierFlagOption)) return YES;
    switch ([event keyCode]) {
        case 53: case 36: case 48: case 51: case 117: // Escape Return Tab Backspace Delete
        case 126: case 125: case 123: case 124:       // arrows
        case 115: case 119: case 116: case 121: case 114: // Home End PageUp PageDown Insert
        case 122: case 120: case 99: case 118: case 96: case 97: case 98:
        case 100: case 101: case 109: case 103: case 111: // F1-F12
            return YES;
        default:
            return NO;
    }
}
} // namespace

- (void)keyDown:(NSEvent *)event {
    if (!_surface) return;
    const auto flags = [event modifierFlags];
    if (flags & NSEventModifierFlagCommand) {
        // Never let AppKit's key bindings or NSTextInputClient see a Command
        // chord: Cmd+V used to become a plain 'v' via insertText:.
        // CapsLock/NumericPad and other incidental flags are irrelevant here.
        const bool plain = (flags & (NSEventModifierFlagControl | NSEventModifierFlagOption)) == 0;
        NSWindow* window = self.window;
        if (plain && command_key_is(event, @"v")) {
            _surface->paste_from_pasteboard();
        } else if (plain && command_key_is(event, @"h")) {
            [NSApp hide:nil];
        } else if (plain && window && command_key_is(event, @"m")) {
            [window performMiniaturize:nil];
        } else if (plain && window &&
                   (command_key_is(event, @"w") || command_key_is(event, @"q"))) {
            // Same path as the close button: windowWillClose ->
            // CocoaLayerSurface::on_window_close -> close callback stops the
            // KqueueRunLoop, so C++ destructors and the PTY are cleaned up.
            [window performClose:nil];
        }
        return; // Every other Command chord is swallowed, never sent to PTY.
    }
    if (!direct_key_event(event, [self hasMarkedText])) {
        _imeHandled = NO;
        [self interpretKeyEvents:@[event]];
        if (_imeHandled || [self hasMarkedText]) return;
    }
    auto ev = gritty::platform::macos::MacInput::translate_key_event(event);
    _surface->on_key_event(ev);
}

- (void)keyUp:(NSEvent *)event {
    // The matching keyDown was consumed; do not leak a release event either.
    if ([event modifierFlags] & NSEventModifierFlagCommand) return;
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

- (void)mouseDown:(NSEvent *)event {
    if (_surface && self.window) {
        const auto decorations = _surface->decorations();
        const bool borderless_titlebar = decorations == gritty::platform::WindowDecorations::Transparent ||
                                         decorations == gritty::platform::WindowDecorations::Buttonless;
        const auto flags = [event modifierFlags] &
            (NSEventModifierFlagShift | NSEventModifierFlagControl |
             NSEventModifierFlagOption | NSEventModifierFlagCommand);
        if (borderless_titlebar && flags == 0 && !_surface->mouse_reporting()) {
            const NSPoint local = [self convertPoint:[event locationInWindow] fromView:nil];
            if (local.y >= self.bounds.size.height - 28.0) {
                // AppKit logs "Window move completed without beginning" when a
                // drag starts after the physical button was released (rapid
                // click or delayed runloop delivery). Verify live button state
                // before handing the press to the native window mover.
                const BOOL left_held = ([NSEvent pressedMouseButtons] & (1 << 0)) != 0;
                if (event.type == NSEventTypeLeftMouseDown && event.buttonNumber == 0 && left_held)
                    [self.window performWindowDragWithEvent:event];
                return; // The custom titlebar belongs to the window, not the PTY.
            }
        }
    }
    [self handleMouse:event];
}
- (void)mouseUp:(NSEvent *)event { [self handleMouse:event]; }
- (void)rightMouseDown:(NSEvent *)event { [self handleMouse:event]; }
- (void)rightMouseUp:(NSEvent *)event { [self handleMouse:event]; }
- (void)otherMouseDown:(NSEvent *)event { [self handleMouse:event]; }
- (void)otherMouseUp:(NSEvent *)event { [self handleMouse:event]; }
- (void)mouseMoved:(NSEvent *)event { [self handleMouse:event]; }
- (void)mouseDragged:(NSEvent *)event { [self handleMouse:event]; }
- (void)rightMouseDragged:(NSEvent *)event { [self handleMouse:event]; }
- (void)otherMouseDragged:(NSEvent *)event { [self handleMouse:event]; }
- (void)scrollWheel:(NSEvent *)event {
    if (!_surface) return;
    if ([event hasPreciseScrollingDeltas]) {
        if (event.phase == NSEventPhaseBegan) _scrollAccumY = 0.0;
        _scrollAccumY += [event scrollingDeltaY];
        constexpr CGFloat kStep = 15.0;
        while (_scrollAccumY >= kStep || _scrollAccumY <= -kStep) {
            auto ev = gritty::platform::macos::MacInput::translate_mouse_event(
                event, 0, 0, self.bounds.size.height);
            ev.button = _scrollAccumY > 0 ? gritty::input::MouseButton::WheelUp
                                          : gritty::input::MouseButton::WheelDown;
            ev.action = gritty::input::MouseAction::Press;
            _surface->on_mouse_event(ev);
            _scrollAccumY += (_scrollAccumY > 0 ? -kStep : kStep);
        }
        return;
    }
    if ([event scrollingDeltaY] == 0.0) return;
    [self handleMouse:event];
}

- (void)handleMouse:(NSEvent *)event {
    if (_surface) {
        auto ev = gritty::platform::macos::MacInput::translate_mouse_event(
            event, 0, 0, self.bounds.size.height);
        _surface->on_mouse_event(ev);
    }
}

- (void)clearMarkedText {
    [_markedText release];
    _markedText = nil;
    _markedRange = NSMakeRange(NSNotFound, 0);
    _selectedRange = NSMakeRange(0, 0);
}

// NSTextInputClient
- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    (void)replacementRange;
    NSString* text = [string isKindOfClass:[NSAttributedString class]]
        ? [(NSAttributedString*)string string] : (NSString*)string;
    if (!text) return;
    [self clearMarkedText];
    _imeHandled = YES;
    const auto utf8 = std::string_view([text UTF8String],
        [text lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
    if (_surface) _surface->commit_text(utf8);
}

- (void)doCommandBySelector:(SEL)selector {
    using gritty::input::KeyCode;
    const auto name = NSStringFromSelector(selector);
    KeyCode key = KeyCode::Unknown;
    if ([name isEqualToString:@"deleteBackward:"]) key = KeyCode::Backspace;
    else if ([name isEqualToString:@"deleteForward:"]) key = KeyCode::Delete;
    else if ([name isEqualToString:@"moveLeft:"]) key = KeyCode::Left;
    else if ([name isEqualToString:@"moveRight:"]) key = KeyCode::Right;
    else if ([name isEqualToString:@"moveUp:"]) key = KeyCode::Up;
    else if ([name isEqualToString:@"moveDown:"]) key = KeyCode::Down;
    else if ([name isEqualToString:@"insertNewline:"]) key = KeyCode::Return;
    else if ([name isEqualToString:@"insertTab:"]) key = KeyCode::Tab;
    else if ([name isEqualToString:@"cancelOperation:"]) key = KeyCode::Escape;
    else if ([name isEqualToString:@"moveToBeginningOfLine:"]) key = KeyCode::Home;
    else if ([name isEqualToString:@"moveToEndOfLine:"]) key = KeyCode::End;
    if (key == KeyCode::Unknown) return;
    _imeHandled = YES;
    [self clearMarkedText];
    [self sendKeyCode:key];
}

- (void)sendKeyCode:(gritty::input::KeyCode)key {
    if (!_surface) return;
    gritty::input::KeyEvent ev;
    ev.key = key;
    switch (key) {
        case gritty::input::KeyCode::Escape: ev.codepoint = 27; break;
        case gritty::input::KeyCode::Return: ev.codepoint = 13; break;
        case gritty::input::KeyCode::Tab: ev.codepoint = 9; break;
        case gritty::input::KeyCode::Backspace: ev.codepoint = 127; break;
        case gritty::input::KeyCode::Delete: ev.codepoint = 127; break;
        default: break;
    }
    _surface->on_key_event(ev);
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
    (void)replacementRange;
    if (!string) { [self clearMarkedText]; return; }
    NSAttributedString* attributed = [string isKindOfClass:[NSAttributedString class]]
        ? (NSAttributedString*)string
        : [[[NSAttributedString alloc] initWithString:(NSString*)string] autorelease];
    if (!_markedText) _markedText = [[NSMutableAttributedString alloc] init];
    [_markedText setAttributedString:attributed];
    const auto length = [_markedText length];
    _markedRange = NSMakeRange(0, length);
    _selectedRange = NSMakeRange(std::min(selectedRange.location, length),
        std::min(selectedRange.length, length - std::min(selectedRange.location, length)));
    _imeHandled = YES;
}

- (void)unmarkText {
    if ([self hasMarkedText] && _surface) {
        NSString* text = [_markedText string];
        if (text && text.length > 0) {
            const auto utf8 = std::string_view([text UTF8String],
                [text lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
            _surface->commit_text(utf8);
        }
    }
    [self clearMarkedText];
}
- (NSRange)selectedRange { return _selectedRange; }
- (NSRange)markedRange { return _markedRange; }
- (BOOL)hasMarkedText { return _markedText != nil && [_markedText length] > 0; }

- (nullable NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range actualRange:(nullable NSRangePointer)actualRange {
    if (![self hasMarkedText]) return nil;
    const auto length = [_markedText length];
    const auto location = std::min(range.location, length);
    const auto safe = NSMakeRange(location, std::min(range.length, length - location));
    if (actualRange) *actualRange = safe;
    return [_markedText attributedSubstringFromRange:safe];
}

- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText {
    return @[NSFontAttributeName, NSForegroundColorAttributeName,
        NSBackgroundColorAttributeName, NSUnderlineStyleAttributeName];
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(nullable NSRangePointer)actualRange {
    if (actualRange) *actualRange = range;
    if (!_surface || !self.window) return NSZeroRect;
    const auto cursor = _surface->cursor_rect();
    const CGFloat view_height = self.bounds.size.height;
    NSRect local = NSMakeRect(cursor.x, view_height - cursor.y - cursor.height,
        std::max(1.0, cursor.width), std::max(1.0, cursor.height));
    NSRect in_window = [self convertRect:local toView:nil];
    return [self.window convertRectToScreen:in_window];
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point { (void)point; return 0; }

@end

@implementation GrittyWindowDelegate
- (instancetype)initWithSurface:(gritty::platform::macos::CocoaLayerSurface*)surface {
    self = [super init];
    if (self) {
        _surface = surface;
    }
    return self;
}
- (void)windowDidChangeOcclusionState:(NSNotification*)notification {
    NSWindow* window = notification.object;
    if (_surface) _surface->on_visibility_changed((window.occlusionState & NSWindowOcclusionStateVisible) != 0);
}
- (void)windowDidBecomeKey:(NSNotification *)notification {
    (void)notification;
    if (_surface) _surface->on_window_focus(true);
}
- (void)windowDidResignKey:(NSNotification *)notification {
    (void)notification;
    if (_surface) _surface->on_window_focus(false);
}
- (void)windowWillClose:(NSNotification *)notification {
    (void)notification;
    if (_surface) {
        _surface->on_window_close();
    }
}
@end
