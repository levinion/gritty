#include "platform/macos/kqueue_loop.hpp"
#import <Cocoa/Cocoa.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>

namespace gritty::platform::macos {

KqueueRunLoop::KqueueRunLoop() {
    int kq = ::kqueue();
    kq_fd_.reset(kq);
    if (kq >= 0) ::fcntl(kq, F_SETFD, FD_CLOEXEC);

    int pipefd[2];
    if (::pipe(pipefd) == 0) {
        ::fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
        ::fcntl(pipefd[0], F_SETFL, ::fcntl(pipefd[0], F_GETFL, 0) | O_NONBLOCK);
        ::fcntl(pipefd[1], F_SETFL, ::fcntl(pipefd[1], F_GETFL, 0) | O_NONBLOCK);
        wake_pipe_read_.reset(pipefd[0]);
        wake_pipe_write_.reset(pipefd[1]);

        struct kevent ev{};
        EV_SET(&ev, wake_pipe_read_.get(), EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        ::kevent(kq_fd_.get(), &ev, 1, nullptr, 0, nullptr);
    }

    // AppKit's event tracking modes do not run our hand-written poll loop.
    // Publish kqueue readiness as a CFRunLoop source in common modes so PTY,
    // timers and wakeups keep firing during live resize/scroll gestures.
    if (kq_fd_.valid()) {
        CFFileDescriptorContext fd_context{};
        fd_context.version = 0;
        fd_context.info = this;
        cf_fd_ = CFFileDescriptorCreate(kCFAllocatorDefault, kq_fd_.get(), false,
            &KqueueRunLoop::on_kq_ready, &fd_context);
        if (cf_fd_) {
            // This factory connects the CFFileDescriptor callout to a runloop
            // source. A generic CFRunLoopSourceCreate() has no fd perform
            // callback and would never signal our on_kq_ready handler.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            cf_source_ = CFFileDescriptorCreateRunLoopSource(kCFAllocatorDefault, cf_fd_, 0);
#pragma clang diagnostic pop
            if (cf_source_) {
                CFRunLoopAddSource(CFRunLoopGetMain(), cf_source_, kCFRunLoopCommonModes);
                CFFileDescriptorEnableCallBacks(cf_fd_, kCFFileDescriptorReadCallBack);
            }
        }
    }
}

KqueueRunLoop::~KqueueRunLoop() {
    stop();
    if (cf_source_) {
        CFRunLoopSourceInvalidate(cf_source_);
        CFRelease(cf_source_);
        cf_source_ = nullptr;
    }
    if (cf_fd_) {
        CFFileDescriptorInvalidate(cf_fd_);
        CFRelease(cf_fd_);
        cf_fd_ = nullptr;
    }
    // kq_fd_ and the wake pipe are closed by their RAII owners.
}

void KqueueRunLoop::add_fd(int fd, uint32_t events, FdCallback cb) {
    if (!kq_fd_.valid() || fd < 0) return;

    callbacks_[fd] = std::move(cb);

    struct kevent ev[2];
    int n = 0;

    if (events & EventReadable) {
        EV_SET(&ev[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    }
    if (events & EventWritable) {
        EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    }

    if (n > 0) {
        ::kevent(kq_fd_.get(), ev, n, nullptr, 0, nullptr);
    }
}

void KqueueRunLoop::remove_fd(int fd) {
    if (!kq_fd_.valid() || fd < 0) return;

    callbacks_.erase(fd);

    struct kevent ev[2];
    EV_SET(&ev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&ev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    ::kevent(kq_fd_.get(), ev, 2, nullptr, 0, nullptr);
}

void KqueueRunLoop::set_timer(int delay_ms, std::function<void()> callback) {
    set_timer(TimerId::Default, delay_ms, std::move(callback));
}

void KqueueRunLoop::set_timer(TimerId id, int delay_ms, std::function<void()> callback) {
    const auto key = static_cast<uintptr_t>(id);
    struct kevent event{};
    if (!callback || delay_ms <= 0) {
        timers_.erase(key);
        if (kq_fd_.valid()) {
            EV_SET(&event, key, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
            ::kevent(kq_fd_.get(), &event, 1, nullptr, 0, nullptr);
        }
        return;
    }
    if (!kq_fd_.valid()) return;

    timers_.insert_or_assign(key, std::move(callback));
    uint16_t fflags = 0;
    if (id == TimerId::FrameCoalesce) {
#ifdef NOTE_CRITICAL
        // Avoid kernel timer coalescing for sub-frame interactive bursts.
        fflags |= NOTE_CRITICAL;
#endif
    }
    // EV_ADD updates an existing kevent with the same ident/filter; no separate
    // EV_DELETE syscall is needed when replacing a pending one-shot timer.
    EV_SET(&event, key, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT, fflags, delay_ms, nullptr);
    ::kevent(kq_fd_.get(), &event, 1, nullptr, 0, nullptr);
}

void KqueueRunLoop::wake() {
    if (wake_pipe_write_.valid()) {
        char c = 1;
        (void)::write(wake_pipe_write_.get(), &c, 1);
    }
    if (NSApp) {
        NSEvent* dummy = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                            location:NSZeroPoint
                                       modifierFlags:0
                                           timestamp:0
                                        windowNumber:0
                                             context:nil
                                             subtype:0
                                               data1:0
                                               data2:0];
        [NSApp postEvent:dummy atStart:NO];
    }
}

void KqueueRunLoop::stop() {
    running_ = false;
    wake();
    if (NSApp) {
        // CFRunLoopStop() alone only exits one CFRunLoop invocation; NSApp
        // would immediately re-enter its own event loop. stop: makes [NSApp
        // run] return, while the posted wake event unblocks a nested modal
        // tracking loop.
        [NSApp stop:nil];
    }
    CFRunLoopStop(CFRunLoopGetMain());
}

void KqueueRunLoop::on_kq_ready(CFFileDescriptorRef fd, CFOptionFlags flags, void* context) {
    (void)flags;
    auto* self = static_cast<KqueueRunLoop*>(context);
    if (self) self->poll_once(0);
    // A callback may run while destruction is invalidating the descriptor.
    // Re-arm only after draining kqueue and only while the fd is still live.
    if (CFFileDescriptorIsValid(fd))
        CFFileDescriptorEnableCallBacks(fd, kCFFileDescriptorReadCallBack);
}

int KqueueRunLoop::poll_once(int timeout_ms) {
    if (!kq_fd_.valid()) return -1;

    struct timespec ts{};
    struct timespec* pts = nullptr;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        pts = &ts;
    }

    struct kevent evs[32];
    int n = ::kevent(kq_fd_.get(), nullptr, 0, evs, 32, pts);
    if (n <= 0) return n;

    for (int i = 0; i < n; ++i) {
        if (evs[i].filter == EVFILT_TIMER) {
            const auto key = static_cast<uintptr_t>(evs[i].ident);
            auto it = timers_.find(key);
            if (it == timers_.end()) continue;
            // Remove before invoking: a callback may reschedule the same id.
            auto callback = std::move(it->second);
            timers_.erase(it);
            if (callback) callback();
            continue;
        }
        int fd = static_cast<int>(evs[i].ident);

        if (fd == wake_pipe_read_.get()) {
            char buf[64];
            while (::read(wake_pipe_read_.get(), buf, sizeof(buf)) > 0) {}
            continue;
        }

        auto it = callbacks_.find(fd);
        if (it != callbacks_.end()) {
            uint32_t flags = 0;
            if (evs[i].filter == EVFILT_READ) flags |= EventReadable;
            if (evs[i].filter == EVFILT_WRITE) flags |= EventWritable;
            if (evs[i].flags & EV_ERROR) flags |= EventError;
            // A callback may re-register or remove itself while handling I/O.
            auto callback = it->second;
            callback(fd, flags);
        }
    }

    return n;
}

void KqueueRunLoop::run() {
    if (NSApp && ![NSApp isRunning]) [NSApp finishLaunching];
    running_ = true;
    if (NSApp) {
        // NSApp owns NSEvent delivery: it dequeues events and calls sendEvent:.
        // A bare CFRunLoop only runs sources; it never dispatches keyDown,
        // mouseDown or the window close button to NSResponder. Our kqueue
        // CFFileDescriptor source is installed in common modes, so PTY and
        // timers are still serviced by NSApp's run loop, including while
        // AppKit is in NSEventTrackingRunLoopMode during live resize/scroll.
        [NSApp run];
        return;
    }

    // Headless probes have no NSApplication; keep the CFRunLoop fallback.
    while (running_) {
        @autoreleasepool {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, true);
        }
    }
}

} // namespace gritty::platform::macos
