#include "lle/protocol/decoder.hpp"

#include "lle/detail/alignment.hpp"

#include "byte_io.hpp"

#include <algorithm>

namespace lle::protocol {

std::expected<DecodedDataView, DecodeError>
decode_data(std::span<const std::byte> packet) noexcept {
    if (packet.size() < kCommonHeaderBytes) {
        return std::unexpected(DecodeError::Truncated);
    }
    if (packet.size() > kMaxPacketBytes) {
        return std::unexpected(DecodeError::PacketTooLarge);
    }
    if (packet[0] != static_cast<std::byte>(kMagic)) {
        return std::unexpected(DecodeError::InvalidMagic);
    }

    const auto version_and_type = std::to_integer<std::uint8_t>(packet[1]);
    if ((version_and_type >> 4U) != kVersion) {
        return std::unexpected(DecodeError::UnsupportedVersion);
    }
    if ((version_and_type & 0x0fU) != static_cast<std::uint8_t>(MessageType::Data)) {
        return std::unexpected(DecodeError::UnsupportedType);
    }
    if (internals::read_u16(packet.data() + 2) != packet.size()) {
        return std::unexpected(DecodeError::PacketLengthMismatch);
    }

    const auto session_id = internals::read_u32(packet.data() + 4);
    if (session_id == 0) {
        return std::unexpected(DecodeError::InvalidSessionId);
    }
    if (packet.size() < kMinDataPacketBytes) {
        return std::unexpected(DecodeError::Truncated);
    }

    const auto packet_sequence = internals::read_u64(packet.data() + 8);
    if (packet_sequence == 0) {
        return std::unexpected(DecodeError::InvalidPacketSequence);
    }
    const auto event_sequence = internals::read_u64(packet.data() + 16);
    if (event_sequence == 0) {
        return std::unexpected(DecodeError::InvalidEventSequence);
    }
    const auto source_timestamp_ns = internals::read_u64(packet.data() + 24);
    const auto stream_id = internals::read_u32(packet.data() + 32);
    if (internals::read_u32(packet.data() + 36) != 0) {
        return std::unexpected(DecodeError::MalformedFrame);
    }

    const std::size_t payload_size = internals::read_u16(packet.data() + 40);
    if (payload_size > kMaxSingleEventPayloadBytes) {
        return std::unexpected(DecodeError::MalformedFrame);
    }
    const std::size_t frame_size =
        detail::align_up(kEventFrameHeaderBytes + payload_size, kEventFrameAlignmentBytes);
    if (!internals::has_bytes(packet.size(), kDataFixedBytes, frame_size)) {
        return std::unexpected(DecodeError::MalformedFrame);
    }

    const std::size_t payload_offset = kDataFixedBytes + kEventFrameHeaderBytes;
    const auto padding = packet.subspan(
        payload_offset + payload_size,
        kDataFixedBytes + frame_size - (payload_offset + payload_size)
    );
    if (std::ranges::all_of(padding, [](std::byte value) { return value == std::byte{0}; })) {
        return std::unexpected(DecodeError::NonZeroPadding);
    }

    if (packet.size() != kDataFixedBytes + frame_size) {
        return std::unexpected(DecodeError::UnsupportedBatch);
    }

    return DecodedDataView{
        .session_id = session_id,
        .packet_sequence = packet_sequence,
        .event = EventView{
            .stream_id = stream_id,
            .sequence = event_sequence,
            .source_timestamp_ns = source_timestamp_ns,
            .payload = packet.subspan(payload_offset, payload_size),
        },
    };
}

} // namespace lle::protocol
