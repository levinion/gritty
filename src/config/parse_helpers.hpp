#pragma once

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#if defined(__APPLE__)
#include <xlocale.h>
#endif

namespace gritty::config::detail {

[[nodiscard]] inline std::string_view trim_ascii(std::string_view text) noexcept {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

[[nodiscard]] inline bool equals_ignore_case(std::string_view text, std::string_view expected) noexcept {
    if (text.size() != expected.size()) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const char lowered = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        if (lowered != expected[i]) return false;
    }
    return true;
}

inline void lower_into(std::string& out, std::string_view text) {
    out.clear();
    out.reserve(text.size());
    for (const char c : text)
        out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
}

[[nodiscard]] inline std::string lower_ascii(std::string_view text) {
    std::string out;
    lower_into(out, text);
    return out;
}

[[nodiscard]] inline bool parse_integer(std::string_view text, int64_t& out) noexcept {
    const char* first = text.data();
    const char* last = first + text.size();
    if (first != last && *first == '+') ++first;
    if (first == last) return false;
    const auto [end, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && end == last;
}

[[nodiscard]] inline bool parse_double(std::string_view text, double& out) noexcept {
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    if (text.empty() || text.size() >= 64) return false;
    const unsigned char lead = static_cast<unsigned char>(text.front());
    if (!((lead >= '0' && lead <= '9') || lead == '-' || lead == '.')) return false;
    for (const char c : text) {
        if (c == 'x' || c == 'X') return false;
    }
    char buf[64];
    std::memcpy(buf, text.data(), text.size());
    buf[text.size()] = '\0';
    errno = 0;
    char* end = nullptr;
#if defined(__APPLE__)
    const double value = strtod_l(buf, &end, nullptr);
#else
    const double value = std::strtod(buf, &end);
#endif
    if (end != buf + text.size() || !std::isfinite(value) ||
        (errno == ERANGE && value != 0.0)) {
        return false;
    }
    out = value;
    return true;
}

} // namespace gritty::config::detail
