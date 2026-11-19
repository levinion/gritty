#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gritty::util {

enum class Base64Mode : uint8_t {
    Strict,  // Reject any character outside the base64 alphabet (OSC 52).
    Relaxed, // Ignore whitespace and other non-base64 bytes (Kitty payloads).
};

// Decodes standard base64. '=' starts padding and terminates decoding.
// `max_bytes` bounds the decoded output; Strict returns std::nullopt for both
// invalid characters and exceeded limits. Relaxed returns std::nullopt only
// when the limit is exceeded.
std::optional<std::vector<uint8_t>> decode_base64(
    std::string_view input,
    Base64Mode mode,
    size_t max_bytes
);

// Standard padded base64, used for OSC 52 clipboard read replies.
std::string encode_base64(std::string_view bytes);

} // namespace gritty::util
