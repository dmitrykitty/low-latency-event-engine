#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

#include <netinet/in.h>

namespace lle::transport {

enum class UdpErrorCode : std::uint8_t {
    InvalidAddress,
    InvalidPort,
    Socket,
    Bind,
    Poll,
    Timeout,
    Send,
    Receive,
    DatagramTooLarge,
};

struct UdpError {
    UdpErrorCode code;
    int error_number{};
};

// Numeric IPv4 address parsed once, before sending packets.
struct UdpAddress {
    sockaddr_in native{};

    [[nodiscard]] static std::expected<UdpAddress, UdpError>
    ipv4(std::string_view address, std::uint16_t port);
};

} // namespace lle::transport
