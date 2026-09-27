#include "lle/protocol/encoder.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lle::protocol {
namespace {

TEST(EncoderTest, EncodesKnownSingleEventPacket) {
    constexpr std::array payload{std::byte{0xde}, std::byte{0xad}, std::byte{0xbe}};
    const EventView event{0x31323334, 0x1112131415161718ULL, 0x2122232425262728ULL, payload};
    std::array<std::byte, 50> output;
    output.fill(std::byte{0xa5});

    const auto result = encode_data(event, 0x01020304, 0x0102030405060708ULL, output);

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(*result, 48U);
    constexpr std::array expected{
        std::byte{0x4c}, std::byte{0x11}, std::byte{0x30}, std::byte{0x00},
        std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01},
        std::byte{0x08}, std::byte{0x07}, std::byte{0x06}, std::byte{0x05},
        std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01},
        std::byte{0x18}, std::byte{0x17}, std::byte{0x16}, std::byte{0x15},
        std::byte{0x14}, std::byte{0x13}, std::byte{0x12}, std::byte{0x11},
        std::byte{0x28}, std::byte{0x27}, std::byte{0x26}, std::byte{0x25},
        std::byte{0x24}, std::byte{0x23}, std::byte{0x22}, std::byte{0x21},
        std::byte{0x34}, std::byte{0x33}, std::byte{0x32}, std::byte{0x31},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x03}, std::byte{0x00}, std::byte{0xde}, std::byte{0xad},
        std::byte{0xbe}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    };
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(output[i], expected[i]) << "byte " << i;
    }
    EXPECT_EQ(output[48], std::byte{0xa5});
    EXPECT_EQ(output[49], std::byte{0xa5});
}

TEST(EncoderTest, RejectsInvalidInputWithoutWriting) {
    constexpr std::array payload{std::byte{0x42}};
    EventView event{7, 8, 9, payload};
    std::array<std::byte, kMaxPacketBytes> output;
    output.fill(std::byte{0xa5});

    auto result = encode_data(event, 0, 1, output);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), EncodeError::InvalidSessionId);

    result = encode_data(event, 1, 0, output);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), EncodeError::InvalidPacketSequence);

    event.sequence = 0;
    result = encode_data(event, 1, 1, output);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), EncodeError::InvalidEventSequence);
    event.sequence = 8;

    result = encode_data(event, 1, 1, std::span{output}.first(43));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), EncodeError::OutputTooSmall);

    std::array<std::byte, kMaxSingleEventPayloadBytes + 1> oversized{};
    event.payload = oversized;
    result = encode_data(event, 1, 1, output);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), EncodeError::PayloadTooLarge);

    for (const auto byte : output) {
        EXPECT_EQ(byte, std::byte{0xa5});
    }
}

TEST(EncoderTest, AcceptsEmptyAndMaximumPayload) {
    EventView event{7, 8, 9, {}};
    std::array<std::byte, kMaxPacketBytes> output;
    output.fill(std::byte{0xa5});

    auto result = encode_data(event, 1, 1, std::span{output}.first(kMinDataPacketBytes));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, kMinDataPacketBytes);
    EXPECT_EQ(output[40], std::byte{0});
    EXPECT_EQ(output[41], std::byte{0});
    EXPECT_EQ(output[42], std::byte{0});
    EXPECT_EQ(output[43], std::byte{0});
    EXPECT_EQ(output[44], std::byte{0xa5});

    std::array<std::byte, kMaxSingleEventPayloadBytes> payload;
    payload.fill(std::byte{0x5a});
    event.payload = payload;
    result = encode_data(event, 1, 2, output);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, kMaxPacketBytes);
    EXPECT_EQ(output[2], std::byte{0x88});
    EXPECT_EQ(output[3], std::byte{0x05});
    EXPECT_EQ(output[40], std::byte{0x5e});
    EXPECT_EQ(output[41], std::byte{0x05});
    EXPECT_EQ(output.back(), std::byte{0x5a});
}

} // namespace
} // namespace lle::protocol
