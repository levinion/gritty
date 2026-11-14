#pragma once

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

    void handle_key_event(const input::KeyEvent& ev);
    void handle_mouse_event(const input::MouseEvent& ev);
    void handle_surface_resize(int px_w, int px_h, double scale);
    void write_pty(std::string_view data);

private:
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
    std::string last_title_;
    double font_scale_{1.0};
    std::string pending_write_;
    size_t write_offset_{0};
    bool watching_write_{false};
};

} // namespace gritty::core
