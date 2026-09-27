#pragma once

#include "lle/protocol/types.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace lle::protocol {

// Encode exactly one event as an LLEP v1 DATA packet into caller-owned storage.
// On success, return the number of packet bytes written to output.
// Reject zero session/packet/event sequences, payloads above the protocol limit,
// and output buffers too small for the complete padded packet.
[[nodiscard]] std::expected<std::size_t, EncodeError> encode_data(
    const EventView& event,
    std::uint32_t session_id,
    std::uint64_t packet_sequence,
    std::span<std::byte> output
) noexcept;

} // namespace lle::protocol
