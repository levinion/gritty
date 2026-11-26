#include "core/terminal_session.hpp"
#include "input/input_mapper.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>

namespace gritty::core {

namespace {
// Safety net for a child that enables DECSET 2026 but never sends ESU.
constexpr int kSyncTimeoutMs = 16;
} // namespace

TerminalSession::TerminalSession(
    const SessionConfig& config,
    std::unique_ptr<platform::IWindowSurface> surface,
    std::unique_ptr<platform::IEventLoop> loop,
    std::unique_ptr<platform::IClipboard> clipboard
)
    : config_(config),
      surface_(std::move(surface)),
      owned_loop_(std::move(loop)),
      loop_(owned_loop_.get()),
      clipboard_(std::move(clipboard)),
      engine_(config.cols, config.rows),
      parser_(engine_),
      font_engine_(config.antialias),
      renderer_(font_engine_, config.render_config) {
    init_session();
}

TerminalSession::TerminalSession(
    const SessionConfig& config,
    std::unique_ptr<platform::IWindowSurface> surface,
    platform::IEventLoop* shared_loop,
    std::unique_ptr<platform::IClipboard> clipboard
)
    : config_(config),
      surface_(std::move(surface)),
      loop_(shared_loop),
      clipboard_(std::move(clipboard)),
      engine_(config.cols, config.rows),
      parser_(engine_),
      font_engine_(config.antialias),
      renderer_(font_engine_, config.render_config) {
    init_session();
}

void TerminalSession::init_session() {
    engine_.configure_palette(config_.palette);
    engine_.configure_initial_colors(config_.render_config.default_fg,
                                     config_.render_config.default_bg,
                                     config_.render_config.cursor_color);
    parser_.set_sync_handler([this](bool enabled) {
        if (enabled) {
            if (loop_) loop_->set_scoped_timer(
                reinterpret_cast<uintptr_t>(this),
                platform::IEventLoop::TimerId::SyncWatchdog,
                kSyncTimeoutMs, [this] { finish_synchronized_update(); });
        } else {
            finish_synchronized_update();
        }
    });
    parser_.set_default_colors(config_.render_config.default_fg, config_.render_config.default_bg,
                               config_.render_config.cursor_color);
    parser_.set_default_color_handler([this](uint32_t foreground, uint32_t background) {
        auto colors = renderer_.config();
        colors.default_fg = foreground;
        colors.default_bg = background;
        renderer_.set_config(colors); // Invalidate text and window margins together.
        full_redraw_pending_ = true;
    });
    parser_.set_cursor_color_handler([this](uint32_t argb) {
        auto colors = renderer_.config();
        colors.cursor_color = argb;
        renderer_.set_config(colors);
    });
    parser_.set_mouse_cursor_shape_handler([this](platform::MouseCursorShape shape) {
        if (surface_) surface_->set_cursor_shape(shape);
    });
    parser_.set_notification_handler([this](std::string_view title, std::string_view body) {
        if (surface_) surface_->send_notification(title, body);
    });
    parser_.set_response_callback([this](std::string_view resp) {
        write_pty(resp);
    });
    // tmux's set-clipboard writes reach the system pasteboard through OSC 52.
    parser_.set_clipboard_handler([this](std::string_view text) {
        if (clipboard_) clipboard_->copy_text(text);
    });
    parser_.set_osc52_mode(config_.osc52);
    parser_.set_clipboard_read_handler([this] {
        return clipboard_ ? clipboard_->paste_text() : std::string{};
    });
    // Kitty graphics: parse APC through the engine, and mirror every placement
    // into the terminal's render list (the image bitmap is shared by refcount).
    parser_.set_apc_handler([this](std::string_view payload) {
        const auto response = engine_.kitty_graphics().process_apc_sequence(
            payload, engine_.cursor_col(), engine_.cursor_row());
        if (!response.empty()) write_pty(response);
    });
    engine_.kitty_graphics().set_placement_handler(
        [this](KittyPlacement& placement, pixman_image_t* image) {
            if (!image) return;
            PlacedImage placed;
            placed.image.reset(pixman_image_ref(image));
            placed.uid = placement.uid;
            placed.image_id = placement.image_id;
            placed.placement_id = placement.placement_id;
            placed.col = placement.col;
            placed.row = placement.row;
            const auto& metrics = font_engine_.metrics();
            const int image_w = pixman_image_get_width(image);
            const int image_h = pixman_image_get_height(image);
            const int eff_x = std::clamp(placement.src_x, 0, std::max(0, image_w - 1));
            const int eff_y = std::clamp(placement.src_y, 0, std::max(0, image_h - 1));
            const int eff_w = placement.src_w > 0
                ? std::clamp(placement.src_w, 1, std::max(1, image_w - eff_x))
                : std::max(1, image_w - eff_x);
            const int eff_h = placement.src_h > 0
                ? std::clamp(placement.src_h, 1, std::max(1, image_h - eff_y))
                : std::max(1, image_h - eff_y);
            placed.x_offset = std::max(0, placement.x_offset);
            placed.y_offset = std::max(0, placement.y_offset);
            const int cell_w = std::max(1, metrics.cell_width);
            const int cell_h = std::max(1, metrics.cell_height);
            if (placement.cols > 0 && placement.rows > 0) {
                placed.cols = placement.cols;
                placed.rows = placement.rows;
                placed.draw_w = 0;
                placed.draw_h = 0;
            } else if (placement.cols > 0) {
                placed.cols = placement.cols;
                placed.draw_w = placement.cols * cell_w;
                placed.draw_h = std::max(1, static_cast<int>(
                    std::lround(static_cast<double>(placed.draw_w) * eff_h / std::max(1, eff_w))));
                placed.rows = std::max(1, (placed.draw_h + cell_h - 1) / cell_h);
            } else if (placement.rows > 0) {
                placed.rows = placement.rows;
                placed.draw_h = placement.rows * cell_h;
                placed.draw_w = std::max(1, static_cast<int>(
                    std::lround(static_cast<double>(placed.draw_h) * eff_w / std::max(1, eff_h))));
                placed.cols = std::max(1, (placed.draw_w + cell_w - 1) / cell_w);
            } else {
                placed.draw_w = std::max(1, eff_w);
                placed.draw_h = std::max(1, eff_h);
                placed.cols = std::max(1, (placed.draw_w + placed.x_offset + cell_w - 1) / cell_w);
                placed.rows = std::max(1, (placed.draw_h + placed.y_offset + cell_h - 1) / cell_h);
            }
            placement.cols = placed.cols;
            placement.rows = placed.rows;
            placement.alt_screen = engine_.is_alt_screen();
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
        if (placement.uid != 0) engine_.remove_image_by_uid(placement.uid);
        else engine_.remove_images_by_placement(placement.image_id, placement.placement_id);
    });
    engine_.kitty_graphics().set_virtual_placement_change_handler([this]() {
        engine_.grid().mark_all_dirty();
    });
}

TerminalSession::~TerminalSession() {
    if (loop_) {
        const auto owner = reinterpret_cast<uintptr_t>(this);
        loop_->cancel_scoped_timer(owner, platform::IEventLoop::TimerId::SyncWatchdog);
        loop_->cancel_scoped_timer(owner, platform::IEventLoop::TimerId::FrameCoalesce);
    }
    if (surface_) {
        surface_->set_key_callback({});
        surface_->set_text_commit_callback({});
        surface_->set_preedit_callback({});
        surface_->set_paste_callback({});
        surface_->set_mouse_callback({});
        surface_->set_resize_callback({});
        surface_->set_close_callback({});
        surface_->set_frame_ready_callback({});
        surface_->set_focus_callback({});
        surface_->set_cursor_rect_provider({});
        surface_->set_bracketed_paste_provider({});
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
            static_cast<unsigned int>(std::lround(config_.font_dpi * font_scale_)),
            config_.font_postscript_name)) {
        return false;
    }
    update_padding_pixels();

