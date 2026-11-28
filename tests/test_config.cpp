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

    // Partial table keeps the other axis's default/base value.
    result = TomlLoader::load_from_string("[window]\npadding = { x = 12 }\n");
    REQUIRE(result.has_value());
    CHECK((*result)->window.padding.x == 12);
    CHECK((*result)->window.padding.y == 0);
}

TEST_CASE("colors.cursor sets cursor.color unless cursor.color is explicit, and validation catches errors") {
    auto res = TomlLoader::load_from_string("[colors]\ncursor = '#112233'\n");
    REQUIRE(res.has_value());
    CHECK((*res)->colors.cursor == 0xFF112233);
    CHECK((*res)->cursor.color == 0xFF112233);

    res = TomlLoader::load_from_string("[cursor]\ncolor = '#445566'\n[colors]\ncursor = '#112233'\n");
    REQUIRE(res.has_value());
    CHECK((*res)->cursor.color == 0xFF445566);

    CHECK_FALSE(TomlLoader::load_from_string("[font]\nsize = 0\n").has_value());
    CHECK_FALSE(TomlLoader::load_from_string("[font]\nantialias = 'bogus'\n").has_value());
    CHECK_FALSE(TomlLoader::load_from_string("[cursor]\nstyle = 'triangle'\n").has_value());
    CHECK_FALSE(TomlLoader::load_from_string("[main]\nosc52 = 'bogus'\n").has_value());
    CHECK_FALSE(TomlLoader::load_from_string("[window]\ndecorations = false\n").has_value());
}

TEST_CASE("Default AppConfig matches Alacritty font and window defaults") {
    const AppConfig cfg;
    CHECK(cfg.font.size == 11.25);
#if defined(PLATFORM_MACOS)
    CHECK(cfg.font.family == "Menlo");
    CHECK(cfg.font.dpi == 72);
#else
    CHECK(cfg.font.family == "monospace");
    CHECK(cfg.font.dpi == 96);
#endif
    CHECK(cfg.window.decorations == gritty::platform::WindowDecorations::Full);
    CHECK(cfg.window.padding.x == 0);
    CHECK(cfg.window.padding.y == 0);
    CHECK(cfg.colors.alpha == 1.0);
    CHECK(cfg.cursor.style == gritty::core::CursorShape::Block);
    CHECK(cfg.cursor.blink == false);
    CHECK(cfg.main.osc52 == Osc52Mode::CopyOnly);
}

