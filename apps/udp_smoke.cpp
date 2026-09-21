#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdexcept>
#include <string>
#include <string_view>

constexpr int STATUS_SUCCESS = 0;
constexpr int STATUS_ERROR = 1;
constexpr std::size_t BUFFER_SIZE = 1024;
constexpr int MAX_PORT_SIZE = 65535;

static int parse_port(const char* port_str) {
    int port = -1;
    const std::string_view text{port_str};
    const auto* end = text.data() + text.size();

    const auto [ptr, error] = std::from_chars(
        text.data(),
        end, //to get exactly const char*
        port
        );

    if (error != std::errc{} || ptr != end || port < 1 || port > MAX_PORT_SIZE) {
        throw std::runtime_error(std::string{port_str} + " has invalid port number");
    }
    return port;
}

int receive_dm(std::uint16_t port) {
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (socket_fd == -1) {
        std::cerr << "socket: " << std::strerror(errno) << '\n';
        return STATUS_ERROR;
    }

    sockaddr_in local_address{};

    local_address.sin_family = AF_INET;                // IPV4
    local_address.sin_port = htons(port);              // host to network short
    local_address.sin_addr.s_addr = htonl(INADDR_ANY); // host to network long 0.0.0.0

    // listen on 0.0.0.0:port

    if (bind(
            socket_fd,
            reinterpret_cast<const sockaddr*>(&local_address),
            sizeof(local_address)
        ) == -1) {
        std::cerr << "bind: " << std::strerror(errno) << '\n';
        close(socket_fd);
        return STATUS_ERROR;
    }
    std::cout << "bound to UDP port " << port << '\n';

    // if something happen
    pollfd poll_fd{};
    poll_fd.fd = socket_fd;
    poll_fd.events = POLLIN;

    const int timeout_ms = 10'000;
    const int poll_result = poll(&poll_fd, 1, timeout_ms);

    if (poll_result < 0) {
        std::cerr << "poll: " << std::strerror(errno) << '\n';
        close(socket_fd);
        return STATUS_ERROR;
    }

    if (poll_result == 0) {
        std::cerr << "timeout exceeded\n";
        close(socket_fd);
        return STATUS_ERROR;
    }

    if ((poll_fd.revents & POLLIN) == 0) {
        std::cerr << "no POLLIN even occured\n";
        close(socket_fd);
        return STATUS_ERROR;
    }

    char buff[BUFFER_SIZE];
    sockaddr_in sender_address{};
    socklen_t sender_address_len = sizeof(sender_address);

    const auto received = recvfrom(
        socket_fd,
        buff,
        BUFFER_SIZE,
        0,
        reinterpret_cast<sockaddr*>(&sender_address),
        &sender_address_len
    );

    if (received < 0) {
        std::cerr << "recvfrom: " << std::strerror(errno) << '\n';
        close(socket_fd);
        return STATUS_ERROR;
    }

    char sender_ip[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &sender_address.sin_addr, sender_ip, sizeof(sender_ip)) == nullptr) {
        std::cerr << "inet_ntop: " << std::strerror(errno) << '\n';
        close(socket_fd);
        return STATUS_ERROR;
    }

    const std::uint16_t sender_port = ntohs(sender_address.sin_port);
    std::cout << "client: received packet (" << received << " bytes) from " << sender_ip << ":"
              << sender_port << "\n\t";
    std::cout.write(buff, received);
    std::cout << '\n';

    close(socket_fd);
    return STATUS_SUCCESS;
}

int send_dm(std::uint16_t port, const char* message, const char* ip) {
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (socket_fd == -1) {
        std::cerr << "socket: " << std::strerror(errno) << '\n';
        return STATUS_ERROR;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    const int result = inet_pton(AF_INET, ip, &address.sin_addr);
    if (result < 0) {
        std::cerr << "inet_pton" << std::strerror(errno) << '\n';
        close(socket_fd);
        return STATUS_ERROR;
    }

    if (result == 0) {
        std::cerr << "invalid IPv4 address\n";
        close(socket_fd);
        return STATUS_ERROR;
    }
    const std::size_t message_len = std::strlen(message);
    constexpr std::size_t MAX_MESSAGE_SIZE = 256;

    if (message_len > MAX_MESSAGE_SIZE) {
        std::cerr << "message too large\n";
        close(socket_fd);
        return STATUS_ERROR;
    }

    const auto sent = sendto(socket_fd, message, message_len,0,
            reinterpret_cast<const sockaddr*>(&address), sizeof(address));

    if (sent < 0) {
        std::cerr << "sendto: " << std::strerror(errno) << '\n';
        close(socket_fd);
        return STATUS_ERROR;
    }

    if (static_cast<std::size_t>(sent) != message_len) {
        std::cerr << "sendto: incomplete datagram\n";
        close(socket_fd);
        return STATUS_ERROR;
    }
    std::cout << "server: message send to " << ip << ":" << port << '\n';
    close(socket_fd);
    return STATUS_SUCCESS;
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            std::cerr << "usage:\n"
                      << "  lle-udp-smoke receive <port>\n"
                      << "  lle-udp-smoke send <port> <ip> <message>\n";
            return STATUS_ERROR;
        }

        const std::string side{argv[1]};

        if (side == "receive") {
            if (argc != 3) {
                std::cerr << "usage: lle-udp-smoke receive <port>\n";
                return STATUS_ERROR;
            }
            const int port = parse_port(argv[2]);
            return receive_dm(static_cast<std::uint16_t>(port));
        }

        if (side == "send") {
            if (argc != 5) {
                std::cerr << "usage: lle-udp-smoke send <port> <ip> <message>\n";
                return STATUS_ERROR;
            }
            const int port = parse_port(argv[2]);
            return send_dm(static_cast<std::uint16_t>(port), argv[4], argv[3]);
        }

        std::cerr << "unknown mode: " << side << '\n';
        return STATUS_ERROR;

    } catch (std::exception& e) {
        std::cerr << e.what() << '\n';
        return STATUS_ERROR;
    }


}