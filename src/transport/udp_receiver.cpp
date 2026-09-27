#include "lle/transport/udp_receiver.hpp"

#include <cerrno>
#include <utility>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace lle::transport {

std::expected<UdpReceiver, UdpError> UdpReceiver::bind(std::uint16_t port) noexcept {
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        return std::unexpected(UdpError{
            .code = UdpErrorCode::Socket,
            .error_number = errno
        });
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(socket_fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) < 0) {
        const int error_number = errno;
        close(socket_fd);
        return std::unexpected(UdpError{
            .code = UdpErrorCode::Bind,
            .error_number = error_number
        });
    }

    socklen_t local_size = sizeof(local);
    if (getsockname(socket_fd, reinterpret_cast<sockaddr*>(&local), &local_size) < 0) {
        const int error_number = errno;
        ::close(socket_fd);
        return std::unexpected(UdpError{UdpErrorCode::Bind, error_number});
    }
    return UdpReceiver{socket_fd, ntohs(local.sin_port)};
}

UdpReceiver::UdpReceiver(UdpReceiver&& other) noexcept
    : socket_fd_(std::exchange(other.socket_fd_, -1)), local_port_(other.local_port_) {}

UdpReceiver::~UdpReceiver() {
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
    }
}

std::expected<ReceivedDatagram, UdpError>
UdpReceiver::receive(std::span<std::byte> buffer, int timeout_ms) const noexcept {
    if (timeout_ms >= 0) {
        pollfd descriptor{socket_fd_, POLLIN, 0};
        const int ready = poll(&descriptor, 1, timeout_ms);
        if (ready < 0) {
            return std::unexpected(UdpError{
                .code = UdpErrorCode::Poll,
                .error_number = errno
            });
        }
        if (ready == 0) {
            return std::unexpected(UdpError{.code = UdpErrorCode::Timeout});
        }
        if ((descriptor.revents & POLLIN) == 0) {
            return std::unexpected(UdpError{
                .code = UdpErrorCode::Poll,
                .error_number = EIO
            });
        }
    }

    sockaddr_in peer{};
    iovec data{buffer.data(), buffer.size()};
    msghdr message{};
    message.msg_name = &peer;
    message.msg_namelen = sizeof(peer);
    message.msg_iov = &data;
    message.msg_iovlen = 1;
    const auto received = recvmsg(socket_fd_, &message, 0);
    if (received < 0) {
        return std::unexpected(UdpError{
            .code = UdpErrorCode::Receive,
            .error_number = errno
        });
    }
    if ((message.msg_flags & MSG_TRUNC) != 0) {
        return std::unexpected(UdpError{.code = UdpErrorCode::DatagramTooLarge});
    }
    return ReceivedDatagram{static_cast<std::size_t>(received), peer};
}

} // namespace lle::transport

