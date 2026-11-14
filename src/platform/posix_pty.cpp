#include "platform/posix_pty.hpp"

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#include <utmp.h>
#endif

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <utility>

namespace gritty::platform {

PosixPty::PosixPty() = default;

PosixPty::PosixPty(PosixPty&& other) noexcept
    : master_fd_(std::move(other.master_fd_)), child_pid_(std::exchange(other.child_pid_, -1)) {}

PosixPty& PosixPty::operator=(PosixPty&& other) noexcept {
    if (this != &other) {
        terminate();
        master_fd_ = std::move(other.master_fd_);
        child_pid_ = std::exchange(other.child_pid_, -1);
    }
    return *this;
}

PosixPty::~PosixPty() {
    terminate();
}

bool PosixPty::spawn(const PtySpawnOptions& options) {
    if (master_fd_.valid() || child_pid_ > 0) return false;
    struct winsize ws{};
    ws.ws_col = static_cast<unsigned short>(options.cols);
    ws.ws_row = static_cast<unsigned short>(options.rows);
    ws.ws_xpixel = static_cast<unsigned short>(options.pixel_width);
    ws.ws_ypixel = static_cast<unsigned short>(options.pixel_height);

    int master = -1;
    pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) {
        return false;
    }

    if (pid == 0) {
        // Child process
        ::signal(SIGCHLD, SIG_DFL);
        ::signal(SIGHUP, SIG_DFL);
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGQUIT, SIG_DFL);
        ::signal(SIGTERM, SIG_DFL);
        ::signal(SIGALRM, SIG_DFL);

        // Change directory if requested
        if (!options.cwd.empty()) {
            if (::chdir(options.cwd.c_str()) != 0) {
                // Ignore failure
            }
        }

        // Setup environment
        ::setenv("TERM", "xterm-256color", 1);
        ::setenv("COLORTERM", "truecolor", 1);
        ::setenv("TERM_PROGRAM", "gritty", 1);
        ::setenv("TERM_PROGRAM_VERSION", "0.1.0", 1);

        for (const auto& e : options.env) {
            size_t eq = e.find('=');
            if (eq != std::string::npos) {
                std::string k = e.substr(0, eq);
                std::string v = e.substr(eq + 1);
                ::setenv(k.c_str(), v.c_str(), 1);
            } else {
                ::unsetenv(e.c_str());
            }
        }

        // Determine executable
        std::string shell_path = options.command;
        if (shell_path.empty()) {
            const char* env_shell = std::getenv("SHELL");
            shell_path = (env_shell && *env_shell) ? env_shell : "/bin/sh";
        }

        std::vector<char*> argv;
        if (!options.args.empty()) {
            argv.push_back(const_cast<char*>(shell_path.c_str()));
            for (const auto& a : options.args) {
                argv.push_back(const_cast<char*>(a.c_str()));
            }
            argv.push_back(nullptr);
        } else {
            argv.push_back(const_cast<char*>(shell_path.c_str()));
            argv.push_back(nullptr);
        }

        ::execvp(shell_path.c_str(), argv.data());
        ::_exit(127);
    }

    // Parent process
    child_pid_ = pid;
    master_fd_.reset(master);
    ::fcntl(master, F_SETFD, FD_CLOEXEC);

    // Set master fd non-blocking
    int flags = ::fcntl(master_fd_.get(), F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(master_fd_.get(), F_SETFL, flags | O_NONBLOCK);
    }

    return true;
}

void PosixPty::resize(int cols, int rows, int pixel_width, int pixel_height) {
    if (!master_fd_.valid()) return;

    struct winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_xpixel = static_cast<unsigned short>(pixel_width);
    ws.ws_ypixel = static_cast<unsigned short>(pixel_height);

    ::ioctl(master_fd_.get(), TIOCSWINSZ, &ws);
}

bool PosixPty::is_alive() const noexcept {
    if (child_pid_ <= 0) return false;
    int status = 0;
    pid_t res;
    do { res = ::waitpid(child_pid_, &status, WNOHANG); } while (res < 0 && errno == EINTR);
    if (res == child_pid_ || (res < 0 && errno == ECHILD)) child_pid_ = -1;
    return res == 0;
}

ssize_t PosixPty::read(std::span<uint8_t> buffer) {
    if (!master_fd_.valid() || buffer.empty()) return -1;
    ssize_t n;
    do { n = ::read(master_fd_.get(), buffer.data(), buffer.size()); } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return -1;
    }
    return n;
}

ssize_t PosixPty::write(std::string_view data) {
    if (!master_fd_.valid() || data.empty()) return -1;
    ssize_t n;
    do { n = ::write(master_fd_.get(), data.data(), data.size()); } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return -1;
    }
    return n;
}

void PosixPty::terminate() {
    // Reap/check first: an already-reaped PID may have been reused by the OS.
    if (is_alive()) {
        ::kill(child_pid_, SIGHUP);
        int status = 0;
        pid_t res = ::waitpid(child_pid_, &status, WNOHANG);
        if (res == 0) {
            ::kill(child_pid_, SIGTERM);
            // Non-blocking check
            res = ::waitpid(child_pid_, &status, WNOHANG);
            if (res == 0) {
                ::kill(child_pid_, SIGKILL);
                while (::waitpid(child_pid_, &status, 0) < 0 && errno == EINTR) {}
            }
        }
        child_pid_ = -1;
    }
    master_fd_.reset();
}

} // namespace gritty::platform
