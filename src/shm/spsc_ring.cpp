// Fixed-capacity SPSC queue implementation is scheduled for week 1.
#include <limits>

#include "shm/spsc_ring.hpp"

namespace lle::shm {

namespace {
std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    const auto remainder = value & (alignment - 1);
    if (remainder == 0) {
        return value;
    }
    return value + alignment - remainder;
}
} // namespace

std::expected<std::size_t, RingError>
SpscRing::required_bytes(const RingConfig& config) noexcept {
    const auto slot_count = config.slot_count;

    if (slot_count < 2 || (slot_count & (slot_count - 1)) != 0) {
        return std::unexpected(RingError::InvalidConfig);
    }

    const std::size_t slot_bytes = ring_slot_metadata_bytes + config.slot_payload_capacity;
    const std::size_t stride = align_up(slot_bytes, ring_cache_line_bytes);

    if (stride > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(RingError::SizeOverflow);
    }

    return ring_header_bytes + stride * slot_count;
}

SpscRing::SpscRing(RingHeader* header, std::byte* slots, bool initializer) noexcept
    : header_(header),
      slots_(slots),
      initializer_(initializer) {}

} // namespace lle::shm