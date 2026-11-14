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

namespace squirt::platform {

PosixPty::PosixPty() = default;

PosixPty::~PosixPty() {
    terminate();
}

bool PosixPty::spawn(const PtySpawnOptions& options) {
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
        ::setenv("TERM_PROGRAM", "squirt", 1);
        ::setenv("TERM_PROGRAM_VERSION", "0.1.0", 1);

        for (const auto& e : options.env) {
            size_t eq = e.find('=');
            if (eq != std::string::npos) {
                std::string k = e.substr(0, eq);
                std::string v = e.substr(eq + 1);
                ::setenv(k.c_str(), v.c_str(), 1);
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
    pid_t res = ::waitpid(child_pid_, &status, WNOHANG);
    return res == 0;
}

ssize_t PosixPty::read(std::span<uint8_t> buffer) {
    if (!master_fd_.valid() || buffer.empty()) return -1;
    ssize_t n = ::read(master_fd_.get(), buffer.data(), buffer.size());
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return -1;
    }
    return n;
}

ssize_t PosixPty::write(std::string_view data) {
    if (!master_fd_.valid() || data.empty()) return -1;
    ssize_t n = ::write(master_fd_.get(), data.data(), data.size());
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return -1;
    }
    return n;
}

void PosixPty::terminate() {
    if (child_pid_ > 0) {
        ::kill(child_pid_, SIGHUP);
        int status = 0;
        pid_t res = ::waitpid(child_pid_, &status, WNOHANG);
        if (res == 0) {
            ::kill(child_pid_, SIGTERM);
            // Non-blocking check
            res = ::waitpid(child_pid_, &status, WNOHANG);
            if (res == 0) {
                ::kill(child_pid_, SIGKILL);
                ::waitpid(child_pid_, &status, 0);
            }
        }
        child_pid_ = -1;
    }
    master_fd_.reset();
}

} // namespace squirt::platform