    const auto& metrics = font_engine_.metrics();
    int cell_w = metrics.cell_width;
    int cell_h = metrics.cell_height;
    engine_.set_cell_size(cell_w, cell_h);

    platform::PtySpawnOptions pty_opts{
        .command = config_.shell,
        .args = config_.args,
        .cwd = config_.cwd,
        .env = config_.env,
        .term = config_.term,
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
        surface_->set_preedit_callback([this](std::string_view utf8, size_t sel_start, size_t sel_end) {
            handle_preedit(utf8, sel_start, sel_end);
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
            request_exit();
        });
        surface_->set_frame_ready_callback([this] { render_pending_frame(); });
        surface_->set_focus_callback([this](bool focused) { on_window_focus(focused); });
        surface_->set_cursor_rect_provider([this]() -> platform::IWindowSurface::CursorRect {
            const auto& metrics = font_engine_.metrics();
            const double scale = surface_ ? surface_->scale() : 1.0;
            const double s = scale > 0 ? scale : 1.0;
            return {
                padding_x_px_ / s + engine_.cursor_col() * metrics.cell_width / s,
                padding_y_px_ / s + engine_.cursor_row() * metrics.cell_height / s,
                metrics.cell_width / s,
                metrics.cell_height / s,
            };
        });
        surface_->set_bracketed_paste_provider([this] { return engine_.bracketed_paste(); });
        surface_->set_mouse_reporting_provider([this] {
            return engine_.mouse_tracking() != MouseTrackingMode::None;
        });

        if (cell_w > 0 && cell_h > 0) {
            surface_->resize(config_.cols * cell_w + 2 * padding_x_px_,
                             config_.rows * cell_h + 2 * padding_y_px_, surface_->scale());
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
            request_exit();
            return;
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
    bool should_exit = false;
    size_t total_read = 0;
    constexpr size_t kByteBudget = 1024 * 1024;
    constexpr size_t kTimingThreshold = 64 * 1024;
    constexpr auto kTimeBudget = std::chrono::milliseconds(2);
    std::chrono::steady_clock::time_point started{};
    bool timing_active = false;
    size_t budget = kByteBudget;
    parsing_pty_ = true;

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
            } else {
                should_exit = true;
            }
            break;
        } else {
            if (errno == EINTR) continue;
            // Linux PTYs report EIO when the slave closes, not EOF.
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                if (config_.hold_on_exit) {
                    if (loop_ && pty_.master_fd() >= 0) loop_->remove_fd(pty_.master_fd());
                } else {
                    should_exit = true;
                }
            }
            break;
        }
    }

    parsing_pty_ = false;
    if (sync_flush_pending_) {
        // ESU/RIS arrived mid-read: flush once, after the whole VT chunk has
        // been parsed, so the submitted frame is never a half-updated grid.
        sync_flush_pending_ = false;
        render_frame(false);
    } else if (got_data) {
        if (!should_exit && coalesce_frames_ && loop_ && surface_ && surface_->supports_frame_pacing()) {
            const auto owner = reinterpret_cast<uintptr_t>(this);
            if (surface_->frame_in_flight()) {
                // Keep parsing; the frame clock will render the accumulated state.
                render_frame(false);
            } else if (!hit_budget && total_read <= 512) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last_immediate_commit_ >= std::chrono::milliseconds(2)) {
                    // Idle keystroke/cursor reply: bypass the coalescing timer.
                    if (coalesce_pending_) {
                        loop_->cancel_scoped_timer(owner, platform::IEventLoop::TimerId::FrameCoalesce);
                        coalesce_pending_ = false;
                    }
                    render_pending_frame();
                    last_immediate_commit_ = now;
                } else if (!coalesce_pending_) {
                    // A burst of tiny writes must not submit one surface per
                    // packet; use a 2 ms coalescing window.
                    coalesce_pending_ = true;
                    loop_->set_scoped_timer(owner, platform::IEventLoop::TimerId::FrameCoalesce, 2, [this] {
                        coalesce_pending_ = false;
                        render_frame(false);
                    });
                }
            } else if (!coalesce_pending_) {
                coalesce_pending_ = true;
                loop_->set_scoped_timer(owner, platform::IEventLoop::TimerId::FrameCoalesce, 2, [this] {
                    coalesce_pending_ = false;
                    render_frame(false);
                });
            }
        } else {
            render_frame(false);
        }
    }
    if (should_exit) request_exit();
}

