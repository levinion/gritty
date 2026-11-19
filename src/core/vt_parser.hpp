#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <functional>
#include "core/terminal_engine.hpp"

namespace gritty::core {

enum class ParserState : uint8_t {
    Ground,
    Escape,
    EscapeIntermediate,
    CsiEntry,
    CsiParam,
    CsiIntermediate,
    CsiIgnore,
    OscString,
    DcsEntry,
    DcsParam,
    DcsIntermediate,
    DcsString,
    DcsIgnore,
    ApcString,
    IgnoredString,
};

struct Parameter {
    int value{-1}; // -1 means default / omitted
    int subparams[8]{-1, -1, -1, -1, -1, -1, -1, -1};
    int subparam_count{0};
};

class VtParser final {
public:
    explicit VtParser(TerminalEngine& engine);
    ~VtParser() = default;

    VtParser(const VtParser&) = delete;
    VtParser& operator=(const VtParser&) = delete;

    // Parse a stream of bytes
    void feed(std::string_view data);
    void feed(const uint8_t* data, size_t length);

    // Callbacks for DCS / APC / OSC handlers (e.g. Kitty graphics, Sixel)
    using ApcHandler = std::function<void(std::string_view)>;
    using DcsHandler = std::function<void(std::string_view)>;
    using OscHandler = std::function<void(int, std::string_view)>;
    using ResponseCallback = std::function<void(std::string_view)>;
    // OSC 52 clipboard writes; reads are gated by an explicit policy.
    using ClipboardHandler = std::function<void(std::string_view)>;
    using ClipboardReadHandler = std::function<std::string()>;
    using NotificationHandler = std::function<void(std::string_view, std::string_view)>;
    using CursorColorHandler = std::function<void(uint32_t)>;
    using MouseCursorShapeHandler = std::function<void(platform::MouseCursorShape)>;

    void set_apc_handler(ApcHandler handler) { apc_handler_ = std::move(handler); }
    void set_dcs_handler(DcsHandler handler) { dcs_handler_ = std::move(handler); }
    void set_osc_handler(OscHandler handler) { osc_handler_ = std::move(handler); }
    void set_response_callback(ResponseCallback cb) { response_cb_ = std::move(cb); }
    void set_clipboard_handler(ClipboardHandler handler) { clipboard_handler_ = std::move(handler); }
    void set_clipboard_read_handler(ClipboardReadHandler handler) { clipboard_read_handler_ = std::move(handler); }
    void set_osc52_read_enabled(bool enabled) noexcept { osc52_read_enabled_ = enabled; }
    void set_notification_handler(NotificationHandler handler) { notification_handler_ = std::move(handler); }
    void set_cursor_color_handler(CursorColorHandler handler) { cursor_color_handler_ = std::move(handler); }
    void set_mouse_cursor_shape_handler(MouseCursorShapeHandler handler) { mouse_cursor_shape_handler_ = std::move(handler); }
    void set_sync_handler(std::function<void(bool)> handler) { sync_handler_ = std::move(handler); }
    void set_default_color_handler(std::function<void(uint32_t, uint32_t)> handler) { default_color_handler_ = std::move(handler); }

    void reset() noexcept;
    void set_default_colors(uint32_t foreground, uint32_t background,
                            uint32_t cursor_color = 0xffffffff) noexcept {
        configured_fg_ = default_fg_ = foreground;
        configured_bg_ = default_bg_ = background;
        configured_cursor_ = cursor_color_ = cursor_color;
    }

private:
    void process_byte(uint8_t byte);
    void handle_c0(uint8_t byte);
    void handle_escape(uint8_t byte);
    void clear_sequence() noexcept;
    void finish_string();
    void append_string(uint8_t byte);
    void print_codepoint(char32_t cp);
    void handle_dcs();
    void handle_query(uint8_t final_char);
    int get_param(int idx, int fallback) const noexcept;
    int dec_mode_status(int mode) const noexcept;
    void handle_csi(uint8_t final_char);
    void handle_sgr();
    void handle_osc();
    void update_default_color(int command, uint32_t rgb);
    void update_cursor_color(uint32_t argb);
    void handle_decset(bool set);

