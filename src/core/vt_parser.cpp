#include "core/vt_parser.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace gritty::core {
namespace {

// SWAR test for eight bytes in [0x20, 0x7e]. A byte is printable ASCII when
// it has no high bit, is >= 0x20 (no borrow from 0x20) and < 0x7f (borrow from
// 0x7f). Returns the length of the leading printable run.
size_t printable_ascii_run(const uint8_t* data, size_t length) {
    constexpr uint64_t msb = 0x8080808080808080ull;
    constexpr uint64_t low = 0x2020202020202020ull;
    constexpr uint64_t high = 0x7f7f7f7f7f7f7full;
    size_t i = 0;
#if defined(__aarch64__) || defined(_M_ARM64)
    const uint8x16_t floor = vdupq_n_u8(0x20);
    const uint8x16_t bad_limit = vdupq_n_u8(0x5f);
    for (; i + 32 <= length; i += 32) {
        const uint8x16_t v0 = vld1q_u8(data + i);
        const uint8x16_t v1 = vld1q_u8(data + i + 16);
        const uint8x16_t d0 = vsubq_u8(v0, floor);
        const uint8x16_t d1 = vsubq_u8(v1, floor);
        if (vmaxvq_u8(vorrq_u8(d0, d1)) < 0x5f) continue;

        const uint8x16_t bad0 = vcgeq_u8(d0, bad_limit);
        const uint8x16_t bad1 = vcgeq_u8(d1, bad_limit);
        const uint8x8_t packed0 = vshrn_n_u16(vreinterpretq_u16_u8(bad0), 4);
        const uint8x8_t packed1 = vshrn_n_u16(vreinterpretq_u16_u8(bad1), 4);
        uint64_t bits0 = 0, bits1 = 0;
        std::memcpy(&bits0, &packed0, sizeof(bits0));
        std::memcpy(&bits1, &packed1, sizeof(bits1));
        if (bits0 != 0) return i + (static_cast<size_t>(__builtin_ctzll(bits0)) >> 2);
        return i + 16 + (static_cast<size_t>(__builtin_ctzll(bits1)) >> 2);
    }
#endif
    while (i + 8 <= length) {
        uint64_t word = 0;
        std::memcpy(&word, data + i, sizeof(word));
        const uint64_t below = (word - low) & msb;
        const uint64_t above_or_equal = msb ^ ((word - high) & msb);
        if ((below | above_or_equal | (word & msb)) != 0) break;
        i += 8;
    }
    while (i < length && data[i] >= 0x20 && data[i] <= 0x7e) ++i;
    return i;
}

// Codepoints eligible for the bulk wide-character path. These are the common
// three-byte CJK ranges; everything else keeps the exact print_char() path.
bool common_wide_run_char(char32_t cp) noexcept {
    if (cp == 0x3099 || cp == 0x309a) return false;
    return (cp >= 0x4e00 && cp <= 0x9fff) ||
           (cp >= 0x3040 && cp <= 0x30ff) ||
           (cp >= 0xac00 && cp <= 0xd7af);
}

struct WideRun {
    size_t bytes{0};
    size_t count{0};
};

WideRun common_wide_run(const uint8_t* data, size_t length, char32_t* out, size_t capacity) noexcept {
    WideRun result;
    while (result.count < capacity && length - result.bytes >= 3) {
        const uint8_t* p = data + result.bytes;
        if (p[0] < 0xe0 || p[0] > 0xef ||
            (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80) break;
        const char32_t cp = ((static_cast<char32_t>(p[0] & 0x0f) << 12) |
                             (static_cast<char32_t>(p[1] & 0x3f) << 6) |
                             static_cast<char32_t>(p[2] & 0x3f));
        if (!common_wide_run_char(cp)) break;
        out[result.count++] = cp;
        result.bytes += 3;
    }
    return result;
}

// Returns the length of a payload run containing no terminal control byte.
size_t string_payload_run(const uint8_t* data, size_t length, bool osc) {
    size_t i = 0;
#if defined(__aarch64__) || defined(_M_ARM64)
    const uint8x16_t esc = vdupq_n_u8(0x1b);
    const uint8x16_t can = vdupq_n_u8(0x18);
    const uint8x16_t sub = vdupq_n_u8(0x1a);
    const uint8x16_t bel = vdupq_n_u8(0x07);
    const uint8x16_t st = vdupq_n_u8(0x9c);
    for (; i + 16 <= length; i += 16) {
        const uint8x16_t v = vld1q_u8(data + i);
        uint8x16_t bad = vorrq_u8(vceqq_u8(v, esc), vorrq_u8(vceqq_u8(v, can), vceqq_u8(v, sub)));
        bad = vorrq_u8(bad, vceqq_u8(v, st)); // Single-byte ST.
        if (osc) bad = vorrq_u8(bad, vceqq_u8(v, bel));
        if (vmaxvq_u8(bad) == 0) continue;
        const uint8x8_t packed = vshrn_n_u16(vreinterpretq_u16_u8(bad), 4);
        uint64_t bits = 0;
        std::memcpy(&bits, &packed, sizeof(bits));
        return i + (static_cast<size_t>(__builtin_ctzll(bits)) >> 2);
    }
#endif
    while (i < length) {
        const uint8_t byte = data[i];
        if (byte == 0x1b || byte == 0x18 || byte == 0x1a || byte == 0x9c ||
            (osc && byte == 0x07)) break;
        ++i;
    }
    return i;
}

} // namespace

// State transitions follow foot vt.c (DEC's ANSI parser), with bounded strings
// for this callback-based implementation. See upstream foot vt.c and tmux source tests.
VtParser::VtParser(TerminalEngine& engine) : engine_(engine) {
    string_buffer_.reserve(65536);
    reset();
}

void VtParser::clear_sequence() noexcept {
    is_private_ = has_greater_ = has_less_ = has_equal_ = false;
    intermediate_char_ = 0;
    param_count_ = current_param_idx_ = 0;
    current_subparam_idx_ = -1;
    initialized_params_ = 0;
}

void VtParser::ensure_csi_param(int idx) noexcept {
    // Slots are initialized monotonically, so omitted leading params also
    // receive their default value instead of retaining the previous CSI.
    for (int i = initialized_params_; i <= idx; ++i) params_[i] = Parameter{};
    initialized_params_ = std::max(initialized_params_, idx + 1);
}

void VtParser::reset() noexcept {
    state_ = ParserState::Ground;
    clear_sequence();
    string_type_ = StringType::None;
    string_buffer_.clear();
    string_overflow_ = false;
    dcs_tmux_escape_pending_ = false;
    utf8_state_ = utf8_codepoint_ = utf8_minimum_ = 0;
}

void VtParser::feed(std::string_view data) {
    feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

void VtParser::feed(const uint8_t* data, size_t length) {
    size_t i = 0;
    while (i < length) {
        const bool in_string = state_ == ParserState::OscString ||
            state_ == ParserState::DcsString || state_ == ParserState::ApcString ||
            state_ == ParserState::IgnoredString;
        if (in_string && !dcs_tmux_escape_pending_ && utf8_state_ == 0) {
            const size_t start = i;
            i += string_payload_run(data + i, length - i, state_ == ParserState::OscString);
            if (i > start && state_ != ParserState::IgnoredString) {
                const size_t limit = string_type_ == StringType::Osc
                    ? 65536 : 32u * 1024u * 1024u;
                const size_t room = string_buffer_.size() < limit ? limit - string_buffer_.size() : 0;
                const size_t count = std::min(i - start, room);
                if (count) string_buffer_.append(
                    reinterpret_cast<const char*>(data + start), count);
                if (count < i - start) string_overflow_ = true;
            }
            if (i < length) process_byte(data[i++]);
            continue;
        }

        // Complete 7-bit CSI sequences are parsed in this loop instead of
        // re-entering process_byte() once per parameter byte. Any incomplete
        // or unusual sequence is left untouched for the normal state machine.
        if (state_ == ParserState::Ground && utf8_state_ == 0 &&
            data[i] == 0x1b && i + 2 < length && data[i + 1] == '[') {
            clear_sequence();
            size_t j = i + 2;
            const uint8_t lead = data[j];
            if (lead >= '<' && lead <= '?') {
                set_csi_prefix(lead);
                ++j;
            }

            bool complete = false;
            while (j < length) {
                const uint8_t byte = data[j];
                if (byte >= '0' && byte <= '9') {
                    append_csi_digit(byte);
                    ++j;
                } else if (byte == ';' || byte == ':') {
                    if (!advance_csi_param(byte)) break;
                    ++j;
                } else if (byte >= 0x40 && byte <= 0x7e) {
                    handle_csi(byte);
                    state_ = ParserState::Ground;
                    i = j + 1;
                    complete = true;
                    break;
                } else {
                    // C0, DEL, ESC, CAN/SUB, intermediates and high bytes all
                    // need the exact normal state-machine behaviour.
                    break;
                }
            }
            if (complete) continue;
        }

        if (state_ == ParserState::Ground && utf8_state_ == 0 &&
            length - i >= 12) { // Four three-byte codepoints.
            std::array<char32_t, 32> wide{};
            const auto run = common_wide_run(data + i, length - i, wide.data(), wide.size());
            if (run.count >= 4) {
                engine_.print_wide_run(std::span<const char32_t>(wide.data(), run.count));
                i += run.bytes;
                continue;
            }
        }

        if (state_ == ParserState::Ground && utf8_state_ == 0 && data[i] >= 0x80) {
            const uint8_t lead = data[i];
            int expected = 0;
            char32_t cp = 0;
            uint32_t minimum = 0;
            if (lead >= 0xc2 && lead <= 0xdf) {
                expected = 1; cp = lead & 0x1f; minimum = 0x80;
            } else if (lead >= 0xe0 && lead <= 0xef) {
                expected = 2; cp = lead & 0x0f; minimum = 0x800;
            } else if (lead >= 0xf0 && lead <= 0xf4) {
                expected = 3; cp = lead & 0x07; minimum = 0x10000;
            }
            if (expected != 0 && length - i >= 4 && static_cast<size_t>(expected + 1) <= length - i) {
                bool valid = true;
                for (int k = 1; k <= expected; ++k) {
                    if ((data[i + k] & 0xc0) != 0x80) { valid = false; break; }
                    cp = (cp << 6) | (data[i + k] & 0x3f);
                }
                if (valid && cp >= minimum && cp <= 0x10ffff &&
                    !(cp >= 0xd800 && cp <= 0xdfff)) {
                    engine_.print_char(cp);
                    i += static_cast<size_t>(expected + 1);
                    continue;
                }
                if (valid) {
                    engine_.print_char(U'\ufffd');
                    i += static_cast<size_t>(expected + 1);
                    continue;
                }
            }
        }
        if (state_ == ParserState::Ground && utf8_state_ == 0 &&
            data[i] >= 0x20 && data[i] <= 0x7e) {
            const size_t run = printable_ascii_run(data + i, length - i);
            if (run >= 4) {
                engine_.print_ascii_run(std::string_view(
                    reinterpret_cast<const char*>(data + i), run));
            } else {
                // Short runs avoid a second process_byte() pass through CAN/SUB,
                // ESC, DEL, C0 and SWAR checks.
                for (size_t k = 0; k < run; ++k)
                    engine_.print_char(data[i + k]);
            }
            i += run;
            continue;
        }
        process_byte(data[i++]);
    }
}

void VtParser::send_response(std::string_view response) {
    if (response_cb_) response_cb_(response);
}

int VtParser::get_param(int idx, int fallback) const noexcept {
    // foot vt_param_get: zero, just like omission, selects the default.
    return idx < param_count_ && params_[idx].value > 0 ? params_[idx].value : fallback;
}

void VtParser::append_string(uint8_t byte) {
    // OSC stays bounded: it is used for titles/hyperlinks/clipboard and has no
    // large-payload use case. APC carries Kitty image transfers (and tmux DCS
    // passthrough), so allow the same 32 MiB budget as the image cache.
    const size_t limit = string_type_ == StringType::Osc
        ? 65536 : 32u * 1024u * 1024u;
    if (string_buffer_.size() < limit) string_buffer_.push_back(static_cast<char>(byte));
    else string_overflow_ = true;
}

void VtParser::finish_string() {
    if (!string_overflow_) {
        if (state_ == ParserState::OscString) handle_osc();
        else if (state_ == ParserState::DcsString) handle_dcs();
        else if (state_ == ParserState::ApcString && apc_handler_) apc_handler_(string_buffer_);
    }
    string_buffer_.clear();
    string_type_ = StringType::None;
    string_overflow_ = false;
    dcs_tmux_escape_pending_ = false;
    // A 32 MiB image must not pin its heap capacity for the rest of the session.
    if (string_buffer_.capacity() > 256 * 1024) {
        std::string empty;
        string_buffer_.swap(empty); // Portable shrink-to-fit across libstdc++/libc++.
    }
}

void VtParser::print_codepoint(char32_t cp) {
    engine_.print_char(cp);
}

void VtParser::process_byte(uint8_t byte) {
    // CAN/SUB cancel sequences in *every* state, including strings.
    if (byte == 0x18 || byte == 0x1a) {
        reset();
        return;
    }
    if (dcs_tmux_escape_pending_ && state_ == ParserState::DcsString) {
        dcs_tmux_escape_pending_ = false;
        if (byte == 0x1b) {
            // ESC ESC inside tmux passthrough represents one literal ESC.
            append_string(0x1b);
        } else if (byte == '\\') {
            // ESC \ is the outer ST; consume it without adding it to payload.
            finish_string();
            state_ = ParserState::Ground;
        } else {
            finish_string();
            clear_sequence();
            state_ = ParserState::Escape;
            process_byte(byte);
        }
        return;
    }
    if (byte == 0x1b) {
        if (state_ == ParserState::DcsString && string_buffer_.starts_with("tmux;")) {
            dcs_tmux_escape_pending_ = true;
            return;
        }
        // ESC \ is the two-byte ST; every OSC terminated by ESC replies with ST.
        if (state_ == ParserState::OscString) osc_reply_with_st_ = true;
        finish_string();
        clear_sequence();
        utf8_state_ = 0;
        state_ = ParserState::Escape;
        return;
    }
    if (state_ == ParserState::OscString || state_ == ParserState::DcsString ||
        state_ == ParserState::ApcString || state_ == ParserState::IgnoredString) {
        // BEL terminates OSC only. DCS/APC are terminated by ST, not BEL.
        if (byte == 0x07 && state_ == ParserState::OscString) {
            osc_reply_with_st_ = false;
            finish_string();
            state_ = ParserState::Ground;
        } else if (byte == 0x9c && state_ != ParserState::IgnoredString) {
            if (state_ == ParserState::OscString) osc_reply_with_st_ = true;
            finish_string();
            state_ = ParserState::Ground;
        } else if (state_ != ParserState::IgnoredString) append_string(byte);
        return;
    }
    if (byte == 0x7f) return; // DEL is ignored, including within ESC and CSI.
    if (byte < 0x20) {
        utf8_state_ = 0;
        handle_c0(byte);
        return;
    }

    switch (state_) {
        case ParserState::Ground: {
            if (utf8_state_ && (byte & 0xc0) == 0x80) {
                utf8_codepoint_ = (utf8_codepoint_ << 6) | (byte & 0x3f);
                if (--utf8_state_ == 0) {
                    const auto cp = utf8_codepoint_;
                    print_codepoint(cp >= utf8_minimum_ && cp <= 0x10ffff &&
                        !(cp >= 0xd800 && cp <= 0xdfff) ? cp : U'\ufffd');
                }
                break;
            }
            if (utf8_state_) {
                utf8_state_ = 0;
                print_codepoint(U'\ufffd');
            }
            if (byte < 0x80) print_codepoint(byte);
            else if (byte >= 0xc2 && byte <= 0xdf) {
                utf8_codepoint_ = byte & 0x1f;
                utf8_state_ = 1;
                utf8_minimum_ = 0x80;
            } else if (byte >= 0xe0 && byte <= 0xef) {
                utf8_codepoint_ = byte & 0x0f;
                utf8_state_ = 2;
                utf8_minimum_ = 0x800;
            } else if (byte >= 0xf0 && byte <= 0xf4) {
                utf8_codepoint_ = byte & 7;
                utf8_state_ = 3;
                utf8_minimum_ = 0x10000;
            }
            break;
        }
        case ParserState::Escape:
        case ParserState::EscapeIntermediate:
            if (byte >= 0x20 && byte <= 0x2f) {
                // Only single-intermediate ESC sequences are currently supported.
                if (intermediate_char_ == 0) intermediate_char_ = byte;
                else intermediate_char_ = 0xff;
                state_ = ParserState::EscapeIntermediate;
            } else if (byte >= 0x30 && byte <= 0x7e) handle_escape(byte);
            else state_ = ParserState::Ground;
            break;
        case ParserState::CsiEntry:
        case ParserState::CsiParam:
        case ParserState::CsiIntermediate:
            parse_csi_param_byte(byte);
            break;
        case ParserState::CsiIgnore:
            if (byte >= 0x40 && byte <= 0x7e) state_ = ParserState::Ground;
            break;
        case ParserState::DcsEntry:
        case ParserState::DcsParam:
        case ParserState::DcsIntermediate:
            if (byte >= 0x40 && byte <= 0x7e) {
                append_string(byte);
                state_ = ParserState::DcsString;
            } else if (byte >= 0x20 && byte <= 0x3f) {
                append_string(byte);
            } else state_ = ParserState::DcsIgnore;
            break;
        default:
            break;
    }
}

void VtParser::handle_c0(uint8_t byte) {
    switch (byte) {
        case 0x08: engine_.move_cursor_backward(1); break;
        case 0x09: engine_.tab_forward(1); break;
        case 0x0a: case 0x0b: case 0x0c: engine_.line_feed(); break;
        case 0x0d: engine_.carriage_return(); break;
        case 0x0e: engine_.select_charset(1); break;
        case 0x0f: engine_.select_charset(0); break;
        default: break;
    }
}

void VtParser::handle_escape(uint8_t byte) {
    state_ = ParserState::Ground;
    if (intermediate_char_) {
        if (intermediate_char_ >= '(' && intermediate_char_ <= '+' && (byte == '0' || byte == 'B'))
            engine_.designate_charset(intermediate_char_ - '(', byte == '0');
        return;
    }
    switch (byte) {
        case '[': clear_sequence(); state_ = ParserState::CsiEntry; break;
        case ']':
            osc_reply_with_st_ = false;
            state_ = ParserState::OscString;
            string_type_ = StringType::Osc;
            break;
        case 'P': state_ = ParserState::DcsEntry; string_type_ = StringType::Dcs; break;
        case '_': state_ = ParserState::ApcString; string_type_ = StringType::Apc; break;
        case 'X': case '^': state_ = ParserState::IgnoredString; break;
        case '7': engine_.save_cursor(); break;
        case '8': engine_.restore_cursor(); break;
        case 'M': engine_.reverse_index(); break;
        case 'D': engine_.line_feed(); break;
        case 'E': engine_.carriage_return(); engine_.line_feed(); break;
        case 'H': engine_.set_tab_stop(); break;
        case 'c':
            engine_.reset();
            update_default_color(10, configured_fg_);
            update_default_color(11, configured_bg_);
            update_cursor_color(configured_cursor_);
            if (mouse_cursor_shape_handler_)
                mouse_cursor_shape_handler_(engine_.mouse_cursor_shape());
            if (sync_handler_) sync_handler_(false);
            break;
        default: break;
    }
}

void VtParser::parse_csi_param_byte(uint8_t byte) {
    if (state_ == ParserState::CsiEntry && byte >= '<' && byte <= '?') {
        set_csi_prefix(byte);
        state_ = ParserState::CsiParam;
        return;
    }
    if (byte >= 0x40 && byte <= 0x7e) {
        handle_csi(byte);
        state_ = ParserState::Ground;
        return;
    }
    if (byte >= 0x20 && byte <= 0x2f) {
        if (intermediate_char_) { state_ = ParserState::CsiIgnore; return; }
        intermediate_char_ = byte;
        state_ = ParserState::CsiIntermediate;
        return;
    }
    if (state_ == ParserState::CsiIntermediate) { state_ = ParserState::CsiIgnore; return; }
    state_ = ParserState::CsiParam;
    if (!consume_csi_param_byte(byte)) state_ = ParserState::CsiIgnore;
}

} // namespace gritty::core
