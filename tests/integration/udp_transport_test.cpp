#include "lle/protocol/decoder.hpp"
#include "lle/protocol/encoder.hpp"
#include "lle/transport/udp_receiver.hpp"
#include "lle/transport/udp_sender.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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

TEST(UdpTransportTest, ProtocolDataSurvivesUdpLoopback) {
    auto receiver = UdpReceiver::bind(0);
    ASSERT_TRUE(receiver.has_value());
    auto destination = UdpAddress::ipv4("127.0.0.1", receiver->local_port());
    ASSERT_TRUE(destination.has_value());
    auto sender = UdpSender::open();
    ASSERT_TRUE(sender.has_value());

    constexpr std::array payload{
        std::byte{0x00}, std::byte{0x7f}, std::byte{0x80}, std::byte{0xff}
    };
    const EventView event{42, 123, 456, payload};
    constexpr std::uint32_t session_id = 7;
    constexpr std::uint64_t packet_sequence = 9;
    std::array<std::byte, protocol::kMaxPacketBytes> send_buffer{};
    const auto encoded = protocol::encode_data(event, session_id, packet_sequence, send_buffer);
    ASSERT_TRUE(encoded.has_value());

    ASSERT_TRUE(sender->send_to(std::span{send_buffer}.first(*encoded), *destination).has_value());

    std::array<std::byte, protocol::kMaxPacketBytes> receive_buffer{};
    const auto datagram = receiver->receive(receive_buffer, 1000);
    ASSERT_TRUE(datagram.has_value());
    ASSERT_EQ(datagram->size, *encoded);

    const auto decoded = protocol::decode_data(std::span{receive_buffer}.first(datagram->size));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->session_id, session_id);
    EXPECT_EQ(decoded->packet_sequence, packet_sequence);
    EXPECT_EQ(decoded->event.stream_id, event.stream_id);
    EXPECT_EQ(decoded->event.sequence, event.sequence);
    EXPECT_EQ(decoded->event.source_timestamp_ns, event.source_timestamp_ns);
    EXPECT_TRUE(std::ranges::equal(decoded->event.payload, event.payload));
    EXPECT_EQ(decoded->event.payload.data(), receive_buffer.data() + 42);
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
