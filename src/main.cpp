#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <iostream>
#include <memory>
#include <pwd.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include "config/cli.hpp"
#include "config/toml_loader.hpp"
#include "core/quit_confirm.hpp"
#include "core/terminal_session.hpp"
#include "platform/single_instance.hpp"
#include "util/heap_trim.hpp"

#if defined(PLATFORM_MACOS)
#include "platform/macos/cocoa_surface.hpp"
#include "platform/macos/kqueue_loop.hpp"
#include "platform/macos/ct_discovery.hpp"
#include "platform/macos/clipboard.hpp"
#endif

using namespace gritty;

namespace {

std::string find_default_config() {
    std::error_code ec;
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        const std::string dir = std::string(xdg) + "/gritty/";
        for (const char* name : {"config.toml", "gritty.ini", "config.ini"}) {
            const std::string p = dir + name;
            if (std::filesystem::exists(p, ec) && !ec) return p;
        }
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        const std::string dir = std::string(home) + "/.config/gritty/";
        for (const char* name : {"config.toml", "gritty.ini", "config.ini"}) {
            const std::string p = dir + name;
            if (std::filesystem::exists(p, ec) && !ec) return p;
        }
    }
    return "";
}

std::string resolve_home_dir() {
    const char* env_home = std::getenv("HOME");
    if (env_home && *env_home) return std::string(env_home);
    if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir && *pw->pw_dir) {
        return std::string(pw->pw_dir);
    }
    return {};
}

std::string absolutize_path(std::string_view raw, const std::string& base_dir) {
    if (raw.empty()) return {};
    std::filesystem::path p(raw);
    if (p.is_absolute() || base_dir.empty()) return p.lexically_normal().string();
    return (std::filesystem::path(base_dir) / p).lexically_normal().string();
}

struct PreparedWindowConfig {
    std::shared_ptr<config::AppConfig> cfg;
    size_t cols{80};
    size_t rows{24};
    std::string shell_cmd;
    std::vector<std::string> shell_args;
    std::string resolved_cwd;
};

std::expected<PreparedWindowConfig, std::string> prepare_window_config(
    const platform::LaunchRequest& req,
    const std::string& home_dir
) {
    std::string config_path = req.config_path;
    if (config_path.empty()) config_path = find_default_config();

    std::shared_ptr<config::AppConfig> cfg;
    if (!config_path.empty()) {
        auto res = config::TomlLoader::load_from_file(config_path);
        if (!res) {
            return std::unexpected("Config error in " + config_path + ": " + res.error().message);
        }
        cfg = *res;
    } else {
        cfg = std::make_shared<config::AppConfig>();
    }

    size_t override_cols = 0;
    size_t override_rows = 0;
    for (const auto& expr : req.config_overrides) {
        auto result = cfg->apply_override(expr, &override_cols, &override_rows);
        if (!result) return std::unexpected(result.error());
    }

    if (!req.font_family.empty()) cfg->font.family = req.font_family;
    if (req.font_size > 0) cfg->font.size = req.font_size;
    if (!req.title.empty()) cfg->main.title = req.title;
    if (!req.app_id.empty()) cfg->main.app_id = req.app_id;

    PreparedWindowConfig out;
    out.cols = req.cols ? req.cols : (override_cols ? override_cols : cfg->window.columns);
    out.rows = req.rows ? req.rows : (override_rows ? override_rows : cfg->window.lines);

    if (!req.command.empty()) {
        out.shell_cmd = req.command;
        out.shell_args = req.command_args;
    } else if (!cfg->main.shell.empty()) {
        std::error_code ec;
        if (!cfg->main.shell_args.empty() ||
            (std::filesystem::exists(cfg->main.shell, ec) && !ec)) {
            out.shell_cmd = cfg->main.shell;
            out.shell_args = cfg->main.shell_args;
        } else {
            auto split = config::split_shell_command(cfg->main.shell);
            out.shell_cmd = std::move(split.executable);
            out.shell_args = std::move(split.args);
        }
    }

    const std::string_view explicit_cwd = !req.explicit_cwd.empty()
        ? std::string_view(req.explicit_cwd)
        : std::string_view(cfg->main.working_directory);
    out.resolved_cwd = config::resolve_working_directory(explicit_cwd, req.inherited_cwd, home_dir);
    if (out.resolved_cwd.empty() && !req.inherited_cwd.empty() && req.inherited_cwd != "/") {
        out.resolved_cwd = req.inherited_cwd;
    } else if (out.resolved_cwd.empty()) {
        out.resolved_cwd = home_dir;
    }

    out.cfg = std::move(cfg);
    return out;
}

#if defined(PLATFORM_MACOS)

