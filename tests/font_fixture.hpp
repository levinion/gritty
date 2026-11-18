#pragma once
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

inline std::string test_font_path() {
    if (const char* path = std::getenv("GRITTY_TEST_FONT")) {
        if (!std::filesystem::is_regular_file(path))
            throw std::runtime_error("GRITTY_TEST_FONT is not a font file: " + std::string(path));
        return path;
    }
    for (const auto* path : {"/System/Library/Fonts/Monaco.ttf", "/System/Library/Fonts/Menlo.ttc",
                            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"}) {
        if (std::filesystem::is_regular_file(path)) return path;
    }
    return {};
}
