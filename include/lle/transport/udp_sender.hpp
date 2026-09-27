#pragma once

#include "lle/transport/udp_types.hpp"

#include <cstddef>
#include <expected>
#include <span>

namespace lle::transport {

class UdpSender {
public:
    [[nodiscard]] static std::expected<UdpSender, UdpError> open() noexcept;

    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;
    UdpSender(UdpSender&& other) noexcept;
    UdpSender& operator=(UdpSender&&) = delete;
    ~UdpSender();

    [[nodiscard]] std::expected<void, UdpError>
    send_to(std::span<const std::byte> datagram, const UdpAddress& destination) const noexcept;

private:
    explicit UdpSender(int socket_fd) noexcept : socket_fd_(socket_fd) {}

    int socket_fd_{-1};
};

} // namespace lle::transport
