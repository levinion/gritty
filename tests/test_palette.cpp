#include <doctest/doctest.h>
#include "config/toml_loader.hpp"
#include "core/terminal_engine.hpp"

TEST_CASE("Custom ANSI colors preserve the xterm color cube and grayscale ramp") {
    const auto loaded = gritty::config::TomlLoader::load_from_string("[colors]\nregular1 = '#123456'\n");
    REQUIRE(loaded.has_value());
    const auto& palette = (*loaded)->colors.palette;
    CHECK(palette[1] == 0x123456);
    CHECK(palette[33] == 0x0087ff);
    CHECK(palette[196] == 0xff0000);
    CHECK(palette[244] == 0x808080);
    gritty::core::TerminalEngine engine(4, 2);
    engine.configure_palette(palette);
    engine.set_palette_color(1, 0xabcdef);
    engine.reset();
    CHECK(engine.palette_color(1) == 0x123456);
    CHECK(engine.palette_color(33) == 0x0087ff);
    CHECK(engine.palette_color(196) == 0xff0000);
    CHECK(engine.palette_color(244) == 0x808080);
}
