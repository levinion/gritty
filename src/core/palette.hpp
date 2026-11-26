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
        0x282c34, 0xf67e7d, 0xcae7b9, 0xe1dd8f, 0x7cb7ff, 0xd7bcc8, 0x00c1e4, 0xdcdfe4,
        0x282c34, 0xf67e7d, 0xcae7b9, 0xe1dd8f, 0x7cb7ff, 0xd7bcc8, 0x00c1e4, 0xdcdfe4,
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
