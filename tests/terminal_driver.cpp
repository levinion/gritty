// Headless transport for esctest2. Only the production parser generates replies;
// no test-only checksum/window/OSC implementations are injected here.
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <poll.h>
#include <sys/wait.h>
#include <thread>
#include "core/vt_parser.hpp"
#include "platform/posix_pty.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: gritty_protocol_driver command [args...]\n");
        return 2;
    }
    gritty::platform::PtySpawnOptions options;
    options.command = argv[1];
    for (int i = 2; i < argc; ++i) options.args.emplace_back(argv[i]);
    options.cols = 80;
    options.rows = 25; // esctest2's reset size, without requiring a GUI backend
    options.env = {"TMUX", "WINDOWID"};
    gritty::platform::PosixPty pty;
    if (!pty.spawn(options)) return 1;
    gritty::core::TerminalEngine engine(80, 25);
    gritty::core::VtParser parser(engine);
    std::string pending;
    parser.set_response_callback([&](std::string_view data) { pending.append(data); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    uint8_t buffer[16384];
    bool closed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!closed) {
            pollfd fd{pty.master_fd(), static_cast<short>(POLLIN | (pending.empty() ? 0 : POLLOUT)), 0};
            const auto result = ::poll(&fd, 1, 20);
            if (result < 0 && errno != EINTR) return 1;
            if (fd.revents & (POLLIN | POLLHUP | POLLERR)) {
                const auto count = pty.read(buffer);
                if (count > 0) parser.feed(buffer, count);
                else if (count == 0 || (count < 0 && errno == EIO)) closed = true;
                else if (errno != EAGAIN && errno != EWOULDBLOCK) return 1;
            }
            while (!pending.empty() && !closed) {
                const auto count = pty.write(pending);
                if (count > 0) pending.erase(0, count);
                else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                else { closed = true; break; }
            }
        } else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        int status = 0;
        const auto pid = ::waitpid(pty.child_pid(), &status, WNOHANG);
        if (pid > 0) {
            // Drain the final terminal reset before releasing the PTY.
            while (const auto count = pty.read(buffer)) {
                if (count < 0) break;
                parser.feed(buffer, count);
            }
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
        if (pid < 0 && errno != EINTR) return 1;
    }
    std::fprintf(stderr, "esctest2 transport timed out (300s)\n");
    return 124;
}
