#include "shm/spsc_ring.hpp"

#include <limits>
#include <memory>
#include <utility>

namespace lle::shm {

namespace {
std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    const auto remainder = value & (alignment - 1);
    if (remainder == 0) {
        return value;
    }
    return value + alignment - remainder;
}

// called only after initialize() validates the inputs and constructs the header.
void fill_preamble(
    RingPreamble* preamble,
    std::size_t size,
    const RingConfig& config,
    std::uint64_t instance_id
) noexcept {
    preamble->magic = ring_magic;
    preamble->layout_version = ring_layout_version;
    preamble->header_bytes = ring_header_bytes;
    preamble->segment_bytes = size;
    preamble->slot_count = config.slot_count;
    preamble->slot_stride =
        static_cast<std::uint32_t>((size - ring_header_bytes) / config.slot_count);
    preamble->slot_payload_capacity = config.slot_payload_capacity;
    preamble->reserved = 0;
    preamble->instance_id = instance_id;
    preamble->reserved_bytes.fill(std::byte{0});
    // no other process may access the ring during construction.
    preamble->state.store(
        static_cast<std::uint32_t>(RingState::Initializing),
        std::memory_order_relaxed
    );
}
} // namespace

std::expected<std::size_t, RingError>
SpscRing::required_bytes(const RingConfig& config) noexcept {
    const auto slot_count = config.slot_count;

    if (slot_count < 2 || (slot_count & (slot_count - 1)) != 0) {
        return std::unexpected(RingError::InvalidConfig);
    }

    constexpr auto max_size = std::numeric_limits<std::size_t>::max();
    const std::size_t slot_bytes = ring_slot_metadata_bytes + config.slot_payload_capacity;
    if (slot_bytes > max_size - (ring_cache_line_bytes - 1)) {
        return std::unexpected(RingError::SizeOverflow);
    }
    const std::size_t stride = align_up(slot_bytes, ring_cache_line_bytes);

    if (stride > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(RingError::SizeOverflow);
    }

    if (slot_count > (max_size - ring_header_bytes) / stride) {
        return std::unexpected(RingError::SizeOverflow);
    }

    return ring_header_bytes + stride * slot_count;
}

std::expected<SpscRing, RingError> SpscRing::initialize(
    std::span<std::byte> memory,
    const RingConfig& config,
    std::uint64_t instance_id
) noexcept {
    // 0 id is invalid by design
    if (instance_id == 0) {
        return std::unexpected(RingError::InvalidConfig);
    }
    auto maybe_size = required_bytes(config);
    if (!maybe_size) {
        return std::unexpected(maybe_size.error());
    }

    const std::size_t size = *maybe_size;
    // according my ABI exact size required
    if (size != memory.size() || memory.data() == nullptr ||
        reinterpret_cast<std::uintptr_t>(memory.data()) % alignof(RingHeader) != 0) {
        return std::unexpected(RingError::InvalidMemory);
    }

    // start the lifetimes of the header and its atomic members in fresh storage.
    auto* header = std::construct_at(reinterpret_cast<RingHeader*>(memory.data()));
    fill_preamble(&header->preamble, size, config, instance_id);
    //because ring is in Initializing state - relaxed is acceptable
    header->producer.position.store(0, std::memory_order_relaxed);
    header->consumer.position.store(0, std::memory_order_relaxed);

    auto* slots = memory.data() + ring_header_bytes;
    for (std::size_t index = 0; index < config.slot_count; ++index) {
        auto* slot = slots + index * header->preamble.slot_stride;
        std::construct_at(reinterpret_cast<RingSlotHeader*>(slot));
    }

    // Publish all metadata and initialized slots to an acquire-loading attacher.
    header->preamble.state.store(
        static_cast<std::uint32_t>(RingState::Ready),
        std::memory_order_release
    );
    return SpscRing{header, slots, true};
}

SpscRing::SpscRing(SpscRing&& other) noexcept
    : header_(std::exchange(other.header_, nullptr)),
      slots_(std::exchange(other.slots_, nullptr)),
      acquired_position_(std::exchange(other.acquired_position_, 0)),
      acquired_(std::exchange(other.acquired_, false)),
      initializer_(std::exchange(other.initializer_, false)) {}

SpscRing::SpscRing(RingHeader* header, std::byte* slots, bool initializer) noexcept
    : header_(header),
      slots_(slots),
      initializer_(initializer) {}

} // namespace lle::shm
