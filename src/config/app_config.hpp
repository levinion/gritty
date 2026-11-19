#pragma once

#include <string>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>
#include <memory>
#include <optional>
#include "core/terminal_engine.hpp"
#include "platform/pal_interfaces.hpp"
#include "render/font_engine.hpp"

namespace gritty::config {

// foot-compatible OSC 52 security policy. Copy-only is the default: a child
// may write/clear the clipboard, but read queries are silently ignored.
enum class Osc52Mode : uint8_t {
    Disabled = 0,
    CopyOnly = 1,
    PasteOnly = 2,
    Enabled = 3,
};

// Case-insensitive parser shared by TOML and CLI overrides.
[[nodiscard]] std::optional<Osc52Mode> parse_osc52_mode(std::string_view value) noexcept;

struct MainConfig {
    std::string shell;
    std::string term{"xterm-256color"};
    std::string title{"Gritty"};
    std::string app_id{"gritty"};
    Osc52Mode osc52{Osc52Mode::CopyOnly};
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
    std::array<uint32_t, 256> palette{core::kDefaultPalette};
    bool palette_customized{false};
    double alpha{1.0}; // Default background only; text/explicit SGR backgrounds stay opaque.
    [[nodiscard]] uint32_t background_argb() const noexcept;
};

struct WindowPadding {
    int x{0}; // Logical pixels/points, applied on both left and right.
    int y{0}; // Logical pixels/points, applied on both top and bottom.
};

struct WindowConfig {
    platform::WindowDecorations decorations{platform::WindowDecorations::Full};
    WindowPadding padding;
};

// Case-insensitive parser shared by TOML, CLI overrides and tests.
[[nodiscard]] std::optional<platform::WindowDecorations> parse_window_decorations(
    std::string_view value
) noexcept;

struct AppConfig {
    MainConfig main;
    FontConfig font;
    CursorConfig cursor;
    ColorsConfig colors;
    WindowConfig window;

    AppConfig();

    // Full native decorations imply an opaque window: alpha < 1 is ignored so
    // the titlebar and terminal content cannot become see-through by accident.
    // Other decoration modes keep the configured per-pixel background alpha.
    [[nodiscard]] uint32_t effective_background_argb() const noexcept;

    [[nodiscard]] std::expected<void, std::string> apply_override(
        std::string_view expr,
        size_t* cols_out = nullptr,
        size_t* rows_out = nullptr
    );
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
