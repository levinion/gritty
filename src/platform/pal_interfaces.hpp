#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <functional>
#include <pixman.h>
#include "util/raii.hpp"
#include "input/input_events.hpp"

namespace gritty::platform {

// Window surface interface (WaylandShmSurface or CocoaLayerSurface)
class IWindowSurface {
public:
    virtual ~IWindowSurface() = default;

    virtual void resize(int pixel_width, int pixel_height, double scale) = 0;
    virtual pixman_image_t* get_back_buffer() = 0;
    virtual void present(const util::PixmanRegion& damage) = 0;
    virtual void set_title(const std::string& title) { (void)title; }
    virtual void set_transparent(bool transparent) { (void)transparent; }

    [[nodiscard]] virtual int pixel_width() const noexcept = 0;
    [[nodiscard]] virtual int pixel_height() const noexcept = 0;
    [[nodiscard]] virtual double scale() const noexcept = 0;

    using KeyCallback = std::function<void(const input::KeyEvent&)>;
    using MouseCallback = std::function<void(const input::MouseEvent&)>;
    using ResizeCallback = std::function<void(int px_w, int px_h, double scale)>;
    using CloseCallback = std::function<void()>;

    virtual void set_key_callback(KeyCallback cb) { (void)cb; }
    virtual void set_mouse_callback(MouseCallback cb) { (void)cb; }
    virtual void set_resize_callback(ResizeCallback cb) { (void)cb; }
    virtual void set_close_callback(CloseCallback cb) { (void)cb; }
};

// Event loop interface (EpollEventLoop or KqueueRunLoop)
class IEventLoop {
public:
    virtual ~IEventLoop() = default;

    static constexpr uint32_t EventReadable = 1 << 0;
    static constexpr uint32_t EventWritable = 1 << 1;
    static constexpr uint32_t EventError    = 1 << 2;

    using FdCallback = std::function<void(int fd, uint32_t events)>;

    virtual void add_fd(int fd, uint32_t events, FdCallback cb) = 0;
    virtual void remove_fd(int fd) = 0;
    // One-shot session watchdog. A nonpositive delay cancels it.
    virtual void set_timer(int delay_ms, std::function<void()> callback) = 0;
    virtual void run() = 0;
    virtual void stop() = 0;
    virtual void wake() = 0;
};

// Font discovery interface (FontconfigFinder or CoreTextFinder)
struct FontPattern {
    std::string family;
    bool bold{false};
    bool italic{false};
};

struct FontMatch {
    std::string path;
    int face_index{0};
};

class IFontDiscovery {
public:
    virtual ~IFontDiscovery() = default;

    virtual std::optional<FontMatch> find_font(const FontPattern& pattern) = 0;
    virtual std::vector<FontMatch> fallback_chain(const FontPattern& pattern) = 0;
};

// Clipboard interface (WaylandClipboard or PasteboardClipboard)
class IClipboard {
public:
    virtual ~IClipboard() = default;

    virtual void copy_text(std::string_view text) = 0;
    virtual std::string paste_text() = 0;
};

} // namespace gritty::platform
