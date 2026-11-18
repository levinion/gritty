#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdlib>
#include <cmath>
#include <stdexcept>

#include "config/toml_loader.hpp"
#include "core/terminal_session.hpp"

#if defined(PLATFORM_MACOS)
#include "platform/macos/cocoa_surface.hpp"
#include "platform/macos/kqueue_loop.hpp"
#include "platform/macos/ct_discovery.hpp"
#include "platform/macos/clipboard.hpp"
#endif

using namespace gritty;

namespace {

void print_help(const char* prog) {
    std::cout << "Usage: " << prog << " [options] [command [args...]]\n\n"
              << "Options:\n"
              << "  -c, --config <path>    Path to config.toml\n"
              << "  -f, --font <family>    Font family name\n"
              << "  -s, --size <pt>        Font size in points\n"
              << "  -t, --title <title>    Window title\n"
              << "  --cols <n>             Initial terminal columns\n"
              << "  --rows <n>             Initial terminal rows\n"
              << "  -v, --version          Print version\n"
              << "  -e, --exec <command>   Execute command instead of the configured shell\n"
              << "  -h, --help             Show this help\n";
}

std::string find_default_config() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        std::string p = std::string(xdg) + "/gritty/config.toml";
        if (std::filesystem::exists(p)) return p;
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        std::string p = std::string(home) + "/.config/gritty/config.toml";
        if (std::filesystem::exists(p)) return p;
    }
    return "";
}

} // namespace

int main(int argc, char* argv[]) {
    std::string config_path;
    std::string font_override;
    double font_size_override = 0;
    std::string title_override;
    size_t cols_override = 0;
    size_t rows_override = 0;
    std::string command;
    std::vector<std::string> command_args;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if ((arg == "-c" || arg == "--config" || arg == "-f" || arg == "--font" ||
             arg == "-s" || arg == "--size" || arg == "-t" || arg == "--title" ||
             arg == "--cols" || arg == "--rows" || arg == "-e" || arg == "--exec") &&
            i + 1 >= argc) {
            std::cerr << "Missing value for " << arg << "\n";
            return 1;
        }
        if (arg == "-h" || arg == "--help") {
            print_help(argv[0]);
            return 0;
        } else if (arg == "-v" || arg == "--version") {
            std::cout << "Gritty 0.1.0 (foot parity + Kitty protocols)\n";
            return 0;
        } else if (arg == "-c" || arg == "--config") {
            if (++i < argc) config_path = argv[i];
        } else if (arg == "-f" || arg == "--font") {
            if (++i < argc) font_override = argv[i];
        } else if (arg == "-s" || arg == "--size") {
            try {
                const std::string value = argv[++i];
                size_t used = 0;
                font_size_override = std::stod(value, &used);
                if (used != value.size() || !std::isfinite(font_size_override) ||
                    font_size_override < 1 || font_size_override > 200)
                    throw std::invalid_argument("font size out of range");
            } catch (const std::exception&) {
                std::cerr << "Invalid font size (expected 1..200 points)\n";
                return 1;
            }
        } else if (arg == "-t" || arg == "--title") {
            if (++i < argc) title_override = argv[i];
        } else if (arg == "--cols" || arg == "--rows") {
            try {
                const std::string value = argv[++i];
                size_t used = 0;
                const long parsed = std::stol(value, &used);
                if (used != value.size() || parsed < 1 || parsed > 1000)
                    throw std::invalid_argument("geometry out of range");
                if (arg == "--cols") cols_override = static_cast<size_t>(parsed);
                else rows_override = static_cast<size_t>(parsed);
            } catch (const std::exception&) {
                std::cerr << "Invalid " << arg << " (expected 1..1000)\n";
                return 1;
            }
        } else if (arg == "-e" || arg == "--exec") {
            if (++i < argc) {
                command = argv[i];
                for (int j = i + 1; j < argc; ++j) command_args.push_back(argv[j]);
                break;
            }
        } else if (arg == "--" || !arg.starts_with('-')) {
            if (arg == "--") {
                if (++i >= argc) break;
                arg = argv[i];
            }
            command = arg;
            for (int j = i + 1; j < argc; ++j) {
                command_args.push_back(argv[j]);
            }
            break;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return 1;
        }
    }

    if (config_path.empty()) {
        config_path = find_default_config();
    }

    std::shared_ptr<config::AppConfig> cfg;
    if (!config_path.empty()) {
        auto res = config::TomlLoader::load_from_file(config_path);
        if (res) {
            cfg = *res;
        } else {
            std::cerr << "Config error in " << config_path << ": " << res.error().message << "\n";
            return 1;
        }
    } else {
        cfg = std::make_shared<config::AppConfig>();
    }

    if (!font_override.empty()) cfg->font.family = font_override;
    if (font_size_override > 0) cfg->font.size = font_size_override;
    if (!title_override.empty()) cfg->main.title = title_override;

#if defined(PLATFORM_MACOS)
    platform::macos::CoreTextFinder font_discovery;
    auto match = font_discovery.find_font(platform::FontPattern{
        .family = cfg->font.family
    });

    const size_t cols = cols_override ? cols_override : 80;
    const size_t rows = rows_override ? rows_override : 24;

    std::string resolved_font_path;
    if (match) {
        resolved_font_path = match->path;
    } else {
        // Fallback to Monaco or Menlo
        auto fallback = font_discovery.find_font(platform::FontPattern{.family = "Monaco"});
        if (fallback) resolved_font_path = fallback->path;
        else resolved_font_path = "/System/Library/Fonts/Monaco.ttf";
    }

    core::SessionConfig sess_cfg{
        .font_path = resolved_font_path,
        .font_size = cfg->font.size,
        .cols = static_cast<int>(cols),
        .rows = static_cast<int>(rows),
        .shell = command.empty() ? cfg->main.shell : command,
        .cwd = "",
        .args = command_args,
        .font_dpi = cfg->font.dpi,
        .antialias = cfg->font.antialias,
        .render_config = {
            .default_fg = cfg->colors.foreground,
            .default_bg = cfg->colors.background_argb(),
            .cursor_color = cfg->cursor.color
        },
        .scrollback_lines = cfg->scrollback.lines,
        .palette = cfg->colors.palette
    };

    const double point_size = font_size_override > 0 ? font_size_override : cfg->font.size;
    const int initial_w = static_cast<int>(std::ceil(static_cast<double>(cols) * point_size * 0.65));
    const int initial_h = static_cast<int>(std::ceil(static_cast<double>(rows) * point_size * 1.30));
    auto surface = std::make_unique<platform::macos::CocoaLayerSurface>(initial_w, initial_h);
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

    surf_ptr->show_window(cfg->main.title);
    session->render_frame(true);

    std::cout << "Gritty Terminal running.\n";
    std::cout << "Using font: " << resolved_font_path << " (" << cfg->font.size << "pt)\n";

    session->run();

    return 0;
#else
    std::cerr << "Linux window backend is not implemented yet.\n";
    return 1;
#endif
}
