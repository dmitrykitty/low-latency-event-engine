#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>

#define STATUS_SUCCESS (0)
#define STATUS_ERROR (1)
#define BUFFER_SIZE (1024)

int receive_dm(std::uint16_t port) {
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (socket_fd == -1) {
        std::cerr << "socket: " << std::strerror(errno) << '\n';
        return STATUS_ERROR;
    }

    sockaddr_in address{};

    address.sin_family = AF_INET;                // IPV4
    address.sin_port = htons(port);              // host to network short
    address.sin_addr.s_addr = htonl(INADDR_ANY); // host to network long 0.0.0.0

    // listen on 0.0.0.0:port

    if (bind(socket_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == -1) {
        std::cerr << "bind: " << std::strerror(errno) << '\n';
        close(socket_fd);
        return STATUS_ERROR;
    }
    std::cout << "bound to UDP port " << port << '\n';

    char buff[BUFFER_SIZE];
    socklen_t add_len = sizeof(address);

    const auto received = recvfrom(socket_fd, buff,BUFFER_SIZE,
    0, reinterpret_cast<sockaddr*>(&address), &add_len);

    if (received < 0) {
        std::cerr << "recvfrom: " << std::strerror(errno) << '\n';
        close(socket_fd);
        return STATUS_ERROR;
    }

    std::cout << "client: received message <";
    std::cout.write(buff, received);
    std::cout << ">\n";

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
        std::cerr << "invali ip\n";
        close(socket_fd);
        return STATUS_ERROR;
    }
    const std::size_t message_len = std::strlen(message);

    if (long sent = sendto(
            socket_fd,
            message,
            message_len,
            0,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)); sent < 0 || sent != message_len) {
        std::cerr << "sendto: " << std::strerror(errno) << '\n';
        close(socket_fd);
        return STATUS_ERROR;
    }
    std::cout << "server: message send to " << ip << ":" << port << '\n';
    close(socket_fd);
    return STATUS_SUCCESS;
}

int main(int argc, char* argv[]) {
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
        const int port = std::atoi(argv[2]);
        return receive_dm(static_cast<std::uint16_t>(port));
    }

    if (side == "send") {
        if (argc != 5) {
            std::cerr << "usage: lle-udp-smoke send <port> <ip> <message>\n";
            return STATUS_ERROR;
        }
        const int port = std::atoi(argv[2]);
        return send_dm(static_cast<std::uint16_t>(port), argv[4], argv[3]);
    }

    std::cerr << "unknown mode: " << side << '\n';
    return STATUS_ERROR;
}