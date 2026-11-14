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
    static std::optional<util::PixmanImage> decode(std::string_view sixel_stream);
};

} // namespace gritty::core
