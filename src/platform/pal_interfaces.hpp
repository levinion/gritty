#pragma once

#include <cstdint>
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
    // Return an exclusively writable buffer with the last presented pixels as
    // its baseline. nullptr means compositor backpressure; keep pending damage.
    // A frame-clock tick alone never grants write access to an attached buffer.
    virtual pixman_image_t* get_back_buffer() = 0;
    virtual void present(const util::PixmanRegion& damage) = 0;
    virtual void set_title(const std::string& title) { (void)title; }
    virtual void set_transparent(bool transparent) { (void)transparent; }
    // True means presentation is scheduled; false keeps synchronous/headless
    // backends working without a native frame clock.
    virtual bool request_frame() { return false; }
    // True when the backend has a native frame clock and can coalesce small
    // PTY bursts without deferring headless/synchronous clients.
    [[nodiscard]] virtual bool supports_frame_pacing() const noexcept { return false; }
    // True while a committed frame is still waiting for the compositor's frame
    // clock. An idle session then draws immediately (lowest input latency);
    // a busy one keeps parsing input/PTY data and renders on the next tick
    // instead of repainting mid-interval (foot's commit/frame-callback state).
    [[nodiscard]] virtual bool frame_in_flight() const noexcept { return false; }

    [[nodiscard]] virtual int pixel_width() const noexcept = 0;
    [[nodiscard]] virtual int pixel_height() const noexcept = 0;
    [[nodiscard]] virtual double scale() const noexcept = 0;

    using KeyCallback = std::function<void(const input::KeyEvent&)>;
    using MouseCallback = std::function<void(const input::MouseEvent&)>;
    using ResizeCallback = std::function<void(int px_w, int px_h, double scale)>;
    // View-local, logical-point rectangle with a top-left origin. Used only
    // by native IME to place the candidate window.
    struct CursorRect {
        double x{0}, y{0}, width{0}, height{0};
    };
    using CursorRectProvider = std::function<CursorRect()>;
    using BracketedPasteProvider = std::function<bool()>;
    using TextCommitCallback = std::function<void(std::string_view)>;
    using CloseCallback = std::function<void()>;
    using FrameReadyCallback = std::function<void()>;
    using FocusCallback = std::function<void(bool focused)>;

    virtual void set_key_callback(KeyCallback cb) { (void)cb; }
    // IME committed text bypasses KeyEvent/keyboard-protocol encoding.
    virtual void set_text_commit_callback(TextCommitCallback cb) { (void)cb; }
    virtual void set_mouse_callback(MouseCallback cb) { (void)cb; }
    virtual void set_resize_callback(ResizeCallback cb) { (void)cb; }
    virtual void set_close_callback(CloseCallback cb) { (void)cb; }
    // Native frame tick or retry after compositor backpressure. The engine keeps
    // its pending damage/full-redraw state until a back buffer is available.
    virtual void set_frame_ready_callback(FrameReadyCallback cb) { (void)cb; }
    virtual void set_focus_callback(FocusCallback cb) { (void)cb; }
    virtual void set_cursor_rect_provider(CursorRectProvider cb) { (void)cb; }
    virtual void set_bracketed_paste_provider(BracketedPasteProvider cb) { (void)cb; }
    // Open a hyperlink (OSC 8) in the platform's default handler.
    virtual void open_url(std::string_view url) { (void)url; }
};

// Event loop interface (EpollEventLoop or KqueueRunLoop)
class IEventLoop {
public:
    virtual ~IEventLoop() = default;

    static constexpr uint32_t EventReadable = 1 << 0;
    static constexpr uint32_t EventWritable = 1 << 1;
    static constexpr uint32_t EventError    = 1 << 2;

    // Independent one-shot timers. Implementations must not let one subsystem
    // (for example the sync watchdog) cancel another one's pending callback.
    enum class TimerId : uintptr_t {
        Default = 1,
        SyncWatchdog = 2,
        FrameCoalesce = 3,
    };

    using FdCallback = std::function<void(int fd, uint32_t events)>;

    virtual void add_fd(int fd, uint32_t events, FdCallback cb) = 0;
    virtual void remove_fd(int fd) = 0;
    // One-shot timers. A nonpositive delay or empty callback cancels `id`.
    // The int overload is retained for headless/backward-compatible loops and
    // maps to TimerId::Default.
    virtual void set_timer(TimerId id, int delay_ms, std::function<void()> callback) {
        (void)id;
        set_timer(delay_ms, std::move(callback));
    }
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
