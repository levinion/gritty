#include "platform/posix_pty.hpp"

#include "util/version.hpp"

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#include <utmp.h>
#endif

#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>
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

    // Pre-build child strings/argv in parent before forkpty() so the child
    // never invokes C++ heap allocators after forking in a multi-threaded process.
    const std::string term_val = options.term.empty() ? "xterm-256color" : options.term;
    const std::string version_val(util::kVersion);
    struct EnvOp {
        std::string key;
        std::string val;
        bool unset;
    };
    std::vector<EnvOp> env_ops;
    env_ops.reserve(options.env.size());
    for (const auto& e : options.env) {
        const size_t eq = e.find('=');
        if (eq != std::string::npos) {
            env_ops.push_back({e.substr(0, eq), e.substr(eq + 1), false});
        } else {
            env_ops.push_back({e, {}, true});
        }
    }
    std::string shell_path = options.command;
    if (shell_path.empty()) {
        const char* env_shell = std::getenv("SHELL");
        if (env_shell && *env_shell) {
            shell_path = env_shell;
        } else if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_shell && *pw->pw_shell) {
            shell_path = pw->pw_shell;
        } else {
#if defined(__APPLE__)
            shell_path = "/bin/zsh";
#else
            shell_path = ::access("/bin/bash", X_OK) == 0 ? "/bin/bash" : "/bin/sh";
#endif
        }
    }
    std::string argv0_storage = shell_path;
#if defined(__APPLE__)
    // macOS GUI apps expect default interactive shells to start as login shells.
    if (options.command.empty() && options.args.empty()) {
        const size_t slash = shell_path.rfind('/');
        const std::string_view base = slash == std::string::npos
            ? std::string_view(shell_path)
            : std::string_view(shell_path).substr(slash + 1);
        if (!base.empty()) argv0_storage = "-" + std::string(base);
    }
#endif
    std::vector<char*> argv;
    argv.reserve(options.args.size() + 2);
    argv.push_back(const_cast<char*>(argv0_storage.c_str()));
    for (const auto& a : options.args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);
    const char* cwd_cstr = options.cwd.empty() ? nullptr : options.cwd.c_str();

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

        if (cwd_cstr) {
            if (::chdir(cwd_cstr) != 0) {
                // Ignore failure
            }
        }

        ::setenv("TERM", term_val.c_str(), 1);
        ::setenv("COLORTERM", "truecolor", 1);
        ::setenv("TERM_PROGRAM", "gritty", 1);
        ::setenv("TERM_PROGRAM_VERSION", version_val.c_str(), 1);

        for (const auto& op : env_ops) {
            if (op.unset) ::unsetenv(op.key.c_str());
            else ::setenv(op.key.c_str(), op.val.c_str(), 1);
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
    // Close the PTY master first so the kernel delivers EOF/SIGHUP to the
    // foreground process group immediately instead of waiting on usleep polls.
    master_fd_.reset();
    // Reap/check first: an already-reaped PID may have been reused by the OS.
    if (is_alive()) {
        auto wait_briefly = [this](int max_ms) -> pid_t {
            int status = 0;
            pid_t res = 0;
            for (int attempt = 0; attempt < max_ms; ++attempt) {
                do {
                    res = ::waitpid(child_pid_, &status, WNOHANG);
                } while (res < 0 && errno == EINTR);
                if (res != 0) return res;
                ::usleep(1000);
            }
            return 0;
        };
        ::kill(-child_pid_, SIGHUP);
        ::kill(child_pid_, SIGHUP);
        if (wait_briefly(20) == 0) {
            ::kill(child_pid_, SIGTERM);
            if (wait_briefly(20) == 0) {
                ::kill(child_pid_, SIGKILL);
                wait_briefly(100);
            }
        }
        child_pid_ = -1;
    }
}

} // namespace gritty::platform
