#include "platform/macos/ct_discovery.hpp"
#import <CoreText/CoreText.h>
#import <CoreFoundation/CoreFoundation.h>

namespace gritty::platform::macos {
namespace {

CTFontDescriptorRef create_matching_descriptor(const FontPattern& pattern) {
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!attributes) return nullptr;

    CFStringRef family_str = CFStringCreateWithCString(
        kCFAllocatorDefault, pattern.family.c_str(), kCFStringEncodingUTF8);
    if (family_str) {
        CFDictionarySetValue(attributes, kCTFontFamilyNameAttribute, family_str);
        CFRelease(family_str);
    }

    CTFontSymbolicTraits traits = 0;
    if (pattern.bold) traits |= kCTFontBoldTrait;
    if (pattern.italic) traits |= kCTFontItalicTrait;

    if (traits != 0) {
        CFNumberRef traits_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &traits);
        CFMutableDictionaryRef traits_dict = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        if (traits_num && traits_dict) {
            CFDictionarySetValue(traits_dict, kCTFontSymbolicTrait, traits_num);
            CFDictionarySetValue(attributes, kCTFontTraitsAttribute, traits_dict);
        }
        if (traits_num) CFRelease(traits_num);
        if (traits_dict) CFRelease(traits_dict);
    }

    CTFontDescriptorRef descriptor = CTFontDescriptorCreateWithAttributes(attributes);
    CFRelease(attributes);
    if (!descriptor) return nullptr;

    const void* mandatory_keys[2] = {kCTFontFamilyNameAttribute, kCTFontTraitsAttribute};
    CFIndex mandatory_count = traits != 0 ? 2 : 1;
    CFSetRef mandatory = CFSetCreate(
        kCFAllocatorDefault, mandatory_keys, mandatory_count, &kCFTypeSetCallBacks);
    CTFontDescriptorRef matched =
        CTFontDescriptorCreateMatchingFontDescriptor(descriptor, mandatory);
    if (mandatory) CFRelease(mandatory);
    CFRelease(descriptor);
    return matched;
}

std::optional<FontMatch> match_from_descriptor(CTFontDescriptorRef descriptor) {
    if (!descriptor) return std::nullopt;
    CFURLRef url = (CFURLRef)CTFontDescriptorCopyAttribute(descriptor, kCTFontURLAttribute);
    if (!url) return std::nullopt;

    char path_buf[PATH_MAX];
    Boolean success = CFURLGetFileSystemRepresentation(url, true, (UInt8*)path_buf, sizeof(path_buf));
    CFRelease(url);
    if (!success) return std::nullopt;

    std::string ps_name;
    CFStringRef ps_ref = (CFStringRef)CTFontDescriptorCopyAttribute(descriptor, kCTFontNameAttribute);
    if (ps_ref) {
        char ps_buf[512];
        if (CFStringGetCString(ps_ref, ps_buf, sizeof(ps_buf), kCFStringEncodingUTF8)) {
            ps_name = ps_buf;
        }
        CFRelease(ps_ref);
    }

    return FontMatch{
        .path = std::string(path_buf),
        .face_index = 0,
        .postscript_name = std::move(ps_name),
    };
}

} // namespace

std::optional<FontMatch> CoreTextFinder::find_font(const FontPattern& pattern) {
    @autoreleasepool {
        CTFontDescriptorRef matched = create_matching_descriptor(pattern);
        if (!matched) return std::nullopt;
        auto match = match_from_descriptor(matched);
        CFRelease(matched);
        return match;
    }
}

std::vector<FontMatch> CoreTextFinder::fallback_chain(const FontPattern& pattern) {
    std::vector<FontMatch> results;
    auto add_unique = [&](const FontMatch& m) {
        for (const auto& existing : results) {
            if (existing.path == m.path && existing.postscript_name == m.postscript_name) return;
        }
        results.push_back(m);
    };

    @autoreleasepool {
        CTFontDescriptorRef matched = create_matching_descriptor(pattern);
        if (matched) {
            if (auto primary = match_from_descriptor(matched)) {
                add_unique(*primary);
            }
            CTFontRef primary_ct = CTFontCreateWithFontDescriptor(matched, 12.0, nullptr);
            if (primary_ct) {
                CFArrayRef cascade = CTFontCopyDefaultCascadeListForLanguages(primary_ct, nullptr);
                if (cascade) {
                    const CFIndex count = CFArrayGetCount(cascade);
                    for (CFIndex i = 0; i < count; ++i) {
                        auto desc = (CTFontDescriptorRef)CFArrayGetValueAtIndex(cascade, i);
                        if (!desc) continue;
                        CTFontDescriptorRef resolved = CTFontDescriptorCreateMatchingFontDescriptor(desc, nullptr);
                        CTFontDescriptorRef target = resolved ? resolved : desc;
                        if (auto m = match_from_descriptor(target)) {
                            add_unique(*m);
                        }
                        if (resolved) CFRelease(resolved);
                    }
                    CFRelease(cascade);
                }
                CFRelease(primary_ct);
            }
            CFRelease(matched);
        }
    }

    // Common system fallback monospace/CJK/emoji fonts
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
        if (m) add_unique(*m);
    }

    return results;
}

} // namespace gritty::platform::macos
