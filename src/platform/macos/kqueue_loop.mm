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
}

KqueueRunLoop::~KqueueRunLoop() {
    stop();
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
    timer_callback_ = std::move(callback);
    struct kevent event{};
    EV_SET(&event, 1, EVFILT_TIMER, delay_ms > 0 ? EV_ADD | EV_ENABLE | EV_ONESHOT : EV_DELETE,
        0, delay_ms > 0 ? delay_ms : 0, nullptr);
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
            auto callback = std::move(timer_callback_);
            timer_callback_ = {};
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
    running_ = true;
    while (running_) {
        @autoreleasepool {
            if (NSApp) {
                poll_once(0);

                NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                 untilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]
                                                    inMode:NSDefaultRunLoopMode
                                                   dequeue:YES];
                if (ev) {
                    [NSApp sendEvent:ev];
                    [NSApp updateWindows];
                }
            } else {
                poll_once(100);
            }
        }
    }
}

} // namespace gritty::platform::macos
