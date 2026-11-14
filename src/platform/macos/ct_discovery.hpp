#pragma once

#include "platform/pal_interfaces.hpp"

namespace squirt::platform::macos {

class CoreTextFinder final : public IFontDiscovery {
public:
    CoreTextFinder() = default;
    ~CoreTextFinder() override = default;

    std::optional<FontMatch> find_font(const FontPattern& pattern) override;
    std::vector<FontMatch> fallback_chain(const FontPattern& pattern) override;
};

} // namespace squirt::platform::macos
