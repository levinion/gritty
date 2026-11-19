#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
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
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        const std::string dir = std::string(xdg) + "/gritty/";
        for (const char* name : {"config.toml", "gritty.ini", "config.ini"}) {
            const std::string p = dir + name;
            if (std::filesystem::exists(p)) return p;
        }
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        const std::string dir = std::string(home) + "/.config/gritty/";
        for (const char* name : {"config.toml", "gritty.ini", "config.ini"}) {
            const std::string p = dir + name;
            if (std::filesystem::exists(p)) return p;
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
    const size_t cols = cli.cols ? cli.cols : (override_cols ? override_cols : 80);
    const size_t rows = cli.rows ? cli.rows : (override_rows ? override_rows : 24);

#if defined(PLATFORM_MACOS)
    platform::macos::CoreTextFinder font_discovery;
    auto match = font_discovery.find_font(platform::FontPattern{
        .family = cfg->font.family
    });

    std::string resolved_font_path;
    if (match) {
        resolved_font_path = match->path;
    } else {
        auto fallback = font_discovery.find_font(platform::FontPattern{.family = "Monaco"});
        if (fallback) resolved_font_path = fallback->path;
        else resolved_font_path = "/System/Library/Fonts/Monaco.ttf";
    }

    core::SessionConfig sess_cfg{
        .font_path = resolved_font_path,
        .font_size = cfg->font.size,
        .cols = static_cast<int>(cols),
        .rows = static_cast<int>(rows),
        .shell = cli.command.empty() ? cfg->main.shell : cli.command,
        .cwd = cli.working_directory,
        .args = cli.command_args,
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

    // Register fallback file paths only: FontEngine opens each face lazily on
    // the first glyph miss, so CJK/Emoji fonts cost no startup heap.
    for (const auto& fallback : font_discovery.fallback_chain(
             platform::FontPattern{.family = cfg->font.family})) {
        if (fallback.path != resolved_font_path) session->add_fallback_font(fallback.path);
    }

    surf_ptr->set_decorations(cfg->window.decorations);
    surf_ptr->show_window(cfg->main.title);
    session->render_frame(true);
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
