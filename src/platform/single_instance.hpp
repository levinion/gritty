#pragma once

#include "platform/pal_interfaces.hpp"
#include "util/raii.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gritty::platform {

// Window creation parameters forwarded from secondary launches to the running instance.
struct LaunchRequest {
    std::string config_path;
    std::string explicit_cwd;
    std::string inherited_cwd;
    std::string title;
    std::string app_id;
    std::string font_family;
    double font_size{0.0};
    size_t cols{0};
    size_t rows{0};
    bool hold{false};
    std::vector<std::string> config_overrides;
    std::string command;
    std::vector<std::string> command_args;
};

[[nodiscard]] std::vector<uint8_t> encode_launch_request(const LaunchRequest& req);
[[nodiscard]] std::optional<LaunchRequest> decode_launch_request(std::span<const uint8_t> payload);
[[nodiscard]] std::string default_instance_socket_path();

enum class SendStatus : uint8_t {
    SentOk = 0,
    ServerError = 1,
    NoServer = 2,
};

[[nodiscard]] SendStatus try_send_launch_request(
    const std::string& socket_path,
    const LaunchRequest& req,
    std::string* error_out = nullptr
);

// Coordinates primary instance ownership and Unix-socket window forwarding.
class SingleInstanceServer final {
public:
    enum class AcquireStatus : uint8_t {
        BecamePrimary = 0,
        ForwardedOk = 1,
        ForwardedError = 2,
        Failed = 3,
    };

    using RequestHandler = std::function<std::expected<void, std::string>(const LaunchRequest&)>;

    SingleInstanceServer() = default;
    ~SingleInstanceServer();

    SingleInstanceServer(const SingleInstanceServer&) = delete;
    SingleInstanceServer& operator=(const SingleInstanceServer&) = delete;

    [[nodiscard]] AcquireStatus acquire_or_forward(
        const std::string& socket_path,
        const LaunchRequest& req,
        std::string* error_out = nullptr
    );

    void attach(IEventLoop& loop, RequestHandler handler);
    void stop();

    [[nodiscard]] bool is_primary() const noexcept { return listen_fd_.valid(); }
    [[nodiscard]] const std::string& socket_path() const noexcept { return socket_path_; }

private:
    struct ClientState {
        util::UniqueFd fd;
        std::vector<uint8_t> buffer;
    };

    void handle_accept();
    void handle_client_io(int fd, uint32_t events);
    void close_client(int fd);
    void send_client_response(int fd, bool ok, std::string_view message);

    std::string socket_path_;
    util::UniqueFd listen_fd_;
    uint64_t bound_dev_{0};
    uint64_t bound_ino_{0};
    bool has_bound_stat_{false};
    IEventLoop* loop_{nullptr};
    RequestHandler handler_;
    std::unordered_map<int, ClientState> clients_;
};

} // namespace gritty::platform
