#include "core/terminal_session.hpp"
#include "core/sixel_decoder.hpp"
#include "input/input_mapper.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
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
      engine_(config.cols, config.rows, config.scrollback_lines),
      parser_(engine_),
      font_engine_(config.antialias),
      renderer_(font_engine_, config.render_config) {
    engine_.configure_palette(config.palette);
    parser_.set_sync_handler([this](bool enabled) {
        if (!loop_) return;
        // foot terminal.c arms a one-second watchdog for DECSET 2026.
        if (enabled) loop_->set_timer(platform::IEventLoop::TimerId::SyncWatchdog, 1000, [this] {
            engine_.set_synchronized_updates(false);
            render_frame();
        });
        else loop_->set_timer(platform::IEventLoop::TimerId::SyncWatchdog, 0, {});
    });
    parser_.set_default_colors(config.render_config.default_fg, config.render_config.default_bg);
    parser_.set_default_color_handler([this](uint32_t foreground, uint32_t background) {
        auto colors = renderer_.config();
        colors.default_fg = foreground;
        colors.default_bg = background;
        renderer_.set_config(colors); // Invalidate text and window margins together.
    });
    parser_.set_response_callback([this](std::string_view resp) {
        write_pty(resp);
    });
    // tmux's set-clipboard writes reach the system pasteboard through OSC 52.
    parser_.set_clipboard_handler([this](std::string_view text) {
        if (clipboard_) clipboard_->copy_text(text);
    });
    // Kitty graphics: parse APC through the engine, and mirror every placement
    // into the terminal's render list (the image bitmap is shared by refcount).
    parser_.set_apc_handler([this](std::string_view payload) {
        const auto response = engine_.kitty_graphics().process_apc_sequence(
            payload, engine_.cursor_col(), engine_.cursor_row());
        if (!response.empty()) write_pty(response);
    });
    engine_.kitty_graphics().set_placement_handler(
        [this](const KittyPlacement& placement, pixman_image_t* image) {
            if (!image) return;
            PlacedImage placed;
            placed.image.reset(pixman_image_ref(image));
            placed.image_id = placement.image_id;
            placed.placement_id = placement.placement_id;
            placed.col = placement.col;
            placed.row = placement.row;
            const auto& metrics = font_engine_.metrics();
            const int image_w = pixman_image_get_width(image);
            const int image_h = pixman_image_get_height(image);
            placed.cols = placement.cols > 0 ? placement.cols
                : std::max(1, (image_w + metrics.cell_width - 1) / metrics.cell_width);
            placed.rows = placement.rows > 0 ? placement.rows
                : std::max(1, (image_h + metrics.cell_height - 1) / metrics.cell_height);
            placed.z_index = placement.z_index;
            placed.src_x = placement.src_x;
            placed.src_y = placement.src_y;
            placed.src_w = placement.src_w;
            placed.src_h = placement.src_h;
            placed.cursor_policy = placement.cursor_policy;
            const int advance_cols = placed.cols, advance_rows = placed.rows;
            engine_.place_image(std::move(placed));
            if (placement.cursor_policy == 0) { // Kitty C=0: advance over the placement.
                for (int i = 0; i < advance_cols; ++i) {
                    if (engine_.cursor_col() + 1 >= engine_.cols()) {
                        engine_.set_cursor_col(0);
                        engine_.line_feed();
                    } else {
                        engine_.move_cursor_forward(1);
                    }
                }
                // The cursor ends on the placement's last row, not one below it.
                for (int i = 0; i < std::max(0, advance_rows - 1); ++i)
                    engine_.line_feed(); // Scrolls when already at margin_bottom.
            }
        });
    engine_.kitty_graphics().set_delete_handler([this](uint32_t image_id) {
        engine_.remove_images_by_id(image_id);
    });
    engine_.kitty_graphics().set_placement_delete_handler([this](const KittyPlacement& placement) {
        engine_.remove_images_by_placement(placement.image_id, placement.placement_id);
    });
    // Sixel: DCS q... payload; place at the cursor cell, below the text.
    parser_.set_dcs_handler([this](std::string_view payload) {
        const auto q = payload.find('q');
        if (q == std::string_view::npos || q > 16) return;
        // Sixel header is "P1;P2;P3;P4 q"; anything else (e.g. tmux passthrough)
        // is not a sixel image.
        for (size_t i = 0; i < q; ++i)
            if (!std::isdigit(static_cast<unsigned char>(payload[i])) && payload[i] != ';') return;
        // DCS header is P1;P2;P3;P4 q. P2=1 selects a transparent background;
        // otherwise Sixel palette[0] fills pixels the image never touches.
        int p2 = 0;
        {
            const auto header = payload.substr(0, q);
            size_t begin = 0;
            size_t end = header.find(';');
            if (end != std::string_view::npos) begin = end + 1;
            if (begin < header.size()) {
                end = header.find(';', begin);
                const auto value = header.substr(begin, end == std::string_view::npos
                    ? std::string_view::npos : end - begin);
                std::from_chars(value.data(), value.data() + value.size(), p2);
            }
        }
        auto decoded = SixelDecoder::decode(payload.substr(q), p2 == 1);
        if (!decoded) return;
        const auto& metrics = font_engine_.metrics();
        if (metrics.cell_width <= 0 || metrics.cell_height <= 0) return;
        const int cols = std::max(1, (decoded->width() + metrics.cell_width - 1) / metrics.cell_width);
        const int rows = std::max(1, (decoded->height() + metrics.cell_height - 1) / metrics.cell_height);
        PlacedImage placed;
        placed.image = std::move(*decoded);
        placed.col = engine_.cursor_col();
        placed.row = engine_.cursor_row();
        placed.cols = cols;
        placed.rows = rows;
        placed.z_index = -1; // Sixel images sit behind the text.
        engine_.place_image(std::move(placed));
        // Keep the shell prompt from overwriting the image: move below it and
        // return to column zero. Feed one line at a time so a placement that
        // crosses margin_bottom scrolls the image into the visible region.
        const int placement_row = engine_.cursor_row();
        if (placement_row >= engine_.margin_top() && placement_row <= engine_.margin_bottom() &&
            placement_row + rows > engine_.margin_bottom()) {
            for (int i = 0; i < rows; ++i) engine_.line_feed();
        } else {
            engine_.move_cursor_down(rows);
        }
        engine_.set_cursor_col(0);
    });
}