TEST_CASE("TomlLoader and overrides support Alacritty TOML schema") {
    const std::string toml_str = R"(
        [general]
        working_directory = "/tmp/work"

        [env]
        TERM = "tmux-256color"
        EDITOR = "nvim"

        [window]
        title = "Alacritty Style"
        class = { general = "top.maruka.gritty" }
        decorations = "Transparent"
        opacity = 0.85
        padding = { x = 10, y = 6 }
        dimensions = { columns = 120, lines = 36 }

        [font]
        normal = { family = "JetBrainsMono Nerd Font" }
        size = 12.5
        dpi = 72
        antialias = "subpixel"

        [cursor]
        style = { shape = "Beam", blinking = "On" }

        [colors.primary]
        foreground = "#c0caf5"
        background = "#1a1b26"

        [colors.cursor]
        cursor = "#ff9e64"

        [colors.normal]
        black = "#15161e"
        red = "#f7768e"

        [colors.bright]
        cyan = "#7dcfff"
        white = "#c0caf5"

        [terminal]
        osc52 = "CopyPaste"

        [terminal.shell]
        program = "/bin/zsh"
        args = ["-l", "-i"]

        [platform.macos.font]
        normal = { family = "Menlo" }
        size = 11.25
    )";

    auto res = TomlLoader::load_from_string(toml_str, "macos");
    REQUIRE(res.has_value());
    const auto cfg = *res;

    CHECK(cfg->main.working_directory == "/tmp/work");
    CHECK(cfg->main.term == "tmux-256color");
    REQUIRE(cfg->main.env.size() == 1);
    CHECK(cfg->main.env[0] == "EDITOR=nvim");
    CHECK(cfg->main.title == "Alacritty Style");
    CHECK(cfg->main.app_id == "top.maruka.gritty");
    CHECK(cfg->window.decorations == gritty::platform::WindowDecorations::Transparent);
    CHECK(cfg->colors.alpha == 0.85);
    CHECK(cfg->window.padding.x == 10);
    CHECK(cfg->window.padding.y == 6);
    CHECK(cfg->window.columns == 120);
    CHECK(cfg->window.lines == 36);
    CHECK(cfg->font.family == "Menlo");
    CHECK(cfg->font.size == 11.25);
    CHECK(cfg->font.antialias == gritty::render::AntialiasMode::SubpixelLcd);
    CHECK(cfg->cursor.style == gritty::core::CursorShape::Beam);
    CHECK(cfg->cursor.blink == true);
    CHECK(cfg->colors.foreground == 0xffc0caf5);
    CHECK(cfg->colors.background == 0xff1a1b26);
    CHECK(cfg->colors.cursor == 0xffff9e64);
    CHECK(cfg->cursor.color == 0xffff9e64);
    CHECK(cfg->colors.palette[0] == 0x0015161e);
    CHECK(cfg->colors.palette[1] == 0x00f7768e);
    CHECK(cfg->colors.palette[14] == 0x007dcfff);
    CHECK(cfg->colors.palette[15] == 0x00c0caf5);
    CHECK(cfg->main.osc52 == Osc52Mode::Enabled);
    CHECK(cfg->main.shell == "/bin/zsh");
    REQUIRE(cfg->main.shell_args.size() == 2);
    CHECK(cfg->main.shell_args[0] == "-l");
    CHECK(cfg->main.shell_args[1] == "-i");

    // CLI -o overrides also accept Alacritty key paths.
    size_t cols = 0, rows = 0;
    REQUIRE(cfg->apply_override("general.working_directory=None").has_value());
    CHECK(cfg->main.working_directory.empty());
    REQUIRE(cfg->apply_override("window.opacity=0.5").has_value());
    CHECK(cfg->colors.alpha == 0.5);
    REQUIRE(cfg->apply_override("cursor.style.shape=Underline").has_value());
    CHECK(cfg->cursor.style == gritty::core::CursorShape::Underline);
    REQUIRE(cfg->apply_override("cursor.style.blinking=Never").has_value());
    CHECK(cfg->cursor.blink == false);
    REQUIRE(cfg->apply_override("terminal.osc52=OnlyPaste").has_value());
    CHECK(cfg->main.osc52 == Osc52Mode::PasteOnly);
    REQUIRE(cfg->apply_override("colors.normal.red=#112233").has_value());
    CHECK(cfg->colors.palette[1] == 0x00112233);
    REQUIRE(cfg->apply_override("env.FOO=bar").has_value());
    REQUIRE(cfg->apply_override("env.EDITOR=vim").has_value());
    CHECK(cfg->main.env[0] == "EDITOR=vim");
    CHECK(cfg->main.env[1] == "FOO=bar");
    REQUIRE(cfg->apply_override("window.dimensions.columns=100", &cols, &rows).has_value());
    CHECK(cols == 100);
    CHECK(cfg->window.columns == 100);

    CHECK_FALSE(TomlLoader::load_from_string("[window]\nopacity = 1.5\n").has_value());
    CHECK(TomlLoader::load_from_string("[window]\ndimensions = { columns = 0, lines = 0 }\n").has_value());
    CHECK_FALSE(TomlLoader::load_from_string("[window]\ndimensions = { columns = -1 }\n").has_value());
    CHECK_FALSE(TomlLoader::load_from_string("[window]\ndimensions = { lines = 1001 }\n").has_value());
    CHECK_FALSE(TomlLoader::load_from_string("[cursor]\nstyle = { shape = 'Circle' }\n").has_value());
    CHECK_FALSE(TomlLoader::load_from_string("[cursor]\nstyle = { blinking = 'Sometimes' }\n").has_value());
    CHECK_FALSE(TomlLoader::load_from_string("[terminal]\nosc52 = 'ReadWrite'\n").has_value());
    CHECK_FALSE(TomlLoader::load_from_string("[terminal.shell]\nargs = [123]\n").has_value());
}

