#include <doctest/doctest.h>
#include "config/toml_loader.hpp"

#include <utility>

using namespace gritty::config;

TEST_CASE("TomlLoader parse basic config and hex colors") {
    std::string toml_str = R"(
        [main]
        term = "foot"
        title = "My Term"
        osc52 = "enabled"

        [font]
        family = "CustomFont"
        size = 15.5
        dpi = 144
        antialias = "subpixel"

        [cursor]
        style = "beam"
        blink = true
        color = "#ff5500"

        [colors]
        foreground = "#abcdef"
        background = "#123456"
        regular1 = "#ff0000"
    )";

    auto res = TomlLoader::load_from_string(toml_str);
    REQUIRE(res.has_value());
    auto cfg = *res;

    CHECK(cfg->main.term == "foot");
    CHECK(cfg->main.title == "My Term");
    CHECK(cfg->main.osc52 == Osc52Mode::Enabled);
    CHECK(cfg->font.family == "CustomFont");
    CHECK(cfg->font.size == 15.5);
    CHECK(cfg->font.dpi == 144);
    CHECK(cfg->font.antialias == gritty::render::AntialiasMode::SubpixelLcd);

    CHECK(cfg->cursor.style == gritty::core::CursorShape::Beam);
    CHECK(cfg->cursor.blink == true);
    CHECK(cfg->cursor.color == 0xFFFF5500);

    CHECK(cfg->colors.foreground == 0xFFABCDEF);
    CHECK(cfg->colors.background == 0xFF123456);
    CHECK(cfg->colors.palette[1] == 0x00FF0000);
}

TEST_CASE("TomlLoader deep-merge platform overrides") {
    std::string toml_str = R"(
        [font]
        family = "DefaultFont"
        size = 12.0

        [platform.macos]
        font = { family = "MacFont", size = 14.5 }

        [platform.linux]
        font = { family = "LinuxFont" }
    )";

    // Force platform override "macos"
    auto res_mac = TomlLoader::load_from_string(toml_str, "macos");
    REQUIRE(res_mac.has_value());
    CHECK((*res_mac)->font.family == "MacFont");
    CHECK((*res_mac)->font.size == 14.5);

    // Force platform override "linux"
    auto res_linux = TomlLoader::load_from_string(toml_str, "linux");
    REQUIRE(res_linux.has_value());
    CHECK((*res_linux)->font.family == "LinuxFont");
    CHECK((*res_linux)->font.size == 12.0); // inherits default size
}

TEST_CASE("Background opacity accepts numeric alpha and composes with AARRGGBB") {
    auto result = TomlLoader::load_from_string("[colors]\nbackground = '#604020'\nalpha = 0.5\n");
    REQUIRE(result.has_value());
    CHECK((*result)->colors.alpha == 0.5);
    CHECK((*result)->colors.background_argb() == 0x80604020);
    result = TomlLoader::load_from_string("[colors]\nbackground = '#80604020'\nalpha = 0.5\n");
    REQUIRE(result.has_value());
    CHECK((*result)->colors.background_argb() == 0x40604020);
    result = TomlLoader::load_from_string("[colors]\nalpha = 0\n");
    REQUIRE(result.has_value());
    CHECK((*result)->colors.background_argb() == 0x00111111);
    result = TomlLoader::load_from_string("[colors]\nalpha = 1\n");
    REQUIRE(result.has_value());
    CHECK((*result)->colors.background_argb() == 0xff111111);
    result = TomlLoader::load_from_string("[colors]\nalpha = 0.8\n[platform.macos.colors]\nalpha = 0.25\n", "macos");
    REQUIRE(result.has_value());
    CHECK((*result)->colors.background_argb() == 0x40111111);
}

TEST_CASE("Full decorations force an opaque background over configured alpha") {
    using gritty::platform::WindowDecorations;
    auto loaded = TomlLoader::load_from_string(
        "[colors]\nbackground = '#604020'\nalpha = 0.5\n");
    REQUIRE(loaded.has_value());
    auto cfg = *loaded;
    CHECK(cfg->colors.background_argb() == 0x80604020); // Raw value is unchanged.
    CHECK(cfg->window.decorations == WindowDecorations::Full);
    CHECK(cfg->effective_background_argb() == 0xff604020);

    cfg->window.decorations = WindowDecorations::Transparent;
    CHECK(cfg->effective_background_argb() == 0x80604020);
    cfg->window.decorations = WindowDecorations::Buttonless;
    CHECK(cfg->effective_background_argb() == 0x80604020);
    cfg->window.decorations = WindowDecorations::None;
    CHECK(cfg->effective_background_argb() == 0x80604020);

    cfg->window.decorations = WindowDecorations::Full;
    REQUIRE(cfg->apply_override("colors.alpha=0.25").has_value());
    CHECK(cfg->effective_background_argb() == 0xff604020);
}

TEST_CASE("Background opacity rejects invalid values") {
    for (const auto* value : {"-0.1", "1.1", "nan", "inf", "'opaque'", "true", "[]"}) {
        CAPTURE(value);
        const auto result = TomlLoader::load_from_string(std::string("[colors]\nalpha = ") + value + "\n");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message.find("colors.alpha") != std::string::npos);
    }
    CHECK_FALSE(TomlLoader::parse_color("#8000000g").has_value());
    CHECK_FALSE(TomlLoader::parse_color("#12345g").has_value());
}