TerminalSession::~TerminalSession() {
    if (loop_) {
        loop_->set_timer(platform::IEventLoop::TimerId::SyncWatchdog, 0, {});
        loop_->set_timer(platform::IEventLoop::TimerId::FrameCoalesce, 0, {});
    }
    if (surface_) {
        surface_->set_key_callback({});
        surface_->set_text_commit_callback({});
        surface_->set_paste_callback({});
        surface_->set_mouse_callback({});
        surface_->set_resize_callback({});
        surface_->set_close_callback({});
        surface_->set_frame_ready_callback({});
        surface_->set_mouse_reporting_provider({});
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
        // Committed IME text is already final UTF-8; never run it through the
        // KeyEvent/Kitty keyboard encoder, which would truncate CJK phrases.
        surface_->set_text_commit_callback([this](std::string_view text) {
            write_pty(text);
        });
        // Native Cmd+V bypasses key events/IME entirely; the engine decides
        // whether the child expects bracketed-paste markers.
        surface_->set_paste_callback([this](std::string_view text) {
            paste_text(text);
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
        surface_->set_frame_ready_callback([this] { render_pending_frame(); });
        surface_->set_focus_callback([this](bool focused) { on_window_focus(focused); });
        surface_->set_cursor_rect_provider([this]() -> platform::IWindowSurface::CursorRect {
            const auto& metrics = font_engine_.metrics();
            const double scale = surface_ ? surface_->scale() : 1.0;
            const double s = scale > 0 ? scale : 1.0;
            return {
                engine_.cursor_col() * metrics.cell_width / s,
                engine_.cursor_row() * metrics.cell_height / s,
                metrics.cell_width / s,
                metrics.cell_height / s,
            };
        });
        surface_->set_bracketed_paste_provider([this] { return engine_.bracketed_paste(); });
        surface_->set_mouse_reporting_provider([this] {
            return engine_.mouse_tracking() != MouseTrackingMode::None;
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
    bool got_data = false;
    bool hit_budget = false;
    size_t total_read = 0;
    constexpr size_t kByteBudget = 1024 * 1024;
    constexpr size_t kTimingThreshold = 64 * 1024;
    constexpr auto kTimeBudget = std::chrono::milliseconds(2);
    std::chrono::steady_clock::time_point started{};
    bool timing_active = false;
    size_t budget = kByteBudget;

    // Small interactive reads avoid all clock queries. Large flows start the
    // 2 ms budget only after 64 KiB so a continuously writing process cannot
    // starve UI events.
    while (budget > 0) {
        if (timing_active &&
            std::chrono::steady_clock::now() - started >= kTimeBudget) {
            hit_budget = true;
            break;
        }
        const size_t req = std::min(read_buf_.size(), budget);
        ssize_t n = pty_.read(std::span<uint8_t>(read_buf_.data(), req));
        if (n > 0) {
            got_data = true;
            const auto bytes = static_cast<size_t>(n);
            budget -= bytes;
            total_read += bytes;
            parser_.feed(read_buf_.data(), bytes);
            if (!timing_active && total_read >= kTimingThreshold) {
                started = std::chrono::steady_clock::now();
                timing_active = true;
            }
            if (budget == 0 ||
                (timing_active && std::chrono::steady_clock::now() - started >= kTimeBudget))
                hit_budget = true; // Stopped by budget, not by EAGAIN.
        } else if (n == 0) { // EOF
            if (config_.hold_on_exit) {
                // Stop polling the closed PTY but keep the event loop (and the
                // window) alive until the user closes it.
                if (loop_ && pty_.master_fd() >= 0) loop_->remove_fd(pty_.master_fd());
            } else if (loop_) loop_->stop();
            break;
        } else {
            if (errno == EINTR) continue;
            // Linux PTYs report EIO when the slave closes, not EOF.
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                if (config_.hold_on_exit) {
                    if (loop_ && pty_.master_fd() >= 0) loop_->remove_fd(pty_.master_fd());
                } else if (loop_) loop_->stop();
            }
            break;
        }
    }

    if (got_data) {
        if (coalesce_frames_ && loop_ && surface_ && surface_->supports_frame_pacing()) {
            if (surface_->frame_in_flight()) {
                // Keep parsing; the frame clock will render the accumulated state.
                render_frame(false);
            } else if (!hit_budget && total_read <= 512) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last_immediate_commit_ >= std::chrono::milliseconds(2)) {
                    // Idle keystroke/cursor reply: bypass the coalescing timer.
                    if (coalesce_pending_) {
                        loop_->set_timer(platform::IEventLoop::TimerId::FrameCoalesce, 0, {});
                        coalesce_pending_ = false;
                    }
                    render_pending_frame();
                    last_immediate_commit_ = now;
                } else if (!coalesce_pending_) {
                    // A burst of tiny writes must not submit one surface per
                    // packet; use a 2 ms coalescing window.
                    coalesce_pending_ = true;
                    loop_->set_timer(platform::IEventLoop::TimerId::FrameCoalesce, 2, [this] {
                        coalesce_pending_ = false;
                        render_frame(false);
                    });
                }
            } else if (!coalesce_pending_) {
                coalesce_pending_ = true;
                loop_->set_timer(platform::IEventLoop::TimerId::FrameCoalesce, 2, [this] {
                    coalesce_pending_ = false;
                    render_frame(false);
                });
            }
        } else {
            render_frame(false);
        }
    }
}

void TerminalSession::paste_text(std::string_view text) {
    if (text.empty()) return;
    write_pty(engine_.format_paste_payload(text));
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

    // Cmd+click on an OSC 8 link opens it. When tmux has mouse reporting on,
    // the click belongs to tmux (it can forward or handle links itself).
    if (ev.button == input::MouseButton::Left && ev.action == input::MouseAction::Press &&
        ev.mods.meta && engine_.mouse_tracking() == core::MouseTrackingMode::None) {
        const auto& cell = engine_.grid().cell(normalized.col, normalized.row);
        const uint16_t link_id = engine_.grid().get_cell_extra(cell.extra_idx).hyperlink_id;
        if (link_id != 0) {
            const auto url = engine_.get_hyperlink_url(link_id);
            if (!url.empty()) {
                if (url_open_handler_) url_open_handler_(url);
                else if (surface_) surface_->open_url(url);
                return;
            }
        }
    }

    std::string bytes = input::InputMapper::map_mouse(normalized, engine_);
    if (!bytes.empty()) {
        write_pty(bytes);
    }
}

void TerminalSession::on_window_focus(bool focused) {
    if (!engine_.focus_events()) return;
    write_pty(focused ? "\x1b[I" : "\x1b[O");
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
    full_redraw_pending_ |= full_redraw;
    if (!surface_ || engine_.synchronized_updates()) return;
    if (!coalesce_frames_) {
        // Legacy pacing: always wait for the native frame clock.
        if (!surface_->request_frame()) render_pending_frame();
        return;
    }
    if (surface_->frame_in_flight()) {
        // A commit is already pending compositing: keep parsing PTY data and
        // accumulate damage, but do not rasterize mid-interval. The frame clock
        // renders the newest state once per vsync (foot's frame callback).
        if (!surface_->request_frame()) render_pending_frame();
        return;
    }
    // Idle: commit the first response immediately instead of waiting a whole
    // refresh interval for the next frame tick.
    render_pending_frame();
}

void TerminalSession::render_pending_frame() {
    if (!surface_ || engine_.synchronized_updates()) return;
    if (engine_.window_title() != last_title_) {
        last_title_ = engine_.window_title();
        surface_->set_title(last_title_);
    }

    pixman_image_t* back_buf = surface_->get_back_buffer();
    if (!back_buf) return;

    damage_region_.clear();
    renderer_.render(engine_, back_buf, &damage_region_, full_redraw_pending_);
    full_redraw_pending_ = false;
    surface_->present(damage_region_);
    if (present_observer_) present_observer_();
}

void TerminalSession::run() {
    if (loop_) {
        loop_->run();
    }
}

} // namespace gritty::core
