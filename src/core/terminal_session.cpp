#include "core/terminal_session.hpp"
#include "input/input_mapper.hpp"

namespace gritty::core {

TerminalSession::TerminalSession(
    const SessionConfig& config,
    std::unique_ptr<platform::IWindowSurface> surface,
    std::unique_ptr<platform::IEventLoop> loop,
    std::unique_ptr<platform::IClipboard> clipboard
)
    : config_(config),
      surface_(std::move(surface)),
      loop_(std::move(loop)),
      clipboard_(std::move(clipboard)),
      engine_(config.cols, config.rows),
      parser_(engine_),
      renderer_(font_engine_) {
    parser_.set_response_callback([this](std::string_view resp) {
        pty_.write(resp);
    });
}

TerminalSession::~TerminalSession() {
    if (loop_ && pty_.master_fd() >= 0) {
        loop_->remove_fd(pty_.master_fd());
    }
}

bool TerminalSession::start() {
    if (!font_engine_.load_font(config_.font_path, config_.font_size)) {
        return false;
    }

    const auto& metrics = font_engine_.metrics();
    int cell_w = metrics.cell_width;
    int cell_h = metrics.cell_height;

    platform::PtySpawnOptions pty_opts{
        .command = config_.shell,
        .cwd = config_.cwd,
        .cols = config_.cols,
        .rows = config_.rows,
        .pixel_width = config_.cols * cell_w,
        .pixel_height = config_.rows * cell_h
    };

    if (!pty_.spawn(pty_opts)) {
        return false;
    }

    if (loop_) {
        loop_->add_fd(pty_.master_fd(), platform::IEventLoop::EventReadable,
            [this](int, uint32_t) {
                handle_pty_read();
            });
    }

    render_frame(true);
    return true;
}

void TerminalSession::handle_pty_read() {
    uint8_t buf[16384];
    bool got_data = false;

    while (true) {
        ssize_t n = pty_.read(std::span<uint8_t>(buf, sizeof(buf)));
        if (n > 0) {
            got_data = true;
            parser_.feed(buf, static_cast<size_t>(n));
        } else if (n == 0) { // EOF
            if (loop_) loop_->stop();
            break;
        } else { // EAGAIN or error
            break;
        }
    }

    if (got_data) {
        render_frame(false);
    }
}

void TerminalSession::handle_key_event(const input::KeyEvent& ev) {
    std::string bytes = input::InputMapper::map_key(ev, engine_);
    if (!bytes.empty()) {
        pty_.write(bytes);
    }
}

void TerminalSession::handle_mouse_event(const input::MouseEvent& ev) {
    std::string bytes = input::InputMapper::map_mouse(ev, engine_);
    if (!bytes.empty()) {
        pty_.write(bytes);
    }
}

void TerminalSession::handle_surface_resize(int px_w, int px_h, double scale) {
    (void)scale;
    const auto& metrics = font_engine_.metrics();
    int new_cols = px_w / metrics.cell_width;
    int new_rows = px_h / metrics.cell_height;

    if (new_cols > 0 && new_rows > 0 && (new_cols != engine_.cols() || new_rows != engine_.rows())) {
        engine_.resize(new_cols, new_rows);
        pty_.resize(new_cols, new_rows, px_w, px_h);
        render_frame(true);
    }
}

void TerminalSession::render_frame(bool full_redraw) {
    if (!surface_) return;

    pixman_image_t* back_buf = surface_->get_back_buffer();
    if (!back_buf) return;

    damage_region_.clear();
    renderer_.render(engine_, back_buf, &damage_region_, full_redraw);
    surface_->present(damage_region_);
}

void TerminalSession::run() {
    if (loop_) {
        loop_->run();
    }
}

} // namespace gritty::core
