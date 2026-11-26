#include "platform/single_instance.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace gritty::platform {

namespace {

// Wire format: [4B LE length][4B magic "GRT1"][length-prefixed fields] (max 64 KiB).
constexpr std::array<uint8_t, 4> kMagic = {'G', 'R', 'T', '1'};
constexpr size_t kMaxPayloadBytes = 64 * 1024;
constexpr size_t kMaxResponseBytes = 4096;

void append_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xffu));
    }
}

void append_u64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xffu));
    }
}

void append_string(std::vector<uint8_t>& out, std::string_view s) {
    const uint32_t len = static_cast<uint32_t>(std::min<size_t>(s.size(), kMaxPayloadBytes));
    append_u32(out, len);
    out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(len));
}

void append_string_vec(std::vector<uint8_t>& out, const std::vector<std::string>& vec) {
    const uint32_t count = static_cast<uint32_t>(std::min<size_t>(vec.size(), 1024));
    append_u32(out, count);
    for (uint32_t i = 0; i < count; ++i) {
        append_string(out, vec[i]);
    }
}

bool read_u32(std::span<const uint8_t>& data, uint32_t& out) noexcept {
    if (data.size() < 4) return false;
    out = static_cast<uint32_t>(data[0]) |
          (static_cast<uint32_t>(data[1]) << 8) |
          (static_cast<uint32_t>(data[2]) << 16) |
          (static_cast<uint32_t>(data[3]) << 24);
    data = data.subspan(4);
    return true;
}

bool read_u64(std::span<const uint8_t>& data, uint64_t& out) noexcept {
    if (data.size() < 8) return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(data[static_cast<size_t>(i)]) << (i * 8);
    }
    data = data.subspan(8);
    out = v;
    return true;
}

bool read_u8(std::span<const uint8_t>& data, uint8_t& out) noexcept {
    if (data.empty()) return false;
    out = data[0];
    data = data.subspan(1);
    return true;
}

bool read_string(std::span<const uint8_t>& data, std::string& out) {
    uint32_t len = 0;
    if (!read_u32(data, len) || len > data.size()) return false;
    out.assign(reinterpret_cast<const char*>(data.data()), len);
    data = data.subspan(len);
    return true;
}

bool read_string_vec(std::span<const uint8_t>& data, std::vector<std::string>& out) {
    uint32_t count = 0;
    if (!read_u32(data, count) || count > 1024) return false;
    out.clear();
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::string item;
        if (!read_string(data, item)) return false;
        out.push_back(std::move(item));
    }
    return true;
}

bool make_unix_addr(const std::string& path, struct sockaddr_un& addr, socklen_t& addr_len) noexcept {
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) return false;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.data(), path.size());
    addr.sun_path[path.size()] = '\0';
    addr_len = static_cast<socklen_t>(sizeof(addr));
    return true;
}

