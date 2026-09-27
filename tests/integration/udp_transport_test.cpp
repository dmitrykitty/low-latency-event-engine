#include "lle/transport/udp_receiver.hpp"
#include "lle/transport/udp_sender.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace lle::transport {
namespace {

TEST(UdpTransportTest, SendsAndReceivesBinaryDatagram) {
    auto receiver = UdpReceiver::bind(0);
    ASSERT_TRUE(receiver.has_value());
    ASSERT_NE(receiver->local_port(), 0);

    auto destination = UdpAddress::ipv4("127.0.0.1", receiver->local_port());
    ASSERT_TRUE(destination.has_value());
    auto sender = UdpSender::open();
    ASSERT_TRUE(sender.has_value());

    constexpr std::array payload{
        std::byte{0x00}, std::byte{0x41}, std::byte{0x80}, std::byte{0xff}
    };
    ASSERT_TRUE(sender->send_to(payload, *destination).has_value());

    std::array<std::byte, 16> buffer{};
    const auto datagram = receiver->receive(buffer, 1000);
    ASSERT_TRUE(datagram.has_value());
    ASSERT_EQ(datagram->size, payload.size());
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(), buffer.begin()));
    EXPECT_EQ(datagram->peer.sin_family, AF_INET);
}

TEST(UdpTransportTest, RejectsTruncatedDatagramAndCanReceiveNextOne) {
    auto receiver = UdpReceiver::bind(0);
    ASSERT_TRUE(receiver.has_value());
    auto destination = UdpAddress::ipv4("127.0.0.1", receiver->local_port());
    ASSERT_TRUE(destination.has_value());
    auto sender = UdpSender::open();
    ASSERT_TRUE(sender.has_value());

    constexpr std::array payload{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}
    };
    ASSERT_TRUE(sender->send_to(payload, *destination).has_value());

    std::array<std::byte, 4> buffer{};
    const auto oversized = receiver->receive(buffer, 1000);
    ASSERT_FALSE(oversized.has_value());
    EXPECT_EQ(oversized.error().code, UdpErrorCode::DatagramTooLarge);

    ASSERT_TRUE(sender->send_to(std::span(payload).first(4), *destination).has_value());
    const auto next = receiver->receive(buffer, 1000);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->size, buffer.size());
    EXPECT_EQ(buffer, (std::array{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}}));
}

TEST(UdpTransportTest, RejectsInvalidDestinationAtSetup) {
    const auto invalid_ip = UdpAddress::ipv4("not-an-ip", 9000);
    ASSERT_FALSE(invalid_ip.has_value());
    EXPECT_EQ(invalid_ip.error().code, UdpErrorCode::InvalidAddress);

    const auto invalid_port = UdpAddress::ipv4("127.0.0.1", 0);
    ASSERT_FALSE(invalid_port.has_value());
    EXPECT_EQ(invalid_port.error().code, UdpErrorCode::InvalidPort);
}

} // namespace
} // namespace lle::transport
