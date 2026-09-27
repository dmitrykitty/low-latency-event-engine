#pragma once

#include "lle/protocol/types.hpp"

#include <cstddef>
#include <expected>
#include <span>

namespace lle::protocol {

// Decode one complete LLEP v1 DATA datagram containing exactly one event.
// The returned payload points into packet and is valid only while packet stays
// alive and unchanged. No view is returned until the whole datagram is valid.
[[nodiscard]] std::expected<DecodedDataView, DecodeError>
decode_data(std::span<const std::byte> packet) noexcept;

} // namespace lle::protocol
