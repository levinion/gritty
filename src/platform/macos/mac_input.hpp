#pragma once

#include "input/input_events.hpp"

#ifdef __OBJC__
@class NSEvent;
#else
class NSEvent;
#endif

namespace squirt::platform::macos {

class MacInput final {
public:
    static input::KeyEvent translate_key_event(NSEvent* event);
    static input::MouseEvent translate_mouse_event(NSEvent* event, int cell_w, int cell_h, double view_height);
    static input::Modifiers translate_modifiers(unsigned long flags);
};

} // namespace squirt::platform::macos
