#include "lle/transport/udp_sender.hpp"

#include <cerrno>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace lle::transport {

std::expected<UdpAddress, UdpError>
UdpAddress::ipv4(std::string_view address, std::uint16_t port) {
    if (port == 0) {
        return std::unexpected(UdpError{.code = UdpErrorCode::InvalidPort});
    }

    sockaddr_in native{};
    native.sin_family = AF_INET;
    native.sin_port = htons(port);
    const std::string address_text{address};
    if (inet_pton(AF_INET, address_text.c_str(), &native.sin_addr) != 1) {
        return std::unexpected(UdpError{.code = UdpErrorCode::InvalidAddress});
    }
    return UdpAddress{native};
}

std::expected<UdpSender, UdpError> UdpSender::open() noexcept {
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        return std::unexpected(
            UdpError{
            .code = UdpErrorCode::Socket,
            .error_number = errno
        });
    }
    return UdpSender{socket_fd};
}

UdpSender::UdpSender(UdpSender&& other) noexcept
    : socket_fd_(std::exchange(other.socket_fd_, -1)) {}

UdpSender::~UdpSender() {
    if (socket_fd_ >= 0) {
        close(socket_fd_);
    }
}

std::expected<void, UdpError>
UdpSender::send_to(std::span<const std::byte> datagram, const UdpAddress& destination) const noexcept {
    const auto sent = sendto(
        socket_fd_, datagram.data(), datagram.size(), 0,
        reinterpret_cast<const sockaddr*>(&destination.native), sizeof(destination.native)
    );
    if (sent < 0) {
        return std::unexpected(UdpError{.code = UdpErrorCode::Send, .error_number = errno});
    }
    if (static_cast<std::size_t>(sent) != datagram.size()) {
        return std::unexpected(UdpError{.code = UdpErrorCode::Send});
    }
    return {};
}

} // namespace lle::transport