// Coordinates independent windows, deferred close teardown, and Cmd+Q confirmation
// under a single application run loop.
class AppController final {
public:
    AppController(platform::SingleInstanceServer* server, std::string home_dir)
        : server_(server), home_dir_(std::move(home_dir)) {}

    ~AppController() {
        cancel_quit_timer();
        if (server_) server_->stop();
        windows_.clear();
        platform::macos::shutdown_cocoa_application();
    }

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    platform::macos::KqueueRunLoop& loop() noexcept { return loop_; }

    std::expected<void, std::string> open_window(const platform::LaunchRequest& req) {
        auto prepared = prepare_window_config(req, home_dir_);
        if (!prepared) return std::unexpected(prepared.error());
        const auto& cfg = prepared->cfg;

        auto match = font_discovery_.find_font(platform::FontPattern{.family = cfg->font.family});
        std::string resolved_family = cfg->font.family;
        std::string resolved_font_path;
        std::string resolved_postscript;
        if (match) {
            resolved_font_path = match->path;
            resolved_postscript = match->postscript_name;
        } else {
            for (const char* family : {"Menlo", "Monaco"}) {
                if (auto fallback = font_discovery_.find_font(platform::FontPattern{.family = family})) {
                    resolved_family = family;
                    resolved_font_path = fallback->path;
                    resolved_postscript = fallback->postscript_name;
                    break;
                }
            }
            if (resolved_font_path.empty()) {
                std::error_code ec;
                if (std::filesystem::exists("/System/Library/Fonts/Menlo.ttc", ec) && !ec) {
                    resolved_family = "Menlo";
                    resolved_font_path = "/System/Library/Fonts/Menlo.ttc";
                } else {
                    resolved_family = "Monaco";
                    resolved_font_path = "/System/Library/Fonts/Monaco.ttf";
                }
            }
        }

        core::SessionConfig sess_cfg{
            .font_path = resolved_font_path,
            .font_postscript_name = resolved_postscript,
            .font_size = cfg->font.size,
            .cols = static_cast<int>(prepared->cols),
            .rows = static_cast<int>(prepared->rows),
            .shell = prepared->shell_cmd,
            .term = cfg->main.term,
            .cwd = prepared->resolved_cwd,
            .args = prepared->shell_args,
            .env = cfg->main.env,
            .font_dpi = cfg->font.dpi,
            .padding_x = cfg->window.padding.x,
            .padding_y = cfg->window.padding.y,
            .antialias = cfg->font.antialias,
            .render_config = {
                .default_fg = cfg->colors.foreground,
                .default_bg = cfg->effective_background_argb(),
                .cursor_color = cfg->cursor.color
            },
            .palette = cfg->colors.palette,
            .osc52 = cfg->main.osc52,
            .hold_on_exit = req.hold,
        };

        auto surface = std::make_unique<platform::macos::CocoaLayerSurface>(0, 0);
        auto clipboard = std::make_unique<platform::macos::PasteboardClipboard>();
        platform::macos::CocoaLayerSurface* surf_ptr = surface.get();

        auto session = std::make_unique<core::TerminalSession>(
            sess_cfg,
            std::move(surface),
            &loop_,
            std::move(clipboard)
        );
        session->engine().set_cursor_shape(cfg->cursor.style, cfg->cursor.blink);

        if (!session->start()) {
            return std::unexpected("Failed to start terminal session.");
        }

        const uint64_t id = next_window_id_++;
        core::TerminalSession* sess_ptr = session.get();
        session->set_exit_callback([this, id] { schedule_close(id); });
        surf_ptr->set_quit_request_callback([this, id] { handle_quit_request(id); });
        surf_ptr->set_new_window_callback([this] { (void)open_default_window(); });
        surf_ptr->set_activity_callback([this] { disarm_quit_confirm(); });
        surf_ptr->set_focus_callback([this, id, sess_ptr](bool focused) {
            sess_ptr->on_window_focus(focused);
            on_window_focus(id, focused);
        });

        surf_ptr->set_decorations(cfg->window.decorations);
        surf_ptr->show_window(cfg->main.title);
        session->render_frame(true);

        for (const bool bold : {true, false}) {
            for (const bool italic : {true, false}) {
                if (!bold && !italic) continue;
                if (auto style_match = font_discovery_.find_font(platform::FontPattern{
                        .family = resolved_family, .bold = bold, .italic = italic})) {
                    if (style_match->path != resolved_font_path ||
                        style_match->postscript_name != resolved_postscript) {
                        session->set_style_font(bold, italic, style_match->path,
                                                style_match->postscript_name);
                    }
                }
            }
        }
        for (const auto& fallback : font_discovery_.fallback_chain(
                 platform::FontPattern{.family = resolved_family})) {
            if (fallback.path != resolved_font_path ||
                fallback.postscript_name != resolved_postscript) {
                session->add_fallback_font(fallback.path, fallback.postscript_name);
            }
        }

        auto entry = std::make_unique<WindowEntry>();
        entry->id = id;
        entry->confirm_quit = cfg->window.confirm_quit;
        entry->surface = surf_ptr;
        entry->session = std::move(session);
        windows_.push_back(std::move(entry));
        focused_window_id_ = id;
        return {};
    }

