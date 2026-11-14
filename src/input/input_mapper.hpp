#pragma once

#include <string>
#include <string_view>
#include "input/input_events.hpp"
#include "core/terminal_engine.hpp"

namespace squirt::input {

class InputMapper final {
public:
    static std::string map_key(const KeyEvent& ev, const core::TerminalEngine& engine);
    static std::string map_mouse(const MouseEvent& ev, const core::TerminalEngine& engine);
};

} // namespace squirt::input