TEST_CASE("TomlLoader parse hex colors utility") {
    CHECK(TomlLoader::parse_color("#ff0000") == 0xFFFF0000);
    CHECK(TomlLoader::parse_color("00ff00") == 0xFF00FF00);
    CHECK(TomlLoader::parse_color("0x0000ff") == 0xFF0000FF);
    CHECK(TomlLoader::parse_color("#80112233") == 0x80112233);
    CHECK(TomlLoader::parse_color("invalid") == std::nullopt);
}

TEST_CASE("TomlLoader parses window decorations") {
    using gritty::platform::WindowDecorations;

    CHECK(parse_window_decorations("full") == WindowDecorations::Full);
    CHECK(parse_window_decorations("NONE") == WindowDecorations::None);
    CHECK(parse_window_decorations("Transparent") == WindowDecorations::Transparent);
    CHECK(parse_window_decorations("BUTTONLESS") == WindowDecorations::Buttonless);
    CHECK_FALSE(parse_window_decorations("borderless").has_value());

    for (const auto pair : {
        std::pair<const char*, WindowDecorations>{"full", WindowDecorations::Full},
        {"none", WindowDecorations::None},
        {"Transparent", WindowDecorations::Transparent},
        {"BUTTONLESS", WindowDecorations::Buttonless},
    }) {
        const auto result = TomlLoader::load_from_string(
            std::string("[window]\ndecorations = \"") + pair.first + "\"\n");
        REQUIRE(result.has_value());
        CHECK((*result)->window.decorations == pair.second);
    }

    const auto invalid = TomlLoader::load_from_string("[window]\ndecorations = \"invalid\"\n");
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().message.find("window.decorations") != std::string::npos);

    auto cfg = AppConfig{};
    const auto overridden = cfg.apply_override("window.decorations=TRANSPARENT");
    REQUIRE(overridden.has_value());
    CHECK(cfg.window.decorations == WindowDecorations::Transparent);
    CHECK_FALSE(cfg.apply_override("window.decorations=invalid").has_value());
}

TEST_CASE("OSC 52 modes parse case-insensitively from TOML and overrides") {
    CHECK(parse_osc52_mode("DISABLED") == Osc52Mode::Disabled);
    CHECK(parse_osc52_mode("Copy-Only") == Osc52Mode::CopyOnly);
    CHECK(parse_osc52_mode("PASTE-ONLY") == Osc52Mode::PasteOnly);
    CHECK(parse_osc52_mode("Enabled") == Osc52Mode::Enabled);
    CHECK_FALSE(parse_osc52_mode("read").has_value());
    CHECK(AppConfig{}.main.osc52 == Osc52Mode::CopyOnly);

    for (const auto pair : {
        std::pair<const char*, Osc52Mode>{"disabled", Osc52Mode::Disabled},
        {"COPY-ONLY", Osc52Mode::CopyOnly},
        {"paste-only", Osc52Mode::PasteOnly},
        {"ENABLED", Osc52Mode::Enabled},
    }) {
        const auto result = TomlLoader::load_from_string(
            std::string("[main]\nosc52 = \"") + pair.first + "\"\n");
        REQUIRE(result.has_value());
        CHECK((*result)->main.osc52 == pair.second);

        AppConfig cfg;
        const auto overridden = cfg.apply_override(
            std::string("main.osc52=") + pair.first);
        REQUIRE(overridden.has_value());
        CHECK(cfg.main.osc52 == pair.second);
    }

    // Legacy boolean key maps to the closest four-state behaviour.
    const auto legacy = TomlLoader::load_from_string("[main]\nosc52_read = true\n");
    REQUIRE(legacy.has_value());
    CHECK((*legacy)->main.osc52 == Osc52Mode::Enabled);
    AppConfig cfg;
    REQUIRE(cfg.apply_override("main.osc52_read=false").has_value());
    CHECK(cfg.main.osc52 == Osc52Mode::CopyOnly);
    CHECK_FALSE(cfg.apply_override("main.osc52=third-party").has_value());
}

TEST_CASE("Window padding parses scalar, table and override forms") {
    auto result = TomlLoader::load_from_string("[window]\npadding = 8\n");
    REQUIRE(result.has_value());
    CHECK((*result)->window.padding.x == 8);
    CHECK((*result)->window.padding.y == 8);

    result = TomlLoader::load_from_string("[window]\npadding = { x = 4, y = 9 }\n");
    REQUIRE(result.has_value());
    CHECK((*result)->window.padding.x == 4);
    CHECK((*result)->window.padding.y == 9);

    result = TomlLoader::load_from_string(
        "[platform.macos.window]\npadding = { x = 6, y = 2 }\n", "macos");
    REQUIRE(result.has_value());
    CHECK((*result)->window.padding.x == 6);
    CHECK((*result)->window.padding.y == 2);

    AppConfig cfg;
    REQUIRE(cfg.apply_override("window.padding.x=3").has_value());
    REQUIRE(cfg.apply_override("window.padding.y=7").has_value());
    CHECK(cfg.window.padding.x == 3);
    CHECK(cfg.window.padding.y == 7);
    REQUIRE(cfg.apply_override("window.padding=5").has_value());
    CHECK(cfg.window.padding.x == 5);
    CHECK(cfg.window.padding.y == 5);

    CHECK_FALSE(cfg.apply_override("window.padding=-1").has_value());
    CHECK_FALSE(cfg.apply_override("window.padding.x=513").has_value());
    CHECK_FALSE(TomlLoader::load_from_string("[window]\npadding = -2\n").has_value());
}
