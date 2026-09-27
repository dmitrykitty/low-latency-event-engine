#include "lle/protocol/decoder.hpp"
#include "lle/protocol/encoder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace lle::protocol {
namespace {

std::array<std::byte, 48> valid_packet() {
    std::array<std::byte, 48> packet{};
    packet[0] = std::byte{0x4c};
    packet[1] = std::byte{0x11};
    packet[2] = std::byte{0x30};
    packet[4] = std::byte{1};
    packet[8] = std::byte{1};
    packet[16] = std::byte{1};
    packet[32] = std::byte{7};
    packet[40] = std::byte{3};
    packet[42] = std::byte{0xde};
    packet[43] = std::byte{0xad};
    packet[44] = std::byte{0xbe};
    return packet;
}

void expect_error(std::span<const std::byte> packet, DecodeError error) {
    const auto result = decode_data(packet);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), error);
}

TEST(DecoderTest, ReadsKnownPacketWithPayloadViewIntoInput) {
    const auto packet = valid_packet();
    const auto result = decode_data(packet);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->session_id, 1U);
    EXPECT_EQ(result->packet_sequence, 1U);
    EXPECT_EQ(result->event.stream_id, 7U);
    EXPECT_EQ(result->event.sequence, 1U);
    EXPECT_EQ(result->event.source_timestamp_ns, 0U);
    ASSERT_EQ(result->event.payload.size(), 3U);
    EXPECT_EQ(result->event.payload.data(), packet.data() + 42);
    EXPECT_EQ(result->event.payload[0], std::byte{0xde});
    EXPECT_EQ(result->event.payload[1], std::byte{0xad});
    EXPECT_EQ(result->event.payload[2], std::byte{0xbe});
}

TEST(DecoderTest, PreservesMetadataAndPayloadThroughEncodeDecode) {
    constexpr std::array payload{
        std::byte{0x00}, std::byte{0x7f}, std::byte{0x80}, std::byte{0xff}};
    const EventView event{0x12345678, 0x0102030405060708ULL, 0xfedcba9876543210ULL, payload};
    std::array<std::byte, kMaxPacketBytes> packet{};
    const auto encoded = encode_data(event, 0xabcdef01, 0x1020304050607080ULL, packet);
    ASSERT_TRUE(encoded.has_value());

    const auto decoded = decode_data(std::span{packet}.first(*encoded));

    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->session_id, 0xabcdef01U);
    EXPECT_EQ(decoded->packet_sequence, 0x1020304050607080ULL);
    EXPECT_EQ(decoded->event.stream_id, event.stream_id);
    EXPECT_EQ(decoded->event.sequence, event.sequence);
    EXPECT_EQ(decoded->event.source_timestamp_ns, event.source_timestamp_ns);
    EXPECT_TRUE(std::ranges::equal(decoded->event.payload, event.payload));
    EXPECT_EQ(decoded->event.payload.data(), packet.data() + 42);
}

TEST(DecoderTest, RejectsInvalidHeaderAndIdentifiers) {
    auto packet = valid_packet();
    packet[0] = std::byte{0};
    expect_error(packet, DecodeError::InvalidMagic);

    packet = valid_packet();
    packet[1] = std::byte{0x21};
    expect_error(packet, DecodeError::UnsupportedVersion);

    packet = valid_packet();
    packet[1] = std::byte{0x12};
    expect_error(packet, DecodeError::UnsupportedType);

    packet = valid_packet();
    packet[2] = std::byte{0x2f};
    expect_error(packet, DecodeError::PacketLengthMismatch);

    packet = valid_packet();
    packet[4] = std::byte{0};
    expect_error(packet, DecodeError::InvalidSessionId);

    packet = valid_packet();
    packet[8] = std::byte{0};
    expect_error(packet, DecodeError::InvalidPacketSequence);

    packet = valid_packet();
    packet[16] = std::byte{0};
    expect_error(packet, DecodeError::InvalidEventSequence);
}

TEST(DecoderTest, RejectsTruncatedOversizedAndMalformedFrames) {
    auto packet = valid_packet();
    for (std::size_t length = 0; length < packet.size(); ++length) {
        EXPECT_FALSE(decode_data(std::span{packet}.first(length)).has_value()) << length;
    }
    std::array<std::byte, kMaxPacketBytes + 1> oversized{};
    expect_error(oversized, DecodeError::PacketTooLarge);

    packet[2] = std::byte{0x2c};
    expect_error(std::span{packet}.first(44), DecodeError::MalformedFrame);

    packet = valid_packet();
    packet[36] = std::byte{1};
    expect_error(packet, DecodeError::MalformedFrame);

    packet = valid_packet();
    packet[40] = std::byte{0x5f};
    packet[41] = std::byte{0x05};
    expect_error(packet, DecodeError::MalformedFrame);

    packet = valid_packet();
    packet[45] = std::byte{1};
    expect_error(packet, DecodeError::NonZeroPadding);
}

TEST(DecoderTest, AcceptsEmptyAndMaximumPayload) {
    EventView event{7, 8, 9, {}};
    std::array<std::byte, kMaxPacketBytes> packet{};
    auto encoded = encode_data(event, 1, 1, packet);
    ASSERT_TRUE(encoded.has_value());
    auto decoded = decode_data(std::span{packet}.first(*encoded));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(decoded->event.payload.empty());

    std::array<std::byte, kMaxSingleEventPayloadBytes> payload;
    payload.fill(std::byte{0x5a});
    event.payload = payload;
    encoded = encode_data(event, 1, 2, packet);
    ASSERT_TRUE(encoded.has_value());
    decoded = decode_data(std::span{packet}.first(*encoded));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->event.payload.size(), kMaxSingleEventPayloadBytes);
    EXPECT_TRUE(std::ranges::equal(decoded->event.payload, payload));
}

TEST(DecoderTest, RejectsBatchUntilMultipleEventViewsAreSupported) {
    std::array<std::byte, 56> packet{};
    const auto first = valid_packet();
    std::ranges::copy(first, packet.begin());
    packet[2] = std::byte{56};
    expect_error(packet, DecodeError::UnsupportedBatch);
}

} // namespace
} // namespace lle::protocol