void TerminalSession::request_exit() {
    if (exit_requested_) return;
    exit_requested_ = true;
    if (loop_ && pty_.master_fd() >= 0) {
        loop_->remove_fd(pty_.master_fd());
    }
    if (exit_cb_) exit_cb_();
    else if (loop_) loop_->stop();
}

void TerminalSession::set_status_banner(std::string_view utf8) {
    renderer_.set_status_banner(utf8);
    render_frame(false);
}

void TerminalSession::handle_preedit(std::string_view utf8, size_t sel_start, size_t sel_end) {
    renderer_.set_preedit(utf8, sel_start, sel_end);
    render_frame(false);
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
    // Mouse coordinates arrive in surface pixels; padding belongs to the
    // window frame, so clicks in it are not terminal cells.
    const int x = ev.pixel_x - padding_x_px_;
    const int y = ev.pixel_y - padding_y_px_;
    const int content_w = engine_.cols() * metrics.cell_width;
    const int content_h = engine_.rows() * metrics.cell_height;
    if (x < 0 || y < 0 || x >= content_w || y >= content_h) return;
    normalized.pixel_x = x;
    normalized.pixel_y = y;
    normalized.col = x / metrics.cell_width;
    normalized.row = y / metrics.cell_height;

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
        if (!font_engine_.set_dpi(
                static_cast<unsigned int>(std::lround(config_.font_dpi * scale)))) return;
        font_scale_ = scale;
    }
    update_padding_pixels();
    const auto& metrics = font_engine_.metrics();
    if (metrics.cell_width <= 0 || metrics.cell_height <= 0) return;
    engine_.set_cell_size(metrics.cell_width, metrics.cell_height);
    int new_cols = (px_w - 2 * padding_x_px_) / metrics.cell_width;
    int new_rows = (px_h - 2 * padding_y_px_) / metrics.cell_height;

    if (new_cols > 0 && new_rows > 0 && (new_cols != engine_.cols() || new_rows != engine_.rows())) {
        engine_.resize(new_cols, new_rows);
    }
    // The surface may have been reallocated even when its cell count is the
    // same. Update PTY pixel dimensions and repaint the entire new buffer.
    if (new_cols > 0 && new_rows > 0)
        pty_.resize(new_cols, new_rows, new_cols * metrics.cell_width, new_rows * metrics.cell_height);
    render_frame(true);
}

