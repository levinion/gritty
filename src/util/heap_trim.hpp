#pragma once

namespace gritty::util {

// One-shot startup cleanup, never called from rendering/input/PTY paths.
// Releases pages that font discovery and config parsing left fragmented.
void trim_transient_heap() noexcept;

} // namespace gritty::util
