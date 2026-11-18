#pragma once

#include <string>
#include <vector>
#include <pixman.h>
#include "input/input_events.hpp"
#include "render/font_engine.hpp"

// Search/URL overlays are deliberately standalone: tmux owns these UI flows
// in the supported configuration. They remain composable for optional local
// use but are not wired into TerminalSession by default.

namespace gritty::core {

class IOverlay {
public:
    virtual ~IOverlay() = default;

    virtual void activate() = 0;
    virtual void deactivate() = 0;
    [[nodiscard]] virtual bool is_active() const noexcept = 0;

    virtual bool handle_key(const input::KeyEvent& ev) = 0;
    virtual void render(
        pixman_image_t* dst,
        render::FontEngine& font_engine,
        int screen_cols,
        int screen_rows
    ) = 0;
};

class SearchOverlay final : public IOverlay {
public:
    SearchOverlay() = default;

    void activate() override { active_ = true; query_.clear(); }
    void deactivate() override { active_ = false; query_.clear(); }
    [[nodiscard]] bool is_active() const noexcept override { return active_; }

    [[nodiscard]] const std::string& query() const noexcept { return query_; }

    bool handle_key(const input::KeyEvent& ev) override;
    void render(
        pixman_image_t* dst,
        render::FontEngine& font_engine,
        int screen_cols,
        int screen_rows
    ) override;

private:
    bool active_{false};
    std::string query_;
};

struct UrlHint {
    int col{0};
    int row{0};
    std::string hint;
    std::string url;
};

class UrlOverlay final : public IOverlay {
public:
    UrlOverlay() = default;

    void activate() override { active_ = true; input_tag_.clear(); }
    void deactivate() override { active_ = false; input_tag_.clear(); }
    [[nodiscard]] bool is_active() const noexcept override { return active_; }

    void add_hint(int col, int row, std::string hint, std::string url);
    void clear_hints() noexcept { hints_.clear(); }

    bool handle_key(const input::KeyEvent& ev) override;
    void render(
        pixman_image_t* dst,
        render::FontEngine& font_engine,
        int screen_cols,
        int screen_rows
    ) override;

private:
    bool active_{false};
    std::string input_tag_;
    std::vector<UrlHint> hints_;
};

} // namespace gritty::core
