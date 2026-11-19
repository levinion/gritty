#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include "core/terminal_engine.hpp"
#include "core/vt_parser.hpp"
#include "render/cpu_renderer.hpp"
#include "render/font_engine.hpp"
#include "platform/posix_pty.hpp"
#include "platform/pal_interfaces.hpp"
#include "input/input_events.hpp"

namespace gritty::core {

struct SessionConfig {
    std::string font_path;
    double font_size{13.0};
    int cols{80};
    int rows{24};
    std::string shell;
    std::string cwd;
    std::vector<std::string> args;
    unsigned int font_dpi{96};
    render::AntialiasMode antialias{render::AntialiasMode::Grayscale};
    render::RenderConfig render_config;
    std::array<uint32_t, 256> palette{kDefaultPalette};
    // Mirrors config::MainConfig::osc52_read.
    bool osc52_read{false};
    bool hold_on_exit{false};
};

class TerminalSession final {
public:
    TerminalSession(
        const SessionConfig& config,
        std::unique_ptr<platform::IWindowSurface> surface,
        std::unique_ptr<platform::IEventLoop> loop,
        std::unique_ptr<platform::IClipboard> clipboard
    );
    ~TerminalSession();

    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;

    bool start();
    void render_frame(bool full_redraw = false);
    void run();

    [[nodiscard]] TerminalEngine& engine() noexcept { return engine_; }
    [[nodiscard]] platform::IWindowSurface& surface() noexcept { return *surface_; }
    [[nodiscard]] platform::IEventLoop& event_loop() noexcept { return *loop_; }
    [[nodiscard]] const render::FontEngine& font_engine() const noexcept { return font_engine_; }

    void handle_key_event(const input::KeyEvent& ev);
    // Format clipboard text for the child (DECSET 2004) and write it to PTY.
    void paste_text(std::string_view text);
    void handle_mouse_event(const input::MouseEvent& ev);
    void handle_surface_resize(int px_w, int px_h, double scale);
    void write_pty(std::string_view data);

    // foot's commit/frame-callback pacing: disabled only for A/B measurement.
    void set_frame_coalescing(bool enabled) noexcept { coalesce_frames_ = enabled; }
    [[nodiscard]] bool frame_coalescing() const noexcept { return coalesce_frames_; }
    // Whole-region scrolls move pixels with memmove instead of re-rasterizing.
    void set_scroll_optimization(bool enabled) noexcept { renderer_.set_scroll_optimization(enabled); }
    void set_glyph_batching(bool enabled) noexcept { renderer_.set_glyph_batching(enabled); }
    [[nodiscard]] const render::CpuRenderer::RenderStats& render_stats() const noexcept { return renderer_.stats(); }
    // Fires once per submitted frame; used by probes to time commit latency.
    void set_present_observer(std::function<void()> cb) { present_observer_ = std::move(cb); }
    // Cmd+click on an OSC 8 link opens it; tests can inject a handler.
    void set_url_open_handler(std::function<void(std::string_view)> cb) { url_open_handler_ = std::move(cb); }
    // Window focus changes feed DECSET 1004 reports to the child.
    void on_window_focus(bool focused);

private:
    void render_pending_frame();
    void handle_pty_read();
    void flush_pty_write();
    void watch_pty(bool writable);

    SessionConfig config_;
    std::unique_ptr<platform::IWindowSurface> surface_;
    std::unique_ptr<platform::IEventLoop> loop_;
    std::unique_ptr<platform::IClipboard> clipboard_;

    TerminalEngine engine_;
    VtParser parser_;
    render::FontEngine font_engine_;
    render::CpuRenderer renderer_;
    platform::PosixPty pty_;

    util::PixmanRegion damage_region_;
    std::array<uint8_t, 65536> read_buf_{};
    std::string last_title_;
    double font_scale_{1.0};
    std::string pending_write_;
    size_t write_offset_{0};
    bool watching_write_{false};
    bool full_redraw_pending_{false};
    bool coalesce_frames_{true};
    bool coalesce_pending_{false};
    std::chrono::steady_clock::time_point last_immediate_commit_{};
    std::function<void()> present_observer_;
    std::function<void(std::string_view)> url_open_handler_;
};

} // namespace gritty::core
