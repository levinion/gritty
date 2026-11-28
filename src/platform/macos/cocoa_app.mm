#include "platform/macos/cocoa_app.hpp"
#include "platform/macos/cocoa_surface.hpp"
#include "platform/macos/application_icon.hpp"
#include "util/version.hpp"
#import <Cocoa/Cocoa.h>
#include <utility>

namespace gritty::platform::macos {
namespace {

AppDelegateCallbacks g_app_callbacks;
bool g_app_initialized = false;

} // namespace
} // namespace gritty::platform::macos

@interface GrittyAppDelegate : NSObject <NSApplicationDelegate, NSMenuItemValidation>
- (void)showAbout:(id)sender;
- (void)newWindow:(id)sender;
- (void)closeCurrentWindow:(id)sender;
- (void)requestQuit:(id)sender;
@end

namespace {

NSAttributedString* build_about_credits() {
    NSMutableParagraphStyle* style = [[[NSMutableParagraphStyle alloc] init] autorelease];
    style.alignment = NSTextAlignmentCenter;
    style.lineSpacing = 3.0;
    NSDictionary* attrs = @{
        NSFontAttributeName: [NSFont systemFontOfSize:12.0],
        NSForegroundColorAttributeName: [NSColor labelColor],
        NSParagraphStyleAttributeName: style,
    };
    NSString* text = @"CPU-rendered terminal emulator for tmux.\n"
                     @"Gritty is written by levinion and contributors.\n"
                     @"GNU General Public License v3.0.\n"
                     @"github.com/levinion/gritty";
    NSMutableAttributedString* credits =
        [[[NSMutableAttributedString alloc] initWithString:text attributes:attrs] autorelease];
    const NSRange link_range = [text rangeOfString:@"github.com/levinion/gritty"];
    if (link_range.location != NSNotFound) {
        [credits addAttribute:NSLinkAttributeName
                        value:@"https://github.com/levinion/gritty"
                        range:link_range];
    }
    return credits;
}

} // namespace

@implementation GrittyAppDelegate
- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender hasVisibleWindows:(BOOL)flag {
    (void)sender;
    if (!flag && gritty::platform::macos::g_app_callbacks.on_reopen_no_windows) {
        gritty::platform::macos::g_app_callbacks.on_reopen_no_windows();
    }
    return YES;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    if (gritty::platform::macos::g_app_callbacks.on_quit_requested) {
        gritty::platform::macos::g_app_callbacks.on_quit_requested();
        return NSTerminateCancel;
    }
    return NSTerminateNow;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return NO;
}

- (void)showAbout:(id)sender {
    (void)sender;
    NSString* version = [[[NSString alloc] initWithBytes:gritty::util::kVersion.data()
                                                  length:gritty::util::kVersion.size()
                                                encoding:NSUTF8StringEncoding] autorelease];
    NSString* ver = version ? version : @"";
    NSDictionary* opts = @{
        NSAboutPanelOptionApplicationName: @"Gritty",
        NSAboutPanelOptionApplicationVersion: ver,
        NSAboutPanelOptionVersion: ver,
        NSAboutPanelOptionCredits: build_about_credits(),
    };
    [NSApp orderFrontStandardAboutPanelWithOptions:opts];
}

- (void)newWindow:(id)sender {
    (void)sender;
    if (gritty::platform::macos::g_app_callbacks.on_new_window) {
        gritty::platform::macos::g_app_callbacks.on_new_window();
    }
}

- (void)closeCurrentWindow:(id)sender {
    (void)sender;
    NSWindow* win = [NSApp keyWindow] ? [NSApp keyWindow] : [NSApp mainWindow];
    if (!win) return;
    if (win.styleMask & NSWindowStyleMaskClosable) {
        [win performClose:nil];
    } else {
        [win close];
    }
}

- (void)requestQuit:(id)sender {
    (void)sender;
    if (gritty::platform::macos::g_app_callbacks.on_quit_requested) {
        gritty::platform::macos::g_app_callbacks.on_quit_requested();
    } else {
        [NSApp terminate:nil];
    }
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if ([item action] == @selector(closeCurrentWindow:)) {
        return [NSApp keyWindow] != nil || [NSApp mainWindow] != nil;
    }
    return YES;
}
@end