void TerminalSession::update_padding_pixels() noexcept {
    const double s = font_scale_ > 0 ? font_scale_ : 1.0;
    padding_x_px_ = std::max(0, static_cast<int>(std::lround(config_.padding_x * s)));
    padding_y_px_ = std::max(0, static_cast<int>(std::lround(config_.padding_y * s)));
}

void TerminalSession::finish_synchronized_update() {
    if (loop_) loop_->cancel_scoped_timer(
        reinterpret_cast<uintptr_t>(this),
        platform::IEventLoop::TimerId::SyncWatchdog);
    // ESU, RIS and the safety timeout all converge here: release the guard and
    // commit the accumulated damage atomically.
    engine_.set_synchronized_updates(false);
    if (parsing_pty_) sync_flush_pending_ = true;
    else render_frame(false);
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
    if (!surface_ || !surface_->is_ready() || engine_.synchronized_updates()) return;
    if (engine_.window_title() != last_title_) {
        last_title_ = engine_.window_title();
        surface_->set_title(last_title_);
    }

    const bool full_redraw = full_redraw_pending_;
    pixman_image_t* back_buf = surface_->get_back_buffer_for_frame(full_redraw);
    if (!back_buf) return;

    const int target_w = pixman_image_get_width(back_buf);
    const int target_h = pixman_image_get_height(back_buf);
    const auto& metrics = font_engine_.metrics();
    const int content_w = std::min(engine_.cols() * metrics.cell_width,
                                   target_w - 2 * padding_x_px_);
    const int content_h = std::min(engine_.rows() * metrics.cell_height,
                                   target_h - 2 * padding_y_px_);
    full_redraw_pending_ = false;

    damage_region_.clear();
    if (full_redraw || content_w <= 0 || content_h <= 0) {
        // Padding is static; repaint only the outer border strips on full redraw
        // (or the whole target when the grid has zero area), leaving the content
        // area to CpuRenderer's single-pass background + foreground render.
        const pixman_color_t bg = render::CpuRenderer::to_pixman_color(renderer_.config().default_bg);
        if (content_w <= 0 || content_h <= 0) {
            const pixman_rectangle16_t rect{0, 0,
                static_cast<uint16_t>(std::max(0, target_w)),
                static_cast<uint16_t>(std::max(0, target_h))};
            pixman_image_fill_rectangles(PIXMAN_OP_SRC, back_buf, &bg, 1, &rect);
        } else {
            pixman_rectangle16_t borders[4];
            int count = 0;
            const int bottom_y = padding_y_px_ + content_h;
            const int right_x = padding_x_px_ + content_w;
            if (padding_y_px_ > 0) {
                borders[count++] = {0, 0, static_cast<uint16_t>(target_w),
                    static_cast<uint16_t>(std::min(padding_y_px_, target_h))};
            }
            if (bottom_y < target_h) {
                borders[count++] = {0, static_cast<int16_t>(bottom_y),
                    static_cast<uint16_t>(target_w), static_cast<uint16_t>(target_h - bottom_y)};
            }
            if (padding_x_px_ > 0) {
                borders[count++] = {0, static_cast<int16_t>(padding_y_px_),
                    static_cast<uint16_t>(std::min(padding_x_px_, target_w)),
                    static_cast<uint16_t>(content_h)};
            }
            if (right_x < target_w) {
                borders[count++] = {static_cast<int16_t>(right_x), static_cast<int16_t>(padding_y_px_),
                    static_cast<uint16_t>(target_w - right_x), static_cast<uint16_t>(content_h)};
            }
            if (count > 0) {
                pixman_image_fill_rectangles(PIXMAN_OP_SRC, back_buf, &bg, count, borders);
            }
        }
        damage_region_.union_rect(0, 0, std::max(0, target_w), std::max(0, target_h));
    }

    if (content_w > 0 && content_h > 0) {
        const auto format = pixman_image_get_format(back_buf);
        const int bpp = std::max(1, static_cast<int>(PIXMAN_FORMAT_BPP(format) / 8));
        auto* pixels = reinterpret_cast<uint8_t*>(pixman_image_get_data(back_buf));
        const int stride = pixman_image_get_stride(back_buf);
        auto* content_bits = reinterpret_cast<uint32_t*>(
            pixels + static_cast<size_t>(padding_y_px_) * stride +
                static_cast<size_t>(padding_x_px_) * bpp);
        util::PixmanImage content_view(pixman_image_create_bits(
            format, content_w, content_h, content_bits, stride));
        util::PixmanRegion content_damage;
        renderer_.render(engine_, content_view.get(), &content_damage, full_redraw);
        if (!full_redraw && !content_damage.empty()) {
            content_damage.translate(padding_x_px_, padding_y_px_);
            damage_region_.union_region(content_damage);
        }
    }
    surface_->present(damage_region_);
    if (present_observer_) present_observer_();
}

void TerminalSession::run() {
    if (loop_) {
        loop_->run();
    }
}

} // namespace gritty::core
