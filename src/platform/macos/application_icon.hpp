#pragma once
#include <CoreGraphics/CoreGraphics.h>

namespace gritty::platform::macos {

// Caller owns the returned image. Pick an appropriate ICNS representation and
// downsample before decoding, rather than retaining the full 1024px image.
CGImageRef copy_application_icon(const char* path, int max_pixels = 256);

} // namespace gritty::platform::macos
