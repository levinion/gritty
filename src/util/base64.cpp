#include "util/base64.hpp"

#include <algorithm>

namespace gritty::util {
namespace {

constexpr int value_of(char c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

constexpr bool is_whitespace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

} // namespace

std::optional<std::vector<uint8_t>> decode_base64(
    std::string_view input,
    Base64Mode mode,
    size_t max_bytes
) {
    std::vector<uint8_t> out;
    out.reserve(std::min(max_bytes, input.size()));

    uint32_t accumulator = 0;
    int bits = 0;
    for (const char c : input) {
        if (c == '=') break; // Padding: the remainder carries no data.
        const int value = value_of(c);
        if (value < 0) {
            if (mode == Base64Mode::Strict) return std::nullopt;
            if (is_whitespace(c)) continue;
            // Kitty's transmission decoder historically ignored stray bytes;
            // keep that behaviour for the relaxed mode.
            continue;
        }
        accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out.size() >= max_bytes) return std::nullopt;
            out.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xff));
        }
    }
    return out;
}

} // namespace gritty::util
