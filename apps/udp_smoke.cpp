#include "lle/transport/udp_receiver.hpp"
#include "lle/transport/udp_sender.hpp"

#include <arpa/inet.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::size_t buffer_size = 1024;
constexpr std::size_t max_message_size = 256;

int parse_port(const char* text) {
    int port = -1;
    const std::string_view input{text};
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), port);
    if (error != std::errc{} || end != input.data() + input.size() || port < 1 || port > 65535) {
        throw std::runtime_error(std::string{text} + " has invalid port number");
    }
    return port;
}

void print_error(std::string_view operation, lle::transport::UdpError error) {
    using lle::transport::UdpErrorCode;
    std::cerr << operation << ": ";
    switch (error.code) {
    case UdpErrorCode::InvalidAddress: std::cerr << "invalid IPv4 address"; break;
    case UdpErrorCode::InvalidPort: std::cerr << "invalid port"; break;
    case UdpErrorCode::Socket: std::cerr << "socket failed"; break;
    case UdpErrorCode::Bind: std::cerr << "bind failed"; break;
    case UdpErrorCode::Poll: std::cerr << "poll failed"; break;
    case UdpErrorCode::Timeout: std::cerr << "timeout exceeded"; break;
    case UdpErrorCode::Send: std::cerr << "send failed"; break;
    case UdpErrorCode::Receive: std::cerr << "receive failed"; break;
    case UdpErrorCode::DatagramTooLarge: std::cerr << "datagram exceeds receive buffer"; break;
    }
    if (error.error_number != 0) {
        std::cerr << ": " << std::strerror(error.error_number);
    }
    std::cerr << '\n';
}

int receive_dm(std::uint16_t port) {
    auto receiver = lle::transport::UdpReceiver::bind(port);
    if (!receiver) {
        print_error("receive", receiver.error());
        return 1;
    }
    std::cout << "bound to UDP port " << receiver->local_port() << '\n';

    std::array<std::byte, buffer_size> buffer{};
    const auto datagram = receiver->receive(buffer, 10'000);
    if (!datagram) {
        print_error("receive", datagram.error());
        return 1;
    }

    char sender_ip[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &datagram->peer.sin_addr, sender_ip, sizeof(sender_ip)) == nullptr) {
        std::cerr << "inet_ntop: " << std::strerror(errno) << '\n';
        return 1;
    }
    std::cout << "client: received packet (" << datagram->size << " bytes) from " << sender_ip
              << ':' << ntohs(datagram->peer.sin_port) << "\n\t";
    std::cout.write(
        reinterpret_cast<const char*>(buffer.data()),
        static_cast<std::streamsize>(datagram->size)
    );
    std::cout << '\n';
    return 0;
}

int send_dm(std::uint16_t port, const char* ip, const char* message) {
    const std::size_t message_size = std::strlen(message);
    if (message_size > max_message_size) {
        std::cerr << "message too large\n";
        return 1;
    }

    const auto destination = lle::transport::UdpAddress::ipv4(ip, port);
    if (!destination) {
        print_error("send", destination.error());
        return 1;
    }
    auto sender = lle::transport::UdpSender::open();
    if (!sender) {
        print_error("send", sender.error());
        return 1;
    }
    const auto bytes = std::as_bytes(std::span{message, message_size});
    const auto sent = sender->send_to(bytes, *destination);
    if (!sent) {
        print_error("send", sent.error());
        return 1;
    }
    std::cout << "server: message send to " << ip << ':' << port << '\n';
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            std::cerr << "usage:\n"
                      << "  lle-udp-smoke receive <port>\n"
                      << "  lle-udp-smoke send <port> <ip> <message>\n";
            return 1;
        }

        const std::string_view mode{argv[1]};
        if (mode == "receive" && argc == 3) {
            return receive_dm(static_cast<std::uint16_t>(parse_port(argv[2])));
        }
        if (mode == "send" && argc == 5) {
            return send_dm(static_cast<std::uint16_t>(parse_port(argv[2])), argv[3], argv[4]);
        }
        std::cerr << "invalid mode or arguments\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