TEST_CASE("Window dimensions, position, and startup_mode follow Alacritty defaults and validation") {
    AppConfig defaults;
    CHECK(defaults.window.columns == 0);
    CHECK(defaults.window.lines == 0);
    CHECK_FALSE(defaults.window.position.has_value());
    CHECK(defaults.window.startup_mode == gritty::platform::WindowStartupMode::Windowed);

    auto placed = TomlLoader::load_from_string(R"(
        [window]
        position = { x = 120, y = 80 }
        startup_mode = "Maximized"
    )");
    REQUIRE(placed.has_value());
    REQUIRE((*placed)->window.position.has_value());
    CHECK((*placed)->window.position->x == 120);
    CHECK((*placed)->window.position->y == 80);
    CHECK((*placed)->window.startup_mode == gritty::platform::WindowStartupMode::Maximized);

    auto reset_none = TomlLoader::load_from_string(R"(
        [window]
        position = "None"
        startup_mode = "SimpleFullscreen"
    )");
    REQUIRE(reset_none.has_value());
    CHECK_FALSE((*reset_none)->window.position.has_value());
    CHECK((*reset_none)->window.startup_mode == gritty::platform::WindowStartupMode::SimpleFullscreen);

    REQUIRE(defaults.apply_override("window.position.x=-40").has_value());
    REQUIRE(defaults.apply_override("window.position.y=60").has_value());
    REQUIRE(defaults.window.position.has_value());
    CHECK(defaults.window.position->x == -40);
    CHECK(defaults.window.position->y == 60);
    REQUIRE(defaults.apply_override("window.position=None").has_value());
    CHECK_FALSE(defaults.window.position.has_value());
    REQUIRE(defaults.apply_override("window.startup_mode=Fullscreen").has_value());
    CHECK(defaults.window.startup_mode == gritty::platform::WindowStartupMode::Fullscreen);

    CHECK_FALSE(TomlLoader::load_from_string("[window]\nposition = 'Center'\n").has_value());
    CHECK_FALSE(TomlLoader::load_from_string("[window]\nposition = {}\n").has_value());
    CHECK_FALSE(TomlLoader::load_from_string("[window]\nposition = { x = 40000 }\n").has_value());
    CHECK_FALSE(TomlLoader::load_from_string("[window]\nstartup_mode = 'Minimized'\n").has_value());
    CHECK_FALSE(defaults.apply_override("window.startup_mode=Minimized").has_value());
    CHECK_FALSE(defaults.apply_override("window.position=Center").has_value());
}

TEST_CASE("Window quit confirmation option defaults to needed and validates all modes") {
    AppConfig defaults;
    CHECK(defaults.window.confirm_quit == QuitConfirmation::Needed);

    auto always = TomlLoader::load_from_string("[window]\nconfirm_quit = 'Always'\n");
    REQUIRE(always.has_value());
    CHECK((*always)->window.confirm_quit == QuitConfirmation::Always);

    auto never = TomlLoader::load_from_string("[general]\nconfirm_quit = 'never'\n");
    REQUIRE(never.has_value());
    CHECK((*never)->window.confirm_quit == QuitConfirmation::Never);

    auto needed = TomlLoader::load_from_string("[window]\nconfirm_quit = 'NEEDED'\n");
    REQUIRE(needed.has_value());
    CHECK((*needed)->window.confirm_quit == QuitConfirmation::Needed);

    CHECK_FALSE(TomlLoader::load_from_string("[window]\nconfirm_quit = 'maybe'\n").has_value());
    CHECK_FALSE(TomlLoader::load_from_string("[window]\nconfirm_quit = true\n").has_value());
    CHECK_FALSE(TomlLoader::load_from_string("[general]\nconfirm_quit = 1\n").has_value());

    REQUIRE(defaults.apply_override("window.confirm_quit=always").has_value());
    CHECK(defaults.window.confirm_quit == QuitConfirmation::Always);
    REQUIRE(defaults.apply_override("general.confirm_quit=never").has_value());
    CHECK(defaults.window.confirm_quit == QuitConfirmation::Never);
    CHECK_FALSE(defaults.apply_override("window.confirm_quit=bogus").has_value());
}
