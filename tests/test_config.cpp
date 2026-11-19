#include <doctest/doctest.h>
#include "config/ini_loader.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace gritty::config;

namespace {

std::filesystem::path temp_dir(std::string_view name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("gritty-ini-" + std::string(name) + "-" + std::to_string(stamp));
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return path;
}

void write_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream file(path);
    file << content;
}

} // namespace

TEST_CASE("IniLoader parses basic config and unquoted hex colors") {
    const std::string ini = R"(
        [main]
        term = foot
        title = My Term

        [font]
        family = CustomFont
        size = 15.5
        dpi = 144
        antialias = subpixel

        [cursor]
        style = beam
        blink = true
        color = #ff5500

        [colors]
        foreground = #abcdef
        background = #123456
        regular1 = #ff0000

        [scrollback]
        lines = 5000
    )";

    const auto res = IniLoader::load_from_string(ini);
    REQUIRE(res.has_value());
    const auto& cfg = **res;

    CHECK(cfg.main.term == "foot");
    CHECK(cfg.main.title == "My Term");
    CHECK(cfg.font.family == "CustomFont");
    CHECK(cfg.font.size == 15.5);
    CHECK(cfg.font.dpi == 144);
    CHECK(cfg.font.antialias == gritty::render::AntialiasMode::SubpixelLcd);

    CHECK(cfg.cursor.style == gritty::core::CursorShape::Beam);
    CHECK(cfg.cursor.blink == true);
    CHECK(cfg.cursor.color == 0xFFFF5500);

    CHECK(cfg.colors.foreground == 0xFFABCDEF);
    CHECK(cfg.colors.background == 0xFF123456);
    CHECK(cfg.colors.palette[1] == 0x00FF0000);
    CHECK(cfg.scrollback.lines == 5000);
}

TEST_CASE("IniLoader applies platform sections regardless of declaration order") {
    const std::string ini = R"(
        [macos.font]
        family = MacFont
        size = 14.5

        [linux.font]
        family = LinuxFont

        [font]
        family = DefaultFont
        size = 12.0

        [colors]
        alpha = 0.8
    )";

    const auto mac = IniLoader::load_from_string(ini, "macos");
    REQUIRE(mac.has_value());
    CHECK((*mac)->font.family == "MacFont");
    CHECK((*mac)->font.size == 14.5);

    const auto linux_cfg = IniLoader::load_from_string(ini, "linux");
    REQUIRE(linux_cfg.has_value());
    CHECK((*linux_cfg)->font.family == "LinuxFont");
    CHECK((*linux_cfg)->font.size == 12.0);

    const std::string legacy = R"(
        [platform.macos.colors]
        alpha = 0.25
    )";
    const auto overridden = IniLoader::load_from_string(legacy, "macos");
    REQUIRE(overridden.has_value());
    CHECK((*overridden)->colors.alpha == 0.25);
}

TEST_CASE("Background opacity accepts numeric alpha and composes with AARRGGBB") {
    auto result = IniLoader::load_from_string("[colors]\nbackground = #604020\nalpha = 0.5\n");
    REQUIRE(result.has_value());
    CHECK((*result)->colors.alpha == 0.5);
    CHECK((*result)->colors.background_argb() == 0x80604020);

    result = IniLoader::load_from_string("[colors]\nbackground = #80604020\nalpha = 0.5\n");
    REQUIRE(result.has_value());
    CHECK((*result)->colors.background_argb() == 0x40604020);

    result = IniLoader::load_from_string("[colors]\nalpha = 0\n");
    REQUIRE(result.has_value());
    CHECK((*result)->colors.background_argb() == 0x00111111);

    result = IniLoader::load_from_string("[colors]\nalpha = 1\n");
    REQUIRE(result.has_value());
    CHECK((*result)->colors.background_argb() == 0xff111111);

    result = IniLoader::load_from_string(
        "[colors]\nalpha = 0.8\n[macos.colors]\nalpha = 0.25\n", "macos");
    REQUIRE(result.has_value());
    CHECK((*result)->colors.background_argb() == 0x40111111);
}

