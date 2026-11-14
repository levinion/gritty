#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
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

    void set_apc_handler(ApcHandler handler) { apc_handler_ = std::move(handler); }
    void set_dcs_handler(DcsHandler handler) { dcs_handler_ = std::move(handler); }
    void set_osc_handler(OscHandler handler) { osc_handler_ = std::move(handler); }
    void set_response_callback(ResponseCallback cb) { response_cb_ = std::move(cb); }
    void set_sync_handler(std::function<void(bool)> handler) { sync_handler_ = std::move(handler); }

    void reset() noexcept;
    void set_default_colors(uint32_t foreground, uint32_t background) noexcept {
        default_fg_ = foreground;
        default_bg_ = background;
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
    void handle_decset(bool set);

    void parse_csi_param_byte(uint8_t byte);
    void send_response(std::string_view resp);

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

    // OSC / DCS / APC string buffer
    enum class StringType : uint8_t { None, Osc, Dcs, Apc };
    StringType string_type_{StringType::None};
    std::string string_buffer_;

    // Callbacks
    ApcHandler apc_handler_;
    DcsHandler dcs_handler_;
    OscHandler osc_handler_;
    ResponseCallback response_cb_;
    std::function<void(bool)> sync_handler_;

    // UTF-8 state for multibyte characters in Ground state
    uint32_t utf8_state_{0};
    uint32_t utf8_codepoint_{0};
    uint32_t utf8_minimum_{0};
    bool string_overflow_{false};
    uint32_t default_fg_{0xffdddddd};
    uint32_t default_bg_{0xff111111};
};

} // namespace gritty::core
