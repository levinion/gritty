#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

namespace gritty::core {

// Shared by configuration and the engine: customizing ANSI 0..15 must never
// replace the xterm cube/grayscale entries with zero-initialized black.
inline constexpr auto kDefaultPalette = [] {
    std::array<uint32_t, 256> palette{};
    constexpr std::array<uint32_t, 16> ansi{
        0x000000, 0xcc0403, 0x19cb00, 0xcecb00, 0x0d73cc, 0xcb1ed1, 0x0dcdcd, 0xdddddd,
        0x767676, 0xf2201f, 0x23fd00, 0xfffd00, 0x1a8fff, 0xfd28ff, 0x14ffff, 0xffffff,
    };
    for (std::size_t i = 0; i < ansi.size(); ++i) palette[i] = ansi[i];
    constexpr std::array<uint32_t, 6> steps{0, 95, 135, 175, 215, 255};
    std::size_t index = 16;
    for (auto r : steps) for (auto g : steps) for (auto b : steps)
        palette[index++] = (r << 16) | (g << 8) | b;
    for (uint32_t i = 0; i < 24; ++i) palette[232 + i] = (8 + i * 10) * 0x010101;
    return palette;
}();

} // namespace gritty::core
