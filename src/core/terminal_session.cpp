#include "core/terminal_session.hpp"
#include "input/input_mapper.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>

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
      font_engine_(config.antialias),
      renderer_(font_engine_, config.render_config) {
    parser_.set_sync_handler([this](bool enabled) {
        if (!loop_) return;
        // foot terminal.c arms a one-second watchdog for DECSET 2026.
        if (enabled) loop_->set_timer(1000, [this] {
            engine_.set_synchronized_updates(false);
            render_frame();
        });
        else loop_->set_timer(0, {});
    });
    parser_.set_default_colors(config.render_config.default_fg, config.render_config.default_bg);
    parser_.set_response_callback([this](std::string_view resp) {
        write_pty(resp);
    });
}

TerminalSession::~TerminalSession() {
    if (loop_) loop_->set_timer(0, {});
    if (surface_) {
        surface_->set_key_callback({});
        surface_->set_mouse_callback({});
        surface_->set_resize_callback({});
        surface_->set_close_callback({});
    }
    if (loop_ && pty_.master_fd() >= 0) {
        loop_->remove_fd(pty_.master_fd());
    }
}

bool TerminalSession::start() {
    if (surface_) surface_->set_transparent((config_.render_config.default_bg >> 24) != 255);
    font_scale_ = surface_ ? surface_->scale() : 1.0;
    if (!font_engine_.load_font(config_.font_path, config_.font_size,
            static_cast<unsigned int>(std::lround(config_.font_dpi * font_scale_)))) {
        return false;
    }

    const auto& metrics = font_engine_.metrics();
    int cell_w = metrics.cell_width;
    int cell_h = metrics.cell_height;

    platform::PtySpawnOptions pty_opts{
        .command = config_.shell,
        .args = config_.args,
        .cwd = config_.cwd,
        .env = {},
        .cols = config_.cols,
        .rows = config_.rows,
        .pixel_width = config_.cols * cell_w,
        .pixel_height = config_.rows * cell_h
    };

    if (!pty_.spawn(pty_opts)) {
        return false;
    }

    watch_pty(false);

    if (surface_) {
        surface_->set_key_callback([this](const input::KeyEvent& ev) {
            handle_key_event(ev);
        });
        surface_->set_mouse_callback([this](const input::MouseEvent& ev) {
            handle_mouse_event(ev);
        });
        surface_->set_resize_callback([this](int w, int h, double s) {
            handle_surface_resize(w, h, s);
        });
        surface_->set_close_callback([this]() {
            if (loop_) loop_->stop();
        });

        if (cell_w > 0 && cell_h > 0) {
            surface_->resize(config_.cols * cell_w, config_.rows * cell_h, surface_->scale());
        }
    }

    render_frame(true);
    return true;
}

void TerminalSession::watch_pty(bool writable) {
    watching_write_ = writable;
    if (!loop_) return;
    loop_->remove_fd(pty_.master_fd());
    const auto mask = platform::IEventLoop::EventReadable |
        (writable ? platform::IEventLoop::EventWritable : 0);
    loop_->add_fd(pty_.master_fd(), mask, [this](int, uint32_t flags) {
        if (flags & (platform::IEventLoop::EventReadable | platform::IEventLoop::EventError))
            handle_pty_read();
        if (flags & platform::IEventLoop::EventWritable) flush_pty_write();
    });
}

void TerminalSession::write_pty(std::string_view data) {
    if (data.empty() || pty_.master_fd() < 0) return;
    pending_write_.append(data);
    flush_pty_write();
}

void TerminalSession::flush_pty_write() {
    while (write_offset_ < pending_write_.size()) {
        const auto n = pty_.write(std::string_view(pending_write_).substr(write_offset_));
        if (n > 0) {
            write_offset_ += static_cast<size_t>(n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            pending_write_.clear();
            write_offset_ = 0;
            if (loop_) loop_->stop();
            break;
        }
    }
    if (write_offset_ == pending_write_.size()) {
        pending_write_.clear();
        write_offset_ = 0;
    }
    const bool writable = !pending_write_.empty();
    if (writable != watching_write_) watch_pty(writable);
}

void TerminalSession::handle_pty_read() {
    uint8_t buf[16384];
    bool got_data = false;

    // Bound each dispatch so a continuously writing process cannot starve UI events.
    size_t budget = 256 * 1024;
    while (budget > 0) {
        ssize_t n = pty_.read(std::span<uint8_t>(buf, std::min(sizeof(buf), budget)));
        if (n > 0) {
            got_data = true;
            budget -= static_cast<size_t>(n);
            parser_.feed(buf, static_cast<size_t>(n));
        } else if (n == 0) { // EOF
            if (loop_) loop_->stop();
            break;
        } else {
            if (errno == EINTR) continue;
            // Linux PTYs report EIO when the slave closes, not EOF.
            if (errno != EAGAIN && errno != EWOULDBLOCK && loop_) loop_->stop();
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
        write_pty(bytes);
    }
}

void TerminalSession::handle_mouse_event(const input::MouseEvent& ev) {
    auto normalized = ev;
    const auto& metrics = font_engine_.metrics();
    if (metrics.cell_width <= 0 || metrics.cell_height <= 0) return;
    normalized.pixel_x = std::clamp(ev.pixel_x, 0, engine_.cols() * metrics.cell_width - 1);
    normalized.pixel_y = std::clamp(ev.pixel_y, 0, engine_.rows() * metrics.cell_height - 1);
    normalized.col = normalized.pixel_x / metrics.cell_width;
    normalized.row = normalized.pixel_y / metrics.cell_height;
    std::string bytes = input::InputMapper::map_mouse(normalized, engine_);
    if (!bytes.empty()) {
        write_pty(bytes);
    }
}

void TerminalSession::handle_surface_resize(int px_w, int px_h, double scale) {
    if (px_w <= 0 || px_h <= 0 || scale <= 0) return;
    if (scale != font_scale_) {
        if (!font_engine_.load_font(config_.font_path, config_.font_size,
                static_cast<unsigned int>(std::lround(config_.font_dpi * scale)))) return;
        font_scale_ = scale;
    }
    const auto& metrics = font_engine_.metrics();
    if (metrics.cell_width <= 0 || metrics.cell_height <= 0) return;
    int new_cols = px_w / metrics.cell_width;
    int new_rows = px_h / metrics.cell_height;

    if (new_cols > 0 && new_rows > 0 && (new_cols != engine_.cols() || new_rows != engine_.rows())) {
        engine_.resize(new_cols, new_rows);
    }
    // The surface may have been reallocated even when its cell count is the
    // same. Update PTY pixel dimensions and repaint the entire new buffer.
    if (new_cols > 0 && new_rows > 0) pty_.resize(new_cols, new_rows, px_w, px_h);
    render_frame(true);
}

void TerminalSession::render_frame(bool full_redraw) {
    if (!surface_ || engine_.synchronized_updates()) return;
    if (engine_.window_title() != last_title_) {
        last_title_ = engine_.window_title();
        surface_->set_title(last_title_);
    }

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
