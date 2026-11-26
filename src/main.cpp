#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <pwd.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "config/cli.hpp"
#include "config/toml_loader.hpp"
#include "core/terminal_session.hpp"
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

} // namespace

int main(int argc, char* argv[]) {
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

    std::string config_path = cli.config_path;
    if (config_path.empty()) config_path = find_default_config();

    std::shared_ptr<config::AppConfig> cfg;
    if (!config_path.empty()) {
        auto res = config::TomlLoader::load_from_file(config_path);
        if (!res) {
            std::cerr << "Config error in " << config_path << ": " << res.error().message << "\n";
            return 1;
        }
        cfg = *res;
    } else {
        cfg = std::make_shared<config::AppConfig>();
    }

    size_t override_cols = 0;
    size_t override_rows = 0;
    for (const auto& expr : cli.config_overrides) {
        auto result = cfg->apply_override(expr, &override_cols, &override_rows);
        if (!result) {
            std::cerr << "gritty: " << result.error() << "\n";
            return 1;
        }
    }

    if (!cli.font_family.empty()) cfg->font.family = cli.font_family;
    if (cli.font_size > 0) cfg->font.size = cli.font_size;
    if (!cli.title.empty()) cfg->main.title = cli.title;
    if (!cli.app_id.empty()) cfg->main.app_id = cli.app_id;
    const size_t cols = cli.cols ? cli.cols : (override_cols ? override_cols : cfg->window.columns);
    const size_t rows = cli.rows ? cli.rows : (override_rows ? override_rows : cfg->window.lines);

#if defined(PLATFORM_MACOS)
    platform::macos::CoreTextFinder font_discovery;
    auto match = font_discovery.find_font(platform::FontPattern{
        .family = cfg->font.family
    });

    std::string resolved_family = cfg->font.family;
    std::string resolved_font_path;
    std::string resolved_postscript;
    if (match) {
        resolved_font_path = match->path;
        resolved_postscript = match->postscript_name;
    } else {
        for (const char* family : {"Menlo", "Monaco"}) {
            if (auto fallback = font_discovery.find_font(platform::FontPattern{.family = family})) {
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

    std::string shell_cmd;
    std::vector<std::string> shell_args;
    if (!cli.command.empty()) {
        shell_cmd = cli.command;
        shell_args = cli.command_args;
    } else if (!cfg->main.shell.empty()) {
        std::error_code ec;
        if (!cfg->main.shell_args.empty() ||
            (std::filesystem::exists(cfg->main.shell, ec) && !ec)) {
            shell_cmd = cfg->main.shell;
            shell_args = cfg->main.shell_args;
        } else {
            auto split = config::split_shell_command(cfg->main.shell);
            shell_cmd = std::move(split.executable);
            shell_args = std::move(split.args);
        }
    }

    std::error_code cwd_ec;
    const std::string inherited_cwd = std::filesystem::current_path(cwd_ec).string();
    const char* env_home = std::getenv("HOME");
    std::string home_dir = (env_home && *env_home) ? std::string(env_home) : std::string{};
    if (home_dir.empty()) {
        if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir && *pw->pw_dir) {
            home_dir = pw->pw_dir;
        }
    }
    const std::string_view explicit_cwd = !cli.working_directory.empty()
        ? std::string_view(cli.working_directory)
        : std::string_view(cfg->main.working_directory);
    const std::string resolved_cwd =
        config::resolve_working_directory(explicit_cwd, inherited_cwd, home_dir);

    core::SessionConfig sess_cfg{
        .font_path = resolved_font_path,
        .font_postscript_name = resolved_postscript,
        .font_size = cfg->font.size,
        .cols = static_cast<int>(cols),
        .rows = static_cast<int>(rows),
        .shell = std::move(shell_cmd),
        .term = cfg->main.term,
        .cwd = resolved_cwd,
        .args = std::move(shell_args),
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
        .hold_on_exit = cli.hold,
    };

    // Start with no IOSurface: TerminalSession::start() loads the font and
    // resizes to the exact grid geometry, so an approximate startup window
    // would allocate and then discard a full-size framebuffer.
    auto surface = std::make_unique<platform::macos::CocoaLayerSurface>(0, 0);
    auto loop = std::make_unique<platform::macos::KqueueRunLoop>();
    auto clipboard = std::make_unique<platform::macos::PasteboardClipboard>();

    platform::macos::CocoaLayerSurface* surf_ptr = surface.get();

    auto session = std::make_unique<core::TerminalSession>(
        sess_cfg,
        std::move(surface),
        std::move(loop),
        std::move(clipboard)
    );

    session->engine().set_cursor_shape(cfg->cursor.style, cfg->cursor.blink);

    if (!session->start()) {
        std::cerr << "Failed to start terminal session.\n";
        return 1;
    }

    surf_ptr->set_decorations(cfg->window.decorations);
    surf_ptr->show_window(cfg->main.title);
    session->render_frame(true);

    // Register real Bold / Italic / Bold-Italic faces and fallbacks lazily after
    // the first frame is presented so CoreText cascade queries never stall launch.
    for (const bool bold : {true, false}) {
        for (const bool italic : {true, false}) {
            if (!bold && !italic) continue;
            if (auto style_match = font_discovery.find_font(platform::FontPattern{
                    .family = resolved_family, .bold = bold, .italic = italic})) {
                if (style_match->path != resolved_font_path ||
                    style_match->postscript_name != resolved_postscript) {
                    session->set_style_font(bold, italic, style_match->path,
                                            style_match->postscript_name);
                }
            }
        }
    }
    for (const auto& fallback : font_discovery.fallback_chain(
             platform::FontPattern{.family = resolved_family})) {
        if (fallback.path != resolved_font_path ||
            fallback.postscript_name != resolved_postscript) {
            session->add_fallback_font(fallback.path, fallback.postscript_name);
        }
    }
    // Config parsing and font discovery allocate short-lived heap pages.
    // Trim once here, never on a rendering, input or PTY path.
    util::trim_transient_heap();

    std::cout << "Gritty Terminal running.\n";
    std::cout << "Using font: " << resolved_font_path << " (" << cfg->font.size << "pt)\n";

    session->run();
    return 0;
#else
    std::cerr << "Linux window backend is not implemented yet.\n";
    return 1;
#endif
}
