#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <pixman.h>
#include "util/raii.hpp"
#include "input/input_events.hpp"

namespace gritty::platform {

enum class WindowDecorations : uint8_t {
    Full = 0,
    None = 1,
    Transparent = 2,
    Buttonless = 3,
};

// Pointer shapes requested by OSC 22. Kept platform-neutral here; native
// cursor mapping lives in each platform backend.
enum class MouseCursorShape : uint8_t {
    Default = 0,
    Text,
    Pointer,
    EwResize,
    NsResize,
    NeswResize,
    NwseResize,
    Crosshair,
    Move,
    NotAllowed,
    Hidden,
};

// Case-insensitive, allocation-free parser for the TOML/CLI string values.
[[nodiscard]] constexpr std::optional<WindowDecorations> parse_window_decorations(
    std::string_view value
) noexcept {
    auto equals = [value](std::string_view expected) noexcept {
        if (value.size() != expected.size()) return false;
        for (size_t i = 0; i < value.size(); ++i) {
            const char c = value[i];
            const char lowered = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            if (lowered != expected[i]) return false;
        }
        return true;
    };
    if (equals("full")) return WindowDecorations::Full;
    if (equals("none")) return WindowDecorations::None;
    if (equals("transparent")) return WindowDecorations::Transparent;
    if (equals("buttonless")) return WindowDecorations::Buttonless;
    return std::nullopt;
}

// Window surface abstraction implemented by the active platform backend.
class IWindowSurface {
public:
    virtual ~IWindowSurface() = default;

    virtual void resize(int pixel_width, int pixel_height, double scale) = 0;
    // Return an exclusively writable buffer with the last presented pixels as
    // its baseline. nullptr means compositor backpressure; keep pending damage.
    // A frame-clock tick alone never grants write access to an attached buffer.
    virtual pixman_image_t* get_back_buffer() = 0;
    virtual pixman_image_t* get_back_buffer_for_frame(bool full_redraw) {
        (void)full_redraw;
        return get_back_buffer();
    }
    [[nodiscard]] virtual bool is_ready() const noexcept { return true; }
    virtual void present(const util::PixmanRegion& damage) = 0;
    virtual void set_title(const std::string& title) { (void)title; }
    virtual void set_transparent(bool transparent) { (void)transparent; }
    virtual void set_decorations(WindowDecorations decorations) { (void)decorations; }
    virtual void set_cursor_shape(MouseCursorShape shape) { (void)shape; }
    [[nodiscard]] virtual WindowDecorations decorations() const noexcept {
        return WindowDecorations::Full;
    }
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
    // True while the child has enabled mouse reporting; native titlebar drags
    // must not steal those events.
    using MouseReportingProvider = std::function<bool()>;
    using TextCommitCallback = std::function<void(std::string_view)>;
    using PreeditCallback = std::function<void(std::string_view utf8, size_t sel_start, size_t sel_end)>;
    // Native paste (Cmd+V / middle-click) hands raw UTF-8 to the session,
    // which applies the terminal's DECSET 2004 formatting before PTY write.
    using PasteCallback = std::function<void(std::string_view)>;
    using CloseCallback = std::function<void()>;
    using FrameReadyCallback = std::function<void()>;
    using FocusCallback = std::function<void(bool focused)>;

    virtual void set_key_callback(KeyCallback cb) { (void)cb; }
    // IME committed text bypasses KeyEvent/keyboard-protocol encoding.
    virtual void set_text_commit_callback(TextCommitCallback cb) { (void)cb; }
    virtual void set_preedit_callback(PreeditCallback cb) { (void)cb; }
    virtual void set_paste_callback(PasteCallback cb) { (void)cb; }
    virtual void set_mouse_callback(MouseCallback cb) { (void)cb; }
    virtual void set_resize_callback(ResizeCallback cb) { (void)cb; }
    virtual void set_close_callback(CloseCallback cb) { (void)cb; }
    // Native frame tick or retry after compositor backpressure. The engine keeps
    // its pending damage/full-redraw state until a back buffer is available.
    virtual void set_frame_ready_callback(FrameReadyCallback cb) { (void)cb; }
    virtual void set_focus_callback(FocusCallback cb) { (void)cb; }
    virtual void set_cursor_rect_provider(CursorRectProvider cb) { (void)cb; }
    virtual void set_bracketed_paste_provider(BracketedPasteProvider cb) { (void)cb; }
    virtual void set_mouse_reporting_provider(MouseReportingProvider cb) { (void)cb; }
    // Open a hyperlink (OSC 8) in the platform's default handler.
    virtual void open_url(std::string_view url) { (void)url; }
    // OSC 777 desktop notification. Implementations must not block the loop.
    virtual void send_notification(std::string_view title, std::string_view body) {
        (void)title;
        (void)body;
    }
};

// Event loop abstraction for I/O watches, scoped one-shot timers, and deferred callbacks.
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
    // One-shot timers keyed by (owner, TimerId); a non-positive delay or empty callback cancels.
    virtual void add_timer(TimerId id, int delay_ms, std::function<void()> callback) {
        set_timer(id, delay_ms, std::move(callback));
    }
    virtual void cancel_timer(TimerId id) { set_timer(id, 0, {}); }
    virtual void set_timer(TimerId id, int delay_ms, std::function<void()> callback) {
        (void)id;
        set_timer(delay_ms, std::move(callback));
    }
    virtual void set_scoped_timer(
        uintptr_t owner,
        TimerId id,
        int delay_ms,
        std::function<void()> callback
    ) {
        (void)owner;
        set_timer(id, delay_ms, std::move(callback));
    }
    virtual void cancel_scoped_timer(uintptr_t owner, TimerId id) {
        set_scoped_timer(owner, id, 0, {});
    }
    virtual void set_timer(int delay_ms, std::function<void()> callback) = 0;
    // Schedule a callback to run safely outside the current I/O or UI handler.
    virtual void post(std::function<void()> callback) {
        if (callback) callback();
    }
    virtual void run() = 0;
    virtual void stop() = 0;
    virtual void wake() = 0;
};

// Font family and fallback chain discovery interface.
struct FontPattern {
    std::string family;
    bool bold{false};
    bool italic{false};
};

struct FontMatch {
    std::string path;
    int face_index{0};
    std::string postscript_name;
};

class IFontDiscovery {
public:
    virtual ~IFontDiscovery() = default;

    virtual std::optional<FontMatch> find_font(const FontPattern& pattern) = 0;
    virtual std::vector<FontMatch> fallback_chain(const FontPattern& pattern) = 0;
};

// System clipboard abstraction.
class IClipboard {
public:
    virtual ~IClipboard() = default;

    virtual void copy_text(std::string_view text) = 0;
    virtual std::string paste_text() = 0;
};

} // namespace gritty::platform