namespace gritty::platform::macos {

namespace {

GrittyAppDelegate* g_app_delegate = nil;

NSMenuItem* make_menu_item(
    NSString* title,
    SEL action,
    NSString* key,
    NSEventModifierFlags mask,
    id target
) {
    NSMenuItem* item = [[[NSMenuItem alloc] initWithTitle:title
                                                   action:action
                                            keyEquivalent:key] autorelease];
    item.keyEquivalentModifierMask = mask;
    item.target = target;
    return item;
}

NSMenu* build_submenu(NSString* title, NSArray<NSMenuItem*>* items) {
    NSMenu* menu = [[[NSMenu alloc] initWithTitle:title] autorelease];
    for (NSMenuItem* item in items) {
        [menu addItem:item];
    }
    return menu;
}

void setup_main_menu(GrittyAppDelegate* delegate) {
    NSMenu* main_menu = [[[NSMenu alloc] initWithTitle:@"MainMenu"] autorelease];
    auto add_top = [&](NSMenu* submenu) {
        NSMenuItem* top = [[[NSMenuItem alloc] initWithTitle:submenu.title
                                                      action:nil
                                               keyEquivalent:@""] autorelease];
        [top setSubmenu:submenu];
        [main_menu addItem:top];
    };

    NSMenu* app_menu = build_submenu(@"Gritty", @[
        make_menu_item(@"About Gritty", @selector(showAbout:), @"", 0, delegate),
        [NSMenuItem separatorItem],
        make_menu_item(@"Hide Gritty", @selector(hide:), @"h", NSEventModifierFlagCommand, NSApp),
        make_menu_item(@"Hide Others", @selector(hideOtherApplications:), @"h",
                       NSEventModifierFlagOption | NSEventModifierFlagCommand, NSApp),
        make_menu_item(@"Show All", @selector(unhideAllApplications:), @"", 0, NSApp),
        [NSMenuItem separatorItem],
        make_menu_item(@"Quit Gritty", @selector(requestQuit:), @"q",
                       NSEventModifierFlagCommand, delegate),
    ]);
    add_top(app_menu);

    NSMenu* file_menu = build_submenu(@"File", @[
        make_menu_item(@"New Window", @selector(newWindow:), @"n",
                       NSEventModifierFlagCommand, delegate),
        make_menu_item(@"Close Window", @selector(closeCurrentWindow:), @"w",
                       NSEventModifierFlagCommand, delegate),
    ]);
    add_top(file_menu);

    NSMenu* edit_menu = build_submenu(@"Edit", @[
        make_menu_item(@"Paste", @selector(paste:), @"v", NSEventModifierFlagCommand, nil),
    ]);
    add_top(edit_menu);

    NSMenu* view_menu = build_submenu(@"View", @[
        make_menu_item(@"Zoom In", @selector(zoomIn:), @"+", NSEventModifierFlagCommand, nil),
        make_menu_item(@"Zoom Out", @selector(zoomOut:), @"-", NSEventModifierFlagCommand, nil),
        make_menu_item(@"Actual Size", @selector(resetZoom:), @"0", NSEventModifierFlagCommand, nil),
        [NSMenuItem separatorItem],
        make_menu_item(@"Toggle Full Screen", @selector(toggleFullScreen:), @"f",
                       NSEventModifierFlagControl | NSEventModifierFlagCommand, nil),
    ]);
    add_top(view_menu);

    NSMenu* window_menu = build_submenu(@"Window", @[
        make_menu_item(@"Minimize", @selector(performMiniaturize:), @"m",
                       NSEventModifierFlagCommand, nil),
        make_menu_item(@"Zoom", @selector(performZoom:), @"", 0, nil),
        [NSMenuItem separatorItem],
        make_menu_item(@"Bring All to Front", @selector(arrangeInFront:), @"", 0, NSApp),
    ]);
    add_top(window_menu);

    [NSApp setMainMenu:main_menu];
    [NSApp setWindowsMenu:window_menu];
}

} // namespace

void ensure_cocoa_app_initialized() {
    if (g_app_initialized) return;
    g_app_initialized = true;
    @autoreleasepool {
        // Disable macOS accent press-and-hold menu so holding keys repeats cleanly
        // (matching terminal emulators) without popping up a character picker.
        NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
        [defaults registerDefaults:@{@"ApplePressAndHoldEnabled": @NO}];
        if ([defaults boolForKey:@"ApplePressAndHoldEnabled"]) {
            [defaults setBool:NO forKey:@"ApplePressAndHoldEnabled"];
        }
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
    }
}

void initialize_cocoa_application(AppDelegateCallbacks callbacks) {
    ensure_cocoa_app_initialized();
    g_app_callbacks = std::move(callbacks);
    @autoreleasepool {
        if (!g_app_delegate) {
            g_app_delegate = [[GrittyAppDelegate alloc] init];
        }
        [NSApp setDelegate:g_app_delegate];
        setup_main_menu(g_app_delegate);
    }
}

void shutdown_cocoa_application() {
    g_app_callbacks = {};
    @autoreleasepool {
        if (NSApp) {
            if ([NSApp windowsMenu]) [[NSApp windowsMenu] removeAllItems];
            [NSApp setWindowsMenu:nil];
            [NSApp setMainMenu:[[[NSMenu alloc] initWithTitle:@""] autorelease]];
        }
        if (NSApp && g_app_delegate && [NSApp delegate] == g_app_delegate) {
            [NSApp setDelegate:nil];
        }
        if (g_app_delegate) {
            [g_app_delegate release];
            g_app_delegate = nil;
        }
    }
}

} // namespace gritty::platform::macos
