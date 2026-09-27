#pragma once

#include "lle/transport/udp_types.hpp"

#include <cstddef>
#include <expected>
#include <span>

namespace lle::transport {

struct ReceivedDatagram {
    std::size_t size{};
    sockaddr_in peer{};
};

class UdpReceiver {
public:
    // Bind to all local IPv4 addresses. Port 0 requests an ephemeral test port.
    [[nodiscard]] static std::expected<UdpReceiver, UdpError> bind(std::uint16_t port) noexcept;

    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;
    UdpReceiver(UdpReceiver&& other) noexcept;
    UdpReceiver& operator=(UdpReceiver&&) = delete;
    ~UdpReceiver();

    [[nodiscard]] std::uint16_t local_port() const noexcept { return local_port_; }

    // timeout_ms < 0 blocks indefinitely; otherwise return Timeout when it expires.
    [[nodiscard]] std::expected<ReceivedDatagram, UdpError>
    receive(std::span<std::byte> buffer, int timeout_ms = -1) noexcept;

private:
    UdpReceiver(int socket_fd, std::uint16_t local_port) noexcept
        : socket_fd_(socket_fd), local_port_(local_port) {}

    int socket_fd_{-1};
    std::uint16_t local_port_{};
};

} // namespace lle::transport