TEST_CASE("Background opacity rejects invalid values") {
    for (const auto* value : {"-0.1", "1.1", "nan", "inf", "opaque", "true", "[]"}) {
        CAPTURE(value);
        const auto result = IniLoader::load_from_string(
            std::string("[colors]\nalpha = ") + value + "\n");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message.find("colors.alpha") != std::string::npos);
    }
    CHECK_FALSE(IniLoader::parse_color("#8000000g").has_value());
    CHECK_FALSE(IniLoader::parse_color("#12345g").has_value());
}

TEST_CASE("IniLoader parses hex colors and foot-style values") {
    CHECK(IniLoader::parse_color("#ff0000") == 0xFFFF0000);
    CHECK(IniLoader::parse_color("00ff00") == 0xFF00FF00);
    CHECK(IniLoader::parse_color("0x0000ff") == 0xFF0000FF);
    CHECK(IniLoader::parse_color("#80112233") == 0x80112233);
    CHECK(IniLoader::parse_color("0x80112233") == 0x80112233);
    CHECK(IniLoader::parse_color("invalid") == std::nullopt);

    const std::string ini =
        "; leading semicolon comment\r\n"
        "  # leading hash comment\r\n"
        "[main]\r\n"
        "title = \"hash # inside and quote: \\\"x\\\"\"\r\n"
        "[font]\r\n"
        "family = 'literal # font'\r\n"
        "size = 14\r\n";
    const auto result = IniLoader::load_from_string(ini, "none");
    REQUIRE(result.has_value());
    CHECK((*result)->main.title == "hash # inside and quote: \"x\"");
    CHECK((*result)->font.family == "literal # font");
    CHECK((*result)->font.size == 14.0);
}

TEST_CASE("IniLoader supports relative include files and rejects cycles") {
    const auto dir = temp_dir("include");
    const auto root = dir / "gritty.ini";
    const auto theme = dir / "theme.ini";
    const auto loop = dir / "loop.ini";
    write_file(theme, "[colors]\nforeground = #112233\nregular0 = #010203\n");
    write_file(loop, "include = gritty.ini\n");
    write_file(root, "[main]\ntitle = base\ninclude = theme.ini\n");

    const auto loaded = IniLoader::load_from_file(root.string(), "none");
    REQUIRE(loaded.has_value());
    CHECK((*loaded)->main.title == "base");
    CHECK((*loaded)->colors.foreground == 0xFF112233);
    CHECK((*loaded)->colors.palette[0] == 0x00010203);

    write_file(root, "include = loop.ini\n");
    const auto cyclic = IniLoader::load_from_file(root.string(), "none");
    REQUIRE_FALSE(cyclic.has_value());
    CHECK(cyclic.error().message.find("include") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("IniLoader reports syntax errors with line and column") {
    const auto unterminated = IniLoader::load_from_string("[main]\ntitle = \"oops\n");
    REQUIRE_FALSE(unterminated.has_value());
    CHECK(unterminated.error().message.find("unterminated") != std::string::npos);
    CHECK(unterminated.error().line == 2);
    CHECK(unterminated.error().column == 9);

    const auto bad_header = IniLoader::load_from_string("[main\ntitle = x\n");
    REQUIRE_FALSE(bad_header.has_value());
    CHECK(bad_header.error().line == 1);
    CHECK(bad_header.error().column == 1);

    const auto missing_equals = IniLoader::load_from_string("[main]\ntitle oops\n");
    REQUIRE_FALSE(missing_equals.has_value());
    CHECK(missing_equals.error().line == 2);
    CHECK(missing_equals.error().column == 11);

    const auto non_matching_platform = IniLoader::load_from_string(
        "[linux.font]\nfamily bad\n", "macos");
    REQUIRE_FALSE(non_matching_platform.has_value());
    CHECK(non_matching_platform.error().line == 2);
}
