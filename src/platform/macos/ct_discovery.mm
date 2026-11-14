#include "platform/macos/ct_discovery.hpp"
#import <CoreText/CoreText.h>
#import <CoreFoundation/CoreFoundation.h>

namespace gritty::platform::macos {

std::optional<FontMatch> CoreTextFinder::find_font(const FontPattern& pattern) {
    @autoreleasepool {
        CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);

        CFStringRef family_str = CFStringCreateWithCString(
            kCFAllocatorDefault, pattern.family.c_str(), kCFStringEncodingUTF8);
        CFDictionarySetValue(attributes, kCTFontFamilyNameAttribute, family_str);
        CFRelease(family_str);

        CTFontSymbolicTraits traits = 0;
        if (pattern.bold) traits |= kCTFontBoldTrait;
        if (pattern.italic) traits |= kCTFontItalicTrait;

        if (traits != 0) {
            CFNumberRef traits_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &traits);
            CFMutableDictionaryRef traits_dict = CFDictionaryCreateMutable(
                kCFAllocatorDefault, 0,
                &kCFTypeDictionaryKeyCallBacks,
                &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(traits_dict, kCTFontSymbolicTrait, traits_num);
            CFDictionarySetValue(attributes, kCTFontTraitsAttribute, traits_dict);
            CFRelease(traits_num);
            CFRelease(traits_dict);
        }

        CTFontDescriptorRef descriptor = CTFontDescriptorCreateWithAttributes(attributes);
        CFRelease(attributes);

        CTFontDescriptorRef matched = CTFontDescriptorCreateMatchingFontDescriptor(descriptor, nullptr);
        CFRelease(descriptor);

        if (!matched) return std::nullopt;

        CFURLRef url = (CFURLRef)CTFontDescriptorCopyAttribute(matched, kCTFontURLAttribute);
        CFRelease(matched);

        if (!url) return std::nullopt;

        char path_buf[PATH_MAX];
        Boolean success = CFURLGetFileSystemRepresentation(url, true, (UInt8*)path_buf, sizeof(path_buf));
        CFRelease(url);

        if (!success) return std::nullopt;

        return FontMatch{
            .path = std::string(path_buf),
            .face_index = 0
        };
    }
}

std::vector<FontMatch> CoreTextFinder::fallback_chain(const FontPattern& pattern) {
    std::vector<FontMatch> results;
    auto primary = find_font(pattern);
    if (primary) {
        results.push_back(*primary);
    }

    // Common system fallback monospace fonts
    const char* fallbacks[] = {
        "Menlo",
        "Monaco",
        "SF Mono",
        "Apple Color Emoji",
        "PingFang SC"
    };

    for (const char* name : fallbacks) {
        if (name == pattern.family) continue;
        auto m = find_font(FontPattern{.family = name, .bold = pattern.bold, .italic = pattern.italic});
        if (m) {
            results.push_back(*m);
        }
    }

    return results;
}

} // namespace gritty::platform::macos
