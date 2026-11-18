#pragma once

#include <format>
#include <string_view>
#include <utility>

namespace gritty::util {

// Formats into a caller-owned stack buffer using std::format_to_n. Returns an
// empty view if the value does not fit; the buffer is always NUL-terminated.
template<size_t N, class... Args>
std::string_view format_to_buffer(
    char (&buffer)[N],
    std::format_string<Args...> fmt,
    Args&&... args
) {
    static_assert(N > 0);
    const auto result = std::format_to_n(
        buffer, N - 1, fmt, std::forward<Args>(args)...);
    *result.out = '\0';
    if (static_cast<size_t>(result.size) >= N) return {};
    return std::string_view(buffer, static_cast<size_t>(result.out - buffer));
}

} // namespace gritty::util
