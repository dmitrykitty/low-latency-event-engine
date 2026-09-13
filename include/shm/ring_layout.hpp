#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace lle::shm {

inline constexpr std::size_t ring_cache_line_bytes = 64;
inline constexpr std::uint16_t ring_layout_version = 1;
inline constexpr std::uint16_t ring_header_bytes = 192;
inline constexpr std::size_t ring_slot_metadata_bytes = 32;
inline constexpr std::array ring_magic{'L', 'L', 'E', '1'};

enum class RingState : std::uint32_t {
    Uninitialized = 0,
    Initializing = 1,
    Ready = 2,
    Closed = 3
};

// These objects live inside the mapping. Only the creator constructs them.
// Store RingState as its underlying integer in state.
struct alignas(ring_cache_line_bytes) RingPreamble {
    std::array<char, 4> magic;
    std::uint16_t layout_version;
    std::uint16_t header_bytes;
    std::uint64_t segment_bytes;
    std::uint32_t slot_count;
    std::uint32_t slot_stride;
    std::uint32_t slot_payload_capacity;
    std::uint32_t reserved;
    std::uint64_t instance_id;
    std::atomic<std::uint32_t> state;
    std::array<std::byte, 20> reserved_bytes;
};

struct alignas(ring_cache_line_bytes) RingCursor {
    std::atomic<std::uint64_t> position;
    std::array<std::byte, 56> reserved_bytes;
};

struct alignas(ring_cache_line_bytes) RingHeader {
    RingPreamble preamble;
    RingCursor producer;
    RingCursor consumer;
};

// Payload follows immediately; slot stride rounds metadata + capacity up to 64.
struct RingSlotHeader {
    std::uint64_t event_sequence;
    std::uint64_t source_timestamp_ns;
    std::uint32_t stream_id;
    std::uint32_t payload_length;
    std::array<std::uint32_t, 2> reserved;
};

static_assert(std::endian::native == std::endian::little);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::is_standard_layout_v<RingPreamble>);
static_assert(std::is_standard_layout_v<RingHeader>);
static_assert(std::is_standard_layout_v<RingSlotHeader>);
static_assert(sizeof(RingPreamble) == 64);
static_assert(alignof(RingPreamble) == 64);
static_assert(offsetof(RingPreamble, layout_version) == 4);
static_assert(offsetof(RingPreamble, header_bytes) == 6);
static_assert(offsetof(RingPreamble, segment_bytes) == 8);
static_assert(offsetof(RingPreamble, slot_count) == 16);
static_assert(offsetof(RingPreamble, slot_stride) == 20);
static_assert(offsetof(RingPreamble, slot_payload_capacity) == 24);
static_assert(offsetof(RingPreamble, reserved) == 28);
static_assert(offsetof(RingPreamble, instance_id) == 32);
static_assert(offsetof(RingPreamble, state) == 40);
static_assert(offsetof(RingPreamble, reserved_bytes) == 44);
static_assert(sizeof(RingCursor) == 64);
static_assert(alignof(RingCursor) == 64);
static_assert(sizeof(RingHeader) == ring_header_bytes);
static_assert(offsetof(RingHeader, producer) == 64);
static_assert(offsetof(RingHeader, consumer) == 128);
static_assert(sizeof(RingSlotHeader) == ring_slot_metadata_bytes);
static_assert(offsetof(RingSlotHeader, source_timestamp_ns) == 8);
static_assert(offsetof(RingSlotHeader, stream_id) == 16);
static_assert(offsetof(RingSlotHeader, payload_length) == 20);
static_assert(offsetof(RingSlotHeader, reserved) == 24);

} // namespace lle::shm
