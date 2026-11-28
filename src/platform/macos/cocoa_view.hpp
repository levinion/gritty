#pragma once
#ifdef __OBJC__
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include "input/input_events.hpp"

namespace gritty::platform::macos { class CocoaLayerSurface; }

@interface GrittyWindow : NSWindow
@end

@interface GrittyView : NSView <NSTextInputClient> {
    gritty::platform::macos::CocoaLayerSurface* _surface;
    CADisplayLink* _displayLink;
    NSMutableAttributedString* _markedText;
    NSRange _markedRange;
    NSRange _selectedRange;
    BOOL _imeHandled;
    CGFloat _scrollAccumY;
    CGFloat _magnifyAccum;
}
- (instancetype)initWithSurface:(gritty::platform::macos::CocoaLayerSurface*)surface;
- (void)detachSurface;
- (void)rebuildDisplayLink;
- (BOOL)requestFrame;
- (void)stopFrames;
- (void)sendKeyCode:(gritty::input::KeyCode)key;
- (void)paste:(id)sender;
- (void)zoomIn:(id)sender;
- (void)zoomOut:(id)sender;
- (void)resetZoom:(id)sender;
- (void)resetCursorRects;
- (void)cursorUpdate:(NSEvent*)event;
@end

@interface GrittyWindowDelegate : NSObject <NSWindowDelegate> {
    gritty::platform::macos::CocoaLayerSurface* _surface;
}
- (instancetype)initWithSurface:(gritty::platform::macos::CocoaLayerSurface*)surface;
@end
#endif
