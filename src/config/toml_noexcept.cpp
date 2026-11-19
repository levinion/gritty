// Single translation unit providing the TOML++ implementation with exceptions
// disabled. Consumers compile against the same TOML_EXCEPTIONS=0 ABI.
#define TOML_IMPLEMENTATION 1
#include <toml++/toml.hpp>
