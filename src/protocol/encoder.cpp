#include <algorithm>

#include "lle/detail/alignment.hpp"
#include "lle/protocol/encoder.hpp"

#include "byte_io.hpp"

namespace lle::protocol {

[[nodiscard]] std::expected<std::size_t, EncodeError> encode_data(
    const EventView& event,
    std::uint32_t session_id,
    std::uint64_t packet_sequence,
    std::span<std::byte> output
) noexcept {
    if (session_id == 0) {
        return std::unexpected(EncodeError::InvalidSessionId);
    }

    if (packet_sequence == 0) {
        return std::unexpected(EncodeError::InvalidPacketSequence);
    }

    if (event.sequence == 0) {
        return std::unexpected(EncodeError::InvalidEventSequence);
    }

    if (event.payload.size() > kMaxSingleEventPayloadBytes) {
        return std::unexpected(EncodeError::PayloadTooLarge);
    }

    const std::size_t payload_size = event.payload.size();
    const std::size_t frame_size =
        detail::align_up(payload_size + kEventFrameHeaderBytes, kEventFrameAlignmentBytes);
    const std::size_t packet_size = frame_size + kDataFixedBytes;

    if (packet_size > output.size()) {
        return std::unexpected(EncodeError::OutputTooSmall);
    }

    output[0] = static_cast<std::byte>(kMagic);
    output[1] =
        static_cast<std::byte>((kVersion << 4U) | static_cast<std::uint8_t>(MessageType::Data));
    internals::write_u16(output.data() + 2, static_cast<std::uint16_t>(packet_size));
    internals::write_u32(output.data() + 4, session_id);
    internals::write_u64(output.data() + 8, packet_sequence);
    internals::write_u64(output.data() + 16, event.sequence);
    internals::write_u64(output.data() + 24, event.source_timestamp_ns);
    internals::write_u32(output.data() + 32, event.stream_id);
    internals::write_u32(output.data() + 36, 0);
    internals::write_u16(output.data() + 40, static_cast<std::uint16_t>(payload_size));
    std::ranges::copy(event.payload, output.data() + 42);
    std::fill(
        output.data() + 42 + payload_size,
        output.data() + packet_size,
        static_cast<std::byte>(0)
    );
    return packet_size;
}

} // namespace lle::protocol
