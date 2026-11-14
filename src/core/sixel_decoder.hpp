#pragma once

#include <cstdint>
#include <string_view>
#include <optional>
#include <vector>
#include <array>
#include "util/raii.hpp"

namespace squirt::core {

class SixelDecoder final {
public:
    static std::optional<util::PixmanImage> decode(std::string_view sixel_stream);
};

} // namespace squirt::core
