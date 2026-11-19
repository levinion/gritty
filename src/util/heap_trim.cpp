#include "util/heap_trim.hpp"

#if defined(PLATFORM_LINUX)
#include <malloc.h>
#elif defined(PLATFORM_MACOS)
#include <malloc/malloc.h>
#endif

namespace gritty::util {

void trim_transient_heap() noexcept {
#if defined(PLATFORM_LINUX)
    ::malloc_trim(0);
#elif defined(PLATFORM_MACOS)
    ::malloc_zone_pressure_relief(nullptr, 0);
#endif
}

} // namespace gritty::util
