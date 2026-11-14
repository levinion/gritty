#include "platform/macos/clipboard.hpp"
#import <Cocoa/Cocoa.h>

namespace squirt::platform::macos {

void PasteboardClipboard::copy_text(std::string_view text) {
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        NSString* str = [[NSString alloc] initWithBytes:text.data()
                                                 length:text.size()
                                               encoding:NSUTF8StringEncoding];
        if (str) {
            [pb setString:str forType:NSPasteboardTypeString];
        }
    }
}

std::string PasteboardClipboard::paste_text() {
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        NSString* str = [pb stringForType:NSPasteboardTypeString];
        if (!str) return "";
        return [str UTF8String];
    }
}

} // namespace squirt::platform::macos