    std::expected<void, std::string> open_default_window() {
        platform::LaunchRequest req;
        req.inherited_cwd = home_dir_;
        return open_window(req);
    }

    void handle_quit_request(uint64_t requester_id) {
        if (quitting_) return;
        if (windows_.empty()) {
            schedule_quit_all();
            return;
        }
        WindowEntry* target = find_window(requester_id);
        if (!target) target = find_window(focused_window_id_);
        if (!target) target = windows_.back().get();

        const auto action = quit_guard_.on_quit_requested(
            target->confirm_quit,
            windows_.size(),
            core::QuitConfirmGuard::Clock::now()
        );
        if (action == core::QuitAction::QuitNow) {
            schedule_quit_all();
            return;
        }

        if (banner_window_id_ != 0 && banner_window_id_ != target->id) {
            if (WindowEntry* prev = find_window(banner_window_id_)) {
                prev->session->set_status_banner({});
            }
        }
        banner_window_id_ = target->id;
        std::string msg = " Press Cmd+Q again to quit ";
        if (windows_.size() > 1) {
            msg = " Press Cmd+Q again to quit (" + std::to_string(windows_.size()) + " windows) ";
        }
        target->session->set_status_banner(msg);
        target->surface->focus_window();

        loop_.set_scoped_timer(
            controller_timer_owner(),
            platform::IEventLoop::TimerId::Default,
            static_cast<int>(quit_guard_.timeout().count()),
            [this] { disarm_quit_confirm(); }
        );
    }

    void handle_reopen() {
        if (quitting_) return;
        if (windows_.empty()) {
            (void)open_default_window();
            return;
        }
        if (WindowEntry* target = find_window(focused_window_id_)) {
            target->surface->focus_window();
        } else {
            windows_.back()->surface->focus_window();
        }
    }

    void disarm_quit_confirm() {
        const bool had_banner = banner_window_id_ != 0;
        const bool was_armed = quit_guard_.armed();
        if (!had_banner && !was_armed) return;
        quit_guard_.disarm();
        cancel_quit_timer();
        if (WindowEntry* target = find_window(banner_window_id_)) {
            target->session->set_status_banner({});
        }
        banner_window_id_ = 0;
    }

private:
    struct WindowEntry {
        uint64_t id{0};
        config::QuitConfirmation confirm_quit{config::QuitConfirmation::Needed};
        platform::macos::CocoaLayerSurface* surface{nullptr};
        std::unique_ptr<core::TerminalSession> session;
    };

    [[nodiscard]] uintptr_t controller_timer_owner() const noexcept {
        return reinterpret_cast<uintptr_t>(this);
    }

    void cancel_quit_timer() {
        loop_.cancel_scoped_timer(controller_timer_owner(), platform::IEventLoop::TimerId::Default);
    }

    WindowEntry* find_window(uint64_t id) noexcept {
        if (id == 0) return nullptr;
        for (auto& entry : windows_) {
            if (entry->id == id) return entry.get();
        }
        return nullptr;
    }

    void on_window_focus(uint64_t id, bool focused) {
        if (focused && focused_window_id_ != id) {
            focused_window_id_ = id;
            if (banner_window_id_ != 0 && banner_window_id_ != id) {
                disarm_quit_confirm();
            }
        }
    }

    void schedule_close(uint64_t id) {
        // Defer teardown out of active PTY/AppKit callbacks.
        if (quitting_) return;
        if (std::find(pending_close_ids_.begin(), pending_close_ids_.end(), id) ==
            pending_close_ids_.end()) {
            pending_close_ids_.push_back(id);
        }
        if (close_flush_scheduled_) return;
        close_flush_scheduled_ = true;
        loop_.post([this] { flush_closed_windows(); });
    }

    void flush_closed_windows() {
        close_flush_scheduled_ = false;
        if (quitting_) {
            pending_close_ids_.clear();
            return;
        }
        auto to_close = std::move(pending_close_ids_);
        pending_close_ids_.clear();

        for (const uint64_t id : to_close) {
            if (banner_window_id_ == id) disarm_quit_confirm();
            if (focused_window_id_ == id) focused_window_id_ = 0;
            std::erase_if(windows_, [id](const std::unique_ptr<WindowEntry>& entry) {
                return entry->id == id;
            });
        }
        if (windows_.size() <= 1 && quit_guard_.armed()) {
            if (WindowEntry* remaining = windows_.empty() ? nullptr : windows_.front().get();
                remaining && remaining->confirm_quit == config::QuitConfirmation::Needed) {
                disarm_quit_confirm();
            }
        }
        if (focused_window_id_ == 0 && !windows_.empty()) {
            focused_window_id_ = windows_.back()->id;
        }
        if (windows_.empty()) {
            quit_all();
        }
    }