void set_cloexec_nonblock(int fd, bool nonblock) noexcept {
    const int fd_flags = ::fcntl(fd, F_GETFD, 0);
    if (fd_flags >= 0) ::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
    if (nonblock) {
        const int fl = ::fcntl(fd, F_GETFL, 0);
        if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
}

void configure_timeouts_and_nosigpipe(int fd) noexcept {
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    struct timeval tv{};
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

bool write_all(int fd, const uint8_t* data, size_t len) noexcept {
    size_t sent = 0;
    while (sent < len) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(fd, data + sent, len - sent, 0);
#endif
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<uint8_t> encode_launch_request(const LaunchRequest& req) {
    std::vector<uint8_t> out;
    out.reserve(256);
    out.insert(out.end(), kMagic.begin(), kMagic.end());
    append_string(out, req.config_path);
    append_string(out, req.explicit_cwd);
    append_string(out, req.inherited_cwd);
    append_string(out, req.title);
    append_string(out, req.app_id);
    append_string(out, req.font_family);
    append_u64(out, std::bit_cast<uint64_t>(req.font_size));
    append_u64(out, static_cast<uint64_t>(req.cols));
    append_u64(out, static_cast<uint64_t>(req.rows));
    out.push_back(req.hold ? 1 : 0);
    append_string_vec(out, req.config_overrides);
    append_string(out, req.command);
    append_string_vec(out, req.command_args);
    return out;
}

std::optional<LaunchRequest> decode_launch_request(std::span<const uint8_t> payload) {
    if (payload.size() < kMagic.size() || payload.size() > kMaxPayloadBytes) return std::nullopt;
    if (!std::equal(kMagic.begin(), kMagic.end(), payload.begin())) return std::nullopt;
    payload = payload.subspan(kMagic.size());

    LaunchRequest req;
    uint64_t font_bits = 0;
    uint64_t cols = 0;
    uint64_t rows = 0;
    uint8_t hold = 0;
    if (!read_string(payload, req.config_path) ||
        !read_string(payload, req.explicit_cwd) ||
        !read_string(payload, req.inherited_cwd) ||
        !read_string(payload, req.title) ||
        !read_string(payload, req.app_id) ||
        !read_string(payload, req.font_family) ||
        !read_u64(payload, font_bits) ||
        !read_u64(payload, cols) ||
        !read_u64(payload, rows) ||
        !read_u8(payload, hold) ||
        !read_string_vec(payload, req.config_overrides) ||
        !read_string(payload, req.command) ||
        !read_string_vec(payload, req.command_args)) {
        return std::nullopt;
    }
    if (!payload.empty()) return std::nullopt;

    req.font_size = std::bit_cast<double>(font_bits);
    req.cols = static_cast<size_t>(cols);
    req.rows = static_cast<size_t>(rows);
    req.hold = (hold != 0);
    return req;
}

std::string default_instance_socket_path() {
    if (const char* override_path = std::getenv("GRITTY_INSTANCE_SOCKET");
        override_path && *override_path) {
        return std::string(override_path);
    }
    const std::string suffix = "/gritty-" + std::to_string(static_cast<unsigned long>(::getuid())) + ".sock";
    struct sockaddr_un dummy{};
    if (const char* tmp = std::getenv("TMPDIR"); tmp && *tmp) {
        std::string dir(tmp);
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        if (!dir.empty() && dir.size() + suffix.size() < sizeof(dummy.sun_path)) {
            return dir + suffix;
        }
    }
    return "/tmp" + suffix;
}

SendStatus try_send_launch_request(
    const std::string& socket_path,
    const LaunchRequest& req,
    std::string* error_out
) {
    struct sockaddr_un addr{};
    socklen_t addr_len = 0;
    if (!make_unix_addr(socket_path, addr, addr_len)) {
        if (error_out) *error_out = "Instance socket path is too long or empty: " + socket_path;
        return SendStatus::NoServer;
    }

    util::UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fd.valid()) {
        if (error_out) *error_out = std::strerror(errno);
        return SendStatus::NoServer;
    }
    set_cloexec_nonblock(fd.get(), false);
    configure_timeouts_and_nosigpipe(fd.get());

    while (::connect(fd.get(), reinterpret_cast<struct sockaddr*>(&addr), addr_len) < 0) {
        if (errno == EINTR) continue;
        return SendStatus::NoServer;
    }

    const std::vector<uint8_t> payload = encode_launch_request(req);
    if (payload.size() > kMaxPayloadBytes) {
        if (error_out) *error_out = "Launch request exceeds maximum payload size";
        return SendStatus::ServerError;
    }
    std::vector<uint8_t> frame;
    frame.reserve(4 + payload.size());
    append_u32(frame, static_cast<uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    if (!write_all(fd.get(), frame.data(), frame.size())) {
        if (error_out) *error_out = "Failed to send launch request to running Gritty instance";
        return SendStatus::ServerError;
    }
    (void)::shutdown(fd.get(), SHUT_WR);

    std::vector<uint8_t> resp;
    uint8_t buf[512];
    while (resp.size() < kMaxResponseBytes) {
        const ssize_t n = ::recv(fd.get(), buf, sizeof(buf), 0);
        if (n > 0) {
            resp.insert(resp.end(), buf, buf + n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    if (resp.empty()) {
        if (error_out) *error_out = "Running Gritty instance closed connection without acknowledging";
        return SendStatus::ServerError;
    }
    if (resp[0] == 'K') return SendStatus::SentOk;
    if (resp[0] == 'E') {
        if (error_out) {
            *error_out = resp.size() > 1
                ? std::string(reinterpret_cast<const char*>(resp.data() + 1), resp.size() - 1)
                : std::string("Running Gritty instance rejected window request");
        }
        return SendStatus::ServerError;
    }
    if (error_out) *error_out = "Unexpected response from running Gritty instance";
    return SendStatus::ServerError;
}

SingleInstanceServer::~SingleInstanceServer() {
    stop();
}

SingleInstanceServer::AcquireStatus SingleInstanceServer::acquire_or_forward(
    const std::string& socket_path,
    const LaunchRequest& req,
    std::string* error_out
) {
    stop();
    if (socket_path.empty()) {
        if (error_out) *error_out = "Empty instance socket path";
        return AcquireStatus::Failed;
    }

    // Hold an exclusive startup lock only during probe + bind so concurrent
    // launches serialize cleanly without blocking future forwarders.
    const std::string lock_path = socket_path + ".lock";
    util::UniqueFd lock_fd(::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600));
    if (lock_fd.valid()) {
        while (::flock(lock_fd.get(), LOCK_EX) < 0 && errno == EINTR) {}
    }

    std::string send_error;
    const SendStatus status = try_send_launch_request(socket_path, req, &send_error);
    if (status == SendStatus::SentOk) {
        return AcquireStatus::ForwardedOk;
    }
    if (status == SendStatus::ServerError) {
        if (error_out) *error_out = std::move(send_error);
        return AcquireStatus::ForwardedError;
    }

    struct sockaddr_un addr{};
    socklen_t addr_len = 0;
    if (!make_unix_addr(socket_path, addr, addr_len)) {
        if (error_out) *error_out = "Instance socket path is too long: " + socket_path;
        return AcquireStatus::Failed;
    }

    // No live server answered connect(): clean up stale socket before binding.
    (void)::unlink(socket_path.c_str());

    util::UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fd.valid()) {
        if (error_out) *error_out = std::strerror(errno);
        return AcquireStatus::Failed;
    }
    set_cloexec_nonblock(fd.get(), true);

    if (::bind(fd.get(), reinterpret_cast<struct sockaddr*>(&addr), addr_len) < 0) {
        if (error_out) *error_out = std::strerror(errno);
        return AcquireStatus::Failed;
    }
    (void)::chmod(socket_path.c_str(), 0600);
    if (::listen(fd.get(), 16) < 0) {
        const int err = errno;
        (void)::unlink(socket_path.c_str());
        if (error_out) *error_out = std::strerror(err);
        return AcquireStatus::Failed;
    }

    struct stat st{};
    if (::lstat(socket_path.c_str(), &st) == 0) {
        bound_dev_ = static_cast<uint64_t>(st.st_dev);
        bound_ino_ = static_cast<uint64_t>(st.st_ino);
        has_bound_stat_ = true;
    }
    socket_path_ = socket_path;
    listen_fd_ = std::move(fd);
    return AcquireStatus::BecamePrimary;
}

void SingleInstanceServer::attach(IEventLoop& loop, RequestHandler handler) {
    loop_ = &loop;
    handler_ = std::move(handler);
    if (!listen_fd_.valid()) return;
    loop_->add_fd(listen_fd_.get(), IEventLoop::EventReadable, [this](int, uint32_t events) {
        if (events & IEventLoop::EventReadable) handle_accept();
    });
}

void SingleInstanceServer::handle_accept() {
    while (listen_fd_.valid()) {
        const int client_fd = ::accept(listen_fd_.get(), nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        set_cloexec_nonblock(client_fd, true);
        configure_timeouts_and_nosigpipe(client_fd);

        ClientState state;
        state.fd.reset(client_fd);
        clients_.emplace(client_fd, std::move(state));

        if (loop_) {
            loop_->add_fd(client_fd, IEventLoop::EventReadable, [this](int fd, uint32_t events) {
                handle_client_io(fd, events);
            });
        }
    }
}

void SingleInstanceServer::handle_client_io(int fd, uint32_t events) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return;

    if (events & IEventLoop::EventError) {
        close_client(fd);
        return;
    }

    auto& buf = it->second.buffer;
    uint8_t tmp[4096];
    bool peer_closed = false;
    while (true) {
        const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n > 0) {
            if (buf.size() + static_cast<size_t>(n) > kMaxPayloadBytes + 4) {
                send_client_response(fd, false, "Launch request too large");
                close_client(fd);
                return;
            }
            buf.insert(buf.end(), tmp, tmp + n);
        } else if (n == 0) {
            peer_closed = true;
            break;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_client(fd);
            return;
        }
    }

    if (buf.size() >= 4) {
        std::span<const uint8_t> header(buf.data(), 4);
        uint32_t payload_len = 0;
        (void)read_u32(header, payload_len);
        if (payload_len < kMagic.size() || payload_len > kMaxPayloadBytes) {
            send_client_response(fd, false, "Invalid launch request length");
            close_client(fd);
            return;
        }
        if (buf.size() >= 4 + static_cast<size_t>(payload_len)) {
            const auto req = decode_launch_request(
                std::span<const uint8_t>(buf.data() + 4, payload_len));
            if (!req) {
                send_client_response(fd, false, "Malformed launch request");
                close_client(fd);
                return;
            }
            std::expected<void, std::string> result;
            if (handler_) result = handler_(*req);
            if (result) send_client_response(fd, true, {});
            else send_client_response(fd, false, result.error());
            close_client(fd);
            return;
        }
    }

    if (peer_closed) {
        close_client(fd);
    }
}

void SingleInstanceServer::send_client_response(int fd, bool ok, std::string_view message) {
    std::vector<uint8_t> out;
    out.reserve(1 + message.size());
    out.push_back(ok ? 'K' : 'E');
    if (!ok && !message.empty()) {
        const size_t n = std::min<size_t>(message.size(), kMaxResponseBytes - 1);
        out.insert(out.end(), message.begin(), message.begin() + static_cast<std::ptrdiff_t>(n));
    }
    (void)write_all(fd, out.data(), out.size());
}

void SingleInstanceServer::close_client(int fd) {
    if (loop_) loop_->remove_fd(fd);
    clients_.erase(fd);
}

void SingleInstanceServer::stop() {
    std::vector<int> client_fds;
    client_fds.reserve(clients_.size());
    for (const auto& [fd, _] : clients_) client_fds.push_back(fd);
    for (const int fd : client_fds) close_client(fd);
    clients_.clear();

    if (listen_fd_.valid()) {
        if (loop_) loop_->remove_fd(listen_fd_.get());
        listen_fd_.reset();
    }
    if (!socket_path_.empty()) {
        // Verify dev/ino before unlinking so we never remove another instance's socket.
        bool remove_sock = !has_bound_stat_;
        if (has_bound_stat_) {
            struct stat st{};
            if (::lstat(socket_path_.c_str(), &st) == 0 &&
                static_cast<uint64_t>(st.st_dev) == bound_dev_ &&
                static_cast<uint64_t>(st.st_ino) == bound_ino_) {
                remove_sock = true;
            }
        }
        if (remove_sock) (void)::unlink(socket_path_.c_str());
        socket_path_.clear();
    }
    has_bound_stat_ = false;
    bound_dev_ = 0;
    bound_ino_ = 0;
    handler_ = {};
    loop_ = nullptr;
}

} // namespace gritty::platform
