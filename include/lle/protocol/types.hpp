#pragma once

#include "lle/event.hpp"

#include <cstddef>
#include <cstdint>

namespace lle::protocol {

// LLEP v1 wire constants. All multi-byte fields are encoded little-endian.
inline constexpr std::uint8_t kMagic = 0x4c;
inline constexpr std::uint8_t kVersion = 1;

enum class MessageType : std::uint8_t {
    Invalid = 0,
    Data = 1,
    Nack = 2,
};

inline constexpr std::size_t kCommonHeaderBytes = 8;
inline constexpr std::size_t kDataPacketHeaderBytes = 16;
inline constexpr std::size_t kDataLevelBytes = 20;
inline constexpr std::size_t kDataFixedBytes = 36;
inline constexpr std::size_t kEventFrameHeaderBytes = 6;
inline constexpr std::size_t kNackRangeBytes = 10;
inline constexpr std::size_t kEventFrameAlignmentBytes = 4;

inline constexpr std::size_t kMaxPacketBytes = 1416;
inline constexpr std::size_t kMinDataPacketBytes = 44;
inline constexpr std::size_t kMaxSingleEventPayloadBytes = 1374;
inline constexpr std::size_t kMaxNackRanges = 64;
inline constexpr std::size_t kMaxNackRequestedPackets = 4096;

static_assert(kDataPacketHeaderBytes + kDataLevelBytes == kDataFixedBytes);
static_assert(kDataFixedBytes + kEventFrameHeaderBytes + 2 == kMinDataPacketBytes);
static_assert(kDataFixedBytes + kEventFrameHeaderBytes + kMaxSingleEventPayloadBytes ==
              kMaxPacketBytes);

enum class EncodeError : std::uint8_t {
    InvalidSessionId,
    InvalidPacketSequence,
    InvalidEventSequence,
    PayloadTooLarge,
    OutputTooSmall,
};

enum class DecodeError : std::uint8_t {
    Truncated,
    InvalidMagic,
    UnsupportedVersion,
    UnsupportedType,
    PacketLengthMismatch,
    PacketTooLarge,
    InvalidSessionId,
    InvalidPacketSequence,
    InvalidEventSequence,
    MalformedFrame,
    NonZeroPadding,
    ArithmeticOverflow,
};

// One-event DATA. The event payload refers to the
// caller-owned packet buffer and remains valid only while that buffer is alive
// and unchanged. Packet identity is transport metadata, not part of EventView.
struct DecodedDataView {
    std::uint32_t session_id{};
    Sequence packet_sequence{};
    EventView event{};
};

} // namespace lle::protocol
