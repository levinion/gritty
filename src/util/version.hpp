#pragma once

#include <string_view>

#ifndef GRITTY_VERSION
// Standalone builds outside Meson still get a usable XTVERSION reply.
#define GRITTY_VERSION "0.1.0"
#endif

namespace gritty::util {

inline constexpr std::string_view kVersion = GRITTY_VERSION;

} // namespace gritty::util