    void schedule_quit_all() {
        if (quitting_ || quit_scheduled_) return;
        quit_scheduled_ = true;
        loop_.post([this] { quit_all(); });
    }

    void quit_all() {
        if (quitting_) return;
        quitting_ = true;
        disarm_quit_confirm();
        if (server_) server_->stop();
        windows_.clear();
        loop_.stop();
    }

    platform::SingleInstanceServer* server_{nullptr};
    std::string home_dir_;
    platform::macos::KqueueRunLoop loop_;
    platform::macos::CoreTextFinder font_discovery_;
    std::vector<std::unique_ptr<WindowEntry>> windows_;
    core::QuitConfirmGuard quit_guard_;
    uint64_t next_window_id_{1};
    uint64_t focused_window_id_{0};
    uint64_t banner_window_id_{0};
    std::vector<uint64_t> pending_close_ids_;
    bool close_flush_scheduled_{false};
    bool quit_scheduled_{false};
    bool quitting_{false};
};

#endif

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);

    const auto parsed_cli = config::parse_cli(argc, argv);
    if (!parsed_cli) {
        std::cerr << "gritty: " << parsed_cli.error() << "\n";
        return 1;
    }
    const auto& cli = parsed_cli.value();
    if (cli.action == config::CliAction::Help) {
        std::cout << config::format_cli_help(argv[0]);
        return 0;
    }
    if (cli.action == config::CliAction::Version) {
        std::cout << config::format_cli_version();
        return 0;
    }

#if defined(PLATFORM_MACOS)
    std::error_code cwd_ec;
    const std::string inherited_cwd = std::filesystem::current_path(cwd_ec).string();
    const std::string home_dir = resolve_home_dir();

    platform::LaunchRequest initial_req{
        .config_path = absolutize_path(cli.config_path, inherited_cwd),
        .explicit_cwd = absolutize_path(cli.working_directory, inherited_cwd),
        .inherited_cwd = inherited_cwd,
        .title = cli.title,
        .app_id = cli.app_id,
        .font_family = cli.font_family,
        .font_size = cli.font_size,
        .cols = cli.cols,
        .rows = cli.rows,
        .hold = cli.hold,
        .config_overrides = cli.config_overrides,
        .command = cli.command,
        .command_args = cli.command_args,
    };

    if (auto validated = prepare_window_config(initial_req, home_dir); !validated) {
        std::cerr << validated.error() << "\n";
        return 1;
    }

    // Resolve/forward before NSApplication init so secondary launches exit
    // cleanly without creating an extra Dock icon.
    platform::SingleInstanceServer server;
    platform::SingleInstanceServer* active_server = nullptr;
    if (!cli.standalone) {
        const std::string socket_path = platform::default_instance_socket_path();
        std::string ipc_error;
        const auto status = server.acquire_or_forward(socket_path, initial_req, &ipc_error);
        switch (status) {
            case platform::SingleInstanceServer::AcquireStatus::ForwardedOk:
                return 0;
            case platform::SingleInstanceServer::AcquireStatus::ForwardedError:
                std::cerr << "gritty: " << ipc_error << "\n";
                return 1;
            case platform::SingleInstanceServer::AcquireStatus::BecamePrimary:
                active_server = &server;
                break;
            case platform::SingleInstanceServer::AcquireStatus::Failed:
                std::cerr << "gritty: warning: single-instance IPC unavailable ("
                          << ipc_error << "); running standalone\n";
                break;
        }
    }

    AppController controller(active_server, home_dir);
    platform::macos::initialize_cocoa_application({
        .on_reopen_no_windows = [&controller] { controller.handle_reopen(); },
        .on_quit_requested = [&controller] { controller.handle_quit_request(0); },
    });

    if (active_server) {
        active_server->attach(controller.loop(),
            [&controller](const platform::LaunchRequest& req) {
                return controller.open_window(req);
            });
    }

    if (auto launched = controller.open_window(initial_req); !launched) {
        std::cerr << launched.error() << "\n";
        return 1;
    }

    // Config parsing and font discovery allocate short-lived heap pages.
    // Trim once here, never on a rendering, input or PTY path.
    util::trim_transient_heap();

    std::cout << "Gritty Terminal running.\n";
    controller.loop().run();
    return 0;
#else
    std::cerr << "Linux window backend is not implemented yet.\n";
    return 1;
#endif
}
