#pragma once

#include <cstdint>
#include <string_view>
#include <optional>
#include <vector>
#include <array>
#include "util/raii.hpp"

namespace gritty::core {

class SixelDecoder final {
public:
    // `sixel_stream` may include the leading 'q'. P2=1 requests a transparent
    // background; otherwise palette[0] fills uncovered pixels.
    static std::optional<util::PixmanImage> decode(
        std::string_view sixel_stream, bool background_transparent = false);
};

} // namespace gritty::core
