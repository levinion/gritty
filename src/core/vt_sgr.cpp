#include "core/vt_parser.hpp"

namespace gritty::core {

void VtParser::handle_sgr() {
    if (param_count_ == 0) {
        engine_.sgr_reset();
        return;
    }

    for (int i = 0; i < param_count_; ++i) {
        const auto& p = params_[i];
        int val = p.value < 0 ? 0 : p.value;

        switch (val) {
            case 0: engine_.sgr_reset(); break;
            case 1: engine_.sgr_bold(true); break;
            case 2: engine_.sgr_dim(true); break;
            case 3: engine_.sgr_italic(true); break;
            case 4:
                if (p.subparam_count > 0 && p.subparams[0] >= 0) {
                    engine_.sgr_underline(static_cast<UnderlineStyle>(p.subparams[0]));
                } else {
                    engine_.sgr_underline(UnderlineStyle::Single);
                }
                break;
            case 5: engine_.sgr_blink(true); break;
            case 7: engine_.sgr_inverse(true); break;
            case 8: engine_.sgr_invisible(true); break;
            case 9: engine_.sgr_strikethrough(true); break;
            case 21: engine_.sgr_underline(UnderlineStyle::Double); break;
            case 22:
                engine_.sgr_bold(false);
                engine_.sgr_dim(false);
                break;
            case 23: engine_.sgr_italic(false); break;
            case 24: engine_.sgr_underline(UnderlineStyle::None); break;
            case 25: engine_.sgr_blink(false); break;
            case 27: engine_.sgr_inverse(false); break;
            case 28: engine_.sgr_invisible(false); break;
            case 29: engine_.sgr_strikethrough(false); break;

            // Standard ANSI colors
            case 30: case 31: case 32: case 33:
            case 34: case 35: case 36: case 37:
                engine_.sgr_fg_color(Color::make_indexed(static_cast<uint8_t>(val - 30)));
                break;
            case 39:
                engine_.sgr_fg_color(Color::make_default());
                break;
            case 40: case 41: case 42: case 43:
            case 44: case 45: case 46: case 47:
                engine_.sgr_bg_color(Color::make_indexed(static_cast<uint8_t>(val - 40)));
                break;
            case 49:
                engine_.sgr_bg_color(Color::make_default());
                break;
            case 90: case 91: case 92: case 93:
            case 94: case 95: case 96: case 97:
                engine_.sgr_fg_color(Color::make_indexed(static_cast<uint8_t>(val - 90 + 8)));
                break;
            case 100: case 101: case 102: case 103:
            case 104: case 105: case 106: case 107:
                engine_.sgr_bg_color(Color::make_indexed(static_cast<uint8_t>(val - 100 + 8)));
                break;

            // Extended colors (38, 48, 58)
            case 38:
            case 48:
            case 58: {
                auto apply_color = [&](Color c) {
                    if (val == 38) engine_.sgr_fg_color(c);
                    else if (val == 48) engine_.sgr_bg_color(c);
                    else engine_.sgr_underline_color(c);
                };

                // Check colon subparameters first (e.g. 38:2::r:g:b or 38:2:cs:r:g:b or 38:2:r:g:b or 38:5:idx)
                if (p.subparam_count >= 1) {
                    int type = p.subparams[0];
                    if (type == 5 && p.subparam_count >= 2 && p.subparams[1] >= 0) {
                        apply_color(Color::make_indexed(static_cast<uint8_t>(p.subparams[1])));
                    } else if (type == 2) {
                        if (p.subparam_count >= 5) {
                            // Format: 38:2::r:g:b or 38:2:cs:r:g:b (58:2:... shares this layout)
                            int r = p.subparams[2];
                            int g = p.subparams[3];
                            int b = p.subparams[4];
                            apply_color(Color::make_rgb(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)));
                        } else if (p.subparam_count >= 4) {
                            // Format: 38:2:r:g:b
                            int r = p.subparams[1];
                            int g = p.subparams[2];
                            int b = p.subparams[3];
                            apply_color(Color::make_rgb(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)));
                        }
                    }
                } else if (i + 1 < param_count_) {
                    int type = params_[i + 1].value;
                    if (type == 5 && i + 2 < param_count_) { // 38;5;n
                        apply_color(Color::make_indexed(static_cast<uint8_t>(params_[i + 2].value)));
                        i += 2;
                    } else if (type == 2 && i + 4 < param_count_) { // 38;2;r;g;b
                        uint8_t r = static_cast<uint8_t>(params_[i + 2].value);
                        uint8_t g = static_cast<uint8_t>(params_[i + 3].value);
                        uint8_t b = static_cast<uint8_t>(params_[i + 4].value);
                        apply_color(Color::make_rgb(r, g, b));
                        i += 4;
                    }
                }
                break;
            }
            case 59:
                engine_.sgr_underline_color(Color::make_default());
                break;
            default:
                break;
        }
    }
}

} // namespace gritty::core