    void parse_csi_param_byte(uint8_t byte);
    void ensure_csi_param(int idx) noexcept;
    void send_response(std::string_view resp);

    // Shared by the state machine and the inline CSI fast path in feed().
    void set_csi_prefix(uint8_t byte) noexcept {
        is_private_ = byte == '?';
        has_greater_ = byte == '>';
        has_less_ = byte == '<';
        has_equal_ = byte == '=';
    }

    inline void append_csi_digit(uint8_t byte) noexcept {
        ensure_csi_param(current_param_idx_);
        int& value = current_subparam_idx_ >= 0
            ? params_[current_param_idx_].subparams[current_subparam_idx_]
            : params_[current_param_idx_].value;
        // Saturate before multiplication to avoid signed overflow.
        const int next = (value > 0 ? value : 0) * 10 + (byte - '0');
        value = next > 65535 ? 65535 : next;
        param_count_ = current_param_idx_ + 1;
    }

    // Returns false on parameter/subparameter overflow so callers can either
    // enter CsiIgnore (state machine) or fall back to process_byte() (fast path).
    inline bool advance_csi_param(uint8_t byte) noexcept {
        if (byte == ';') {
            if (++current_param_idx_ >= 32) return false;
            ensure_csi_param(current_param_idx_);
            current_subparam_idx_ = -1;
            param_count_ = current_param_idx_ + 1;
            return true;
        }
        if (byte == ':') {
            ensure_csi_param(current_param_idx_);
            if (++current_subparam_idx_ >= 8) return false;
            params_[current_param_idx_].subparam_count = current_subparam_idx_ + 1;
            param_count_ = current_param_idx_ + 1;
            return true;
        }
        return false;
    }

    inline bool consume_csi_param_byte(uint8_t byte) noexcept {
        if (byte >= '0' && byte <= '9') {
            append_csi_digit(byte);
            return true;
        }
        return advance_csi_param(byte);
    }

    TerminalEngine& engine_;
    ParserState state_{ParserState::Ground};

    // CSI parsing state
    bool is_private_{false}; // '?' prefix
    bool has_greater_{false}; // '>' prefix
    bool has_less_{false}; // '<' prefix
    bool has_equal_{false}; // '=' prefix
    uint8_t intermediate_char_{0};
    Parameter params_[32];
    int param_count_{0};
    int current_param_idx_{0};
    int current_subparam_idx_{-1};
    // Number of leading params_ slots initialized for the current CSI. Avoids
    // clearing all 32 slots (1 KiB) for the common short/no-param sequence.
    int initialized_params_{0};

    // OSC / DCS / APC string buffer
    enum class StringType : uint8_t { None, Osc, Dcs, Apc };
    StringType string_type_{StringType::None};
    std::string string_buffer_;
    bool osc_reply_with_st_{false}; // Replies mirror the request terminator.

    // Callbacks
    ApcHandler apc_handler_;
    DcsHandler dcs_handler_;
    OscHandler osc_handler_;
    ResponseCallback response_cb_;
    ClipboardHandler clipboard_handler_;
    ClipboardReadHandler clipboard_read_handler_;
    bool osc52_read_enabled_{false};
    NotificationHandler notification_handler_;
    CursorColorHandler cursor_color_handler_;
    MouseCursorShapeHandler mouse_cursor_shape_handler_;
    std::function<void(bool)> sync_handler_;
    std::function<void(uint32_t, uint32_t)> default_color_handler_;

    // UTF-8 state for multibyte characters in Ground state
    uint32_t utf8_state_{0};
    uint32_t utf8_codepoint_{0};
    uint32_t utf8_minimum_{0};
    bool string_overflow_{false};
    // tmux doubles ESC inside a DCS passthrough payload. This records the
    // first ESC until the next byte disambiguates ESC ESC from ESC ST.
    bool dcs_tmux_escape_pending_{false};
    int passthrough_depth_{0};
    uint32_t default_fg_{0xffdddddd};
    uint32_t default_bg_{0xff111111};
    uint32_t cursor_color_{0xffffffff};
    uint32_t configured_fg_{0xffdddddd};
    uint32_t configured_bg_{0xff111111};
    uint32_t configured_cursor_{0xffffffff};
};

} // namespace gritty::core
