#pragma once

#include <string_view>

#ifndef GRITTY_VERSION
// Standalone builds outside Meson still get a usable XTVERSION reply.
#define GRITTY_VERSION "2026.11.26.1"
#endif

namespace gritty::util {

inline constexpr std::string_view kVersion = GRITTY_VERSION;

} // namespace gritty::util
