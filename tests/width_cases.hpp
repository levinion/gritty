#pragma once
#include <array>

// tmux's default UTF8PROC policy: East Asian ambiguous and private-use glyphs
// are narrow; ordinary CJK and emoji presentation scalars are wide. These are
// codepoint vectors, not a claim that every VS/ZWJ grapheme is implemented.
struct WidthCase {
    const char* name;
    char32_t codepoint;
    int width;
};
inline constexpr std::array width_cases{
    WidthCase{"ASCII", U'A', 1},
    WidthCase{"CJK", U'界', 2},
    WidthCase{"fullwidth", U'Ａ', 2},
    WidthCase{"supplementary CJK", U'𠀀', 2},
    WidthCase{"ambiguous narrow", U'Ω', 1},
    WidthCase{"combining acute", U'\u0301', 0},
    WidthCase{"emoji face", U'😀', 2},
    WidthCase{"emoji rocket", U'🚀', 2},
    WidthCase{"emoji test tube", U'🧪', 2},
    WidthCase{"Powerline PUA", U'\ue0b0', 1},
    WidthCase{"Nerd Font supplementary PUA", U'\U000f0001', 1},
};
