#pragma once

#include <string>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include "core/terminal_engine.hpp"
#include "render/font_engine.hpp"

namespace gritty::config {

struct MainConfig {
    std::string shell;
    std::string term{"xterm-256color"};
    std::string title{"Gritty"};
};

struct FontConfig {
    std::string family;
    double size{13.0};
    unsigned int dpi{96};
    render::AntialiasMode antialias{render::AntialiasMode::Grayscale};
};

struct CursorConfig {
    core::CursorShape style{core::CursorShape::Block};
    bool blink{false};
    uint32_t color{0xffffffff};
};

struct ColorsConfig {
    uint32_t foreground{0xffdddddd};
    uint32_t background{0xff111111};
    uint32_t cursor{0xffffffff};
    std::optional<uint32_t> selection_fg;
    std::optional<uint32_t> selection_bg;
    std::array<uint32_t, 256> palette{core::kDefaultPalette};
    bool palette_customized{false};
    double alpha{1.0}; // Default background only; text/explicit SGR backgrounds stay opaque.
    [[nodiscard]] uint32_t background_argb() const noexcept;
};

struct ScrollbackConfig {
    size_t lines{2000};
    int multiplier{3};
};

struct AppConfig {
    MainConfig main;
    FontConfig font;
    CursorConfig cursor;
    ColorsConfig colors;
    ScrollbackConfig scrollback;

    AppConfig();
};

class ConfigStore final {
public:
    ConfigStore();
    explicit ConfigStore(std::shared_ptr<const AppConfig> initial);

    [[nodiscard]] std::shared_ptr<const AppConfig> get() const noexcept {
        return config_;
    }

    void update(std::shared_ptr<const AppConfig> new_config) noexcept {
        config_ = std::move(new_config);
    }

private:
    std::shared_ptr<const AppConfig> config_;
};

} // namespace gritty::config
