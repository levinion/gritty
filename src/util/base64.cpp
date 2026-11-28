#include "util/base64.hpp"

#include <algorithm>
#include <array>
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

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

#if defined(__aarch64__) || defined(_M_ARM64)
constexpr std::array<uint8_t, 64> make_lut(int offset) noexcept {
    std::array<uint8_t, 64> t{};
    for (int i = 0; i < 64; ++i) {
        const int v = value_of(static_cast<char>(offset + i));
        t[static_cast<size_t>(i)] = v >= 0 ? static_cast<uint8_t>(v + 1) : 0;
    }
    return t;
}
#endif

} // namespace

std::string encode_base64(std::string_view bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const uint32_t word = (static_cast<uint32_t>(static_cast<uint8_t>(bytes[i])) << 16) |
            (static_cast<uint32_t>(static_cast<uint8_t>(bytes[i + 1])) << 8) |
            static_cast<uint32_t>(static_cast<uint8_t>(bytes[i + 2]));
        out.push_back(alphabet[(word >> 18) & 63]);
        out.push_back(alphabet[(word >> 12) & 63]);
        out.push_back(alphabet[(word >> 6) & 63]);
        out.push_back(alphabet[word & 63]);
    }
    if (i < bytes.size()) {
        uint32_t word = static_cast<uint32_t>(static_cast<uint8_t>(bytes[i])) << 16;
        if (i + 1 < bytes.size()) word |= static_cast<uint32_t>(static_cast<uint8_t>(bytes[i + 1])) << 8;
        out.push_back(alphabet[(word >> 18) & 63]);
        out.push_back(alphabet[(word >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? alphabet[(word >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::optional<std::vector<uint8_t>> decode_base64(
    std::string_view input,
    Base64Mode mode,
    size_t max_bytes
) {
    std::vector<uint8_t> out;
    out.reserve(std::min(max_bytes, (input.size() / 4) * 3 + 3));

    uint32_t accumulator = 0;
    int bits = 0;
    size_t i = 0;
    while (i < input.size()) {
#if defined(__aarch64__) || defined(_M_ARM64)
        if (bits == 0 && i + 64 <= input.size() && out.size() + 48 <= max_bytes) {
            static constexpr auto kLutLo = make_lut(0);
            static constexpr auto kLutHi = make_lut(64);
            const uint8x16x4_t tbl_lo = {{
                vld1q_u8(kLutLo.data()), vld1q_u8(kLutLo.data() + 16),
                vld1q_u8(kLutLo.data() + 32), vld1q_u8(kLutLo.data() + 48)
            }};
            const uint8x16x4_t tbl_hi = {{
                vld1q_u8(kLutHi.data()), vld1q_u8(kLutHi.data() + 16),
                vld1q_u8(kLutHi.data() + 32), vld1q_u8(kLutHi.data() + 48)
            }};
            const uint8x16_t v64 = vdupq_n_u8(64);
            const uint8x16_t v1 = vdupq_n_u8(1);
            while (i + 64 <= input.size() && out.size() + 48 <= max_bytes) {
                const uint8x16x4_t in = vld4q_u8(
                    reinterpret_cast<const uint8_t*>(input.data() + i));
                uint8x16_t p1[4];
                for (int k = 0; k < 4; ++k) {
                    p1[k] = vorrq_u8(
                        vqtbl4q_u8(tbl_lo, in.val[k]),
                        vqtbl4q_u8(tbl_hi, vsubq_u8(in.val[k], v64)));
                }
                const uint8x16_t ok = vminq_u8(
                    vminq_u8(p1[0], p1[1]), vminq_u8(p1[2], p1[3]));
                if (vminvq_u8(ok) == 0) break;
                const uint8x16_t s0 = vsubq_u8(p1[0], v1);
                const uint8x16_t s1 = vsubq_u8(p1[1], v1);
                const uint8x16_t s2 = vsubq_u8(p1[2], v1);
                const uint8x16_t s3 = vsubq_u8(p1[3], v1);
                const uint8x16x3_t packed = {{
                    vorrq_u8(vshlq_n_u8(s0, 2), vshrq_n_u8(s1, 4)),
                    vorrq_u8(vshlq_n_u8(s1, 4), vshrq_n_u8(s2, 2)),
                    vorrq_u8(vshlq_n_u8(s2, 6), s3),
                }};
                const size_t pos = out.size();
                out.resize(pos + 48);
                vst3q_u8(out.data() + pos, packed);
                i += 64;
            }
            if (i >= input.size()) break;
        }
#endif
        const char c = input[i++];
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
