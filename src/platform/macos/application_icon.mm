#include "platform/macos/application_icon.hpp"
#import <ImageIO/ImageIO.h>
#include <algorithm>
#include <cstring>
#include <limits>

namespace gritty::platform::macos {

CGImageRef copy_application_icon(const char* path, int max_pixels) {
    if (!path || !*path || max_pixels <= 0 || max_pixels > 1024) return nullptr;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr,
        reinterpret_cast<const UInt8*>(path), std::strlen(path), false);
    if (!url) return nullptr;
    const void* source_keys[] = {kCGImageSourceShouldCache};
    const void* source_values[] = {kCFBooleanFalse};
    CFDictionaryRef options = CFDictionaryCreate(nullptr, source_keys, source_values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!options) { CFRelease(url); return nullptr; }
    CGImageSourceRef source = CGImageSourceCreateWithURL(url, options);
    CFRelease(url);
    CFRelease(options);
    if (!source) return nullptr;

    size_t selected = 0;
    int best_large = std::numeric_limits<int>::max(), best_small = 0;
    const size_t count = CGImageSourceGetCount(source);
    for (size_t i = 0; i < count; ++i) {
        CFDictionaryRef properties = CGImageSourceCopyPropertiesAtIndex(source, i, nullptr);
        if (!properties) continue;
        int width = 0, height = 0;
        auto w = static_cast<CFNumberRef>(CFDictionaryGetValue(properties, kCGImagePropertyPixelWidth));
        auto h = static_cast<CFNumberRef>(CFDictionaryGetValue(properties, kCGImagePropertyPixelHeight));
        if (w) CFNumberGetValue(w, kCFNumberIntType, &width);
        if (h) CFNumberGetValue(h, kCFNumberIntType, &height);
        CFRelease(properties);
        const int dimension = std::max(width, height);
        if (dimension >= max_pixels && dimension < best_large) {
            best_large = dimension;
            selected = i;
        } else if (best_large == std::numeric_limits<int>::max() && dimension > best_small) {
            best_small = dimension;
            selected = i;
        }
    }

    CFNumberRef maximum = CFNumberCreate(nullptr, kCFNumberIntType, &max_pixels);
    if (!maximum) { CFRelease(source); return nullptr; }
    const void* keys[] = {kCGImageSourceCreateThumbnailFromImageAlways,
        kCGImageSourceThumbnailMaxPixelSize, kCGImageSourceShouldCacheImmediately,
        kCGImageSourceCreateThumbnailWithTransform};
    const void* values[] = {kCFBooleanTrue, maximum, kCFBooleanTrue, kCFBooleanTrue};
    CFDictionaryRef thumbnail_options = CFDictionaryCreate(nullptr, keys, values, 4,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!thumbnail_options) { CFRelease(maximum); CFRelease(source); return nullptr; }
    CGImageRef image = count > 0 ? CGImageSourceCreateThumbnailAtIndex(source, selected, thumbnail_options) : nullptr;
    CFRelease(thumbnail_options);
    CFRelease(maximum);
    CFRelease(source);
    return image;
}

} // namespace gritty::platform::macos
