#pragma once

#include "platform/pal_interfaces.hpp"

namespace squirt::platform::macos {

class PasteboardClipboard final : public IClipboard {
public:
    PasteboardClipboard() = default;
    ~PasteboardClipboard() override = default;

    void copy_text(std::string_view text) override;
    std::string paste_text() override;
};

} // namespace squirt::platform::macos
