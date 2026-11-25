#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <sys/types.h>
#include "util/raii.hpp"

namespace gritty::platform {

struct PtySpawnOptions {
    std::string command; // Empty for default shell
    std::vector<std::string> args;
    std::string cwd;
    std::vector<std::string> env; // NAME=value sets, bare NAME unsets.
    std::string term{"xterm-256color"};
    int cols{80};
    int rows{24};
    int pixel_width{640};
    int pixel_height{480};
};

class PosixPty final {
public:
    PosixPty();
    ~PosixPty();

    PosixPty(const PosixPty&) = delete;
    PosixPty& operator=(const PosixPty&) = delete;
    PosixPty(PosixPty&& other) noexcept;
    PosixPty& operator=(PosixPty&& other) noexcept;

    bool spawn(const PtySpawnOptions& options);
    void resize(int cols, int rows, int pixel_width, int pixel_height);

    [[nodiscard]] int master_fd() const noexcept { return master_fd_.get(); }
    [[nodiscard]] pid_t child_pid() const noexcept { return child_pid_; }
    [[nodiscard]] bool is_alive() const noexcept;

    // Read available bytes (non-blocking). Returns bytes read, 0 on EOF, or -1 on EAGAIN/error.
    ssize_t read(std::span<uint8_t> buffer);

    // Write bytes to child (non-blocking). Returns bytes written or -1.
    ssize_t write(std::string_view data);

    void terminate();

private:
    util::UniqueFd master_fd_;
    mutable pid_t child_pid_{-1};
};

} // namespace gritty::platform
