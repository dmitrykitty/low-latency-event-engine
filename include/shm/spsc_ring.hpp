#pragma once

#include "lle/event.hpp"
#include "shm/ring_layout.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>

namespace lle::shm {

struct RingConfig {
    std::uint32_t slot_count;
    std::uint32_t slot_payload_capacity;
};

enum class RingError : std::uint8_t {
    InvalidConfig,
    SizeOverflow,
    InvalidMemory,
    InvalidLayout,
    NotReady
};

enum class AcquireError : std::uint8_t {
    Empty,
    Closed, // Closed and no events remain.
    AlreadyAcquired,
    InvalidSlot
};

// Uses memory owned by SharedMemorySegment. Keep that segment open while using the ring.
// One producer and one consumer, each with its own ring handle.
// Each handle occupies whole cache lines, so a producer handle and a consumer handle kept
// in one process (threads, tests) never share a line; their per-event writes stay local.
class alignas(ring_cache_line_bytes) SpscRing {
  public:
    // Calculate the segment size. slot_count must be a power of two and at least 2.
    static std::expected<std::size_t, RingError>
    required_bytes(const RingConfig& config) noexcept;

    // Creator: initialize fresh memory once. instance_id must be random and nonzero.
    static std::expected<SpscRing, RingError> initialize(
        std::span<std::byte> memory,
        const RingConfig& config,
        std::uint64_t instance_id
    ) noexcept;

    // open an existing ring after the creator signals initialization is complete.
    static std::expected<SpscRing, RingError>
    attach(std::span<std::byte> memory) noexcept;

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    SpscRing(SpscRing&& other) noexcept;
    SpscRing& operator=(SpscRing&& other) = delete;
    ~SpscRing() = default; // Does not release a slot or unmap memory.

    // Producer: write one event, or return Full without waiting.
    PublishResult try_publish(const EventView& event) noexcept;

    // Consumer: read one event. Its payload stays valid until release().
    std::expected<EventView, AcquireError> try_acquire() noexcept;

    // Consumer: allow reuse of the slot. Returns false if nothing was acquired.
    bool release() noexcept;

    // Creator: call after stopping the producer. Queued events remain readable.
    // Returns false for an attached handle; repeated calls by the creator succeed.
    bool close_publication() noexcept;

  private:
    SpscRing(RingHeader* header, std::byte* slots, bool initializer) noexcept;

    RingHeader* header_{nullptr};
    std::byte* slots_{nullptr};
    // geometry is validated once and copied so the hot path never rereads shared metadata.
    std::uint32_t slot_count_{0};
    std::uint32_t slot_stride_{0};
    std::uint32_t slot_payload_capacity_{0};
    // last observed peer cursor; a stale value is a conservative lower bound.
    std::uint64_t cached_read_position_{0};  // producer side
    std::uint64_t cached_write_position_{0}; // consumer side
    std::uint64_t acquired_position_{0};
    bool acquired_{false};
    bool initializer_{false};
};

static_assert(alignof(SpscRing) == ring_cache_line_bytes);
static_assert(sizeof(SpscRing) % ring_cache_line_bytes == 0);

} // namespace lle::shm

namespace lle::detail {
// hot-path helpers; kept out of lle::shm so they do not shadow lle::detail in ring code.
inline void fill_slot_header(shm::RingSlotHeader* header, const EventView& event) noexcept {
    header->event_sequence = event.sequence;
    header->source_timestamp_ns = event.source_timestamp_ns;
    header->stream_id = event.stream_id;
    header->payload_length = static_cast<std::uint32_t>(event.payload.size());
}

inline bool
invalid_slot(const shm::RingSlotHeader& slot, std::uint32_t payload_capacity) noexcept {
    return slot.payload_length > payload_capacity || slot.reserved[0] != 0 ||
           slot.reserved[1] != 0;
}
} // namespace lle::detail

namespace lle::shm {

// The per-event operations are defined inline here, like rigtorp and Boost, so callers
// can inline them without link-time optimization. Setup and validation stay in spsc_ring.cpp.

inline PublishResult SpscRing::try_publish(const EventView& event) noexcept {
    if (header_ == nullptr) {
        return PublishResult::Closed;
    }
    const auto state = header_->preamble.state.load(std::memory_order::acquire);

    if (state != static_cast<std::uint32_t>(RingState::Ready)) {
        return PublishResult::Closed;
    }

    if (event.payload.size() > slot_payload_capacity_) {
        return PublishResult::PayloadTooLarge;
    }

    auto& write_cursor = header_->producer;
    // only the producer writes this cursor, so its own load can be relaxed.
    const auto write_pos = write_cursor.position.load(std::memory_order::relaxed);

    if (write_pos == std::numeric_limits<std::uint64_t>::max()) {
        return PublishResult::Closed;
    }
    // touch the consumer's cache line only when the cached cursor says the ring is full.
    if (write_pos - cached_read_position_ >= slot_count_) {
        // acquire the consumer's release before reusing its slot.
        cached_read_position_ = header_->consumer.position.load(std::memory_order::acquire);
        if (cached_read_position_ > write_pos ||
            write_pos - cached_read_position_ > slot_count_) {
            return PublishResult::Closed;
        }
        if (write_pos - cached_read_position_ == slot_count_) {
            return PublishResult::Full;
        }
    }

    const auto write_idx = write_pos & (slot_count_ - 1);
    auto* slot = slots_ + static_cast<std::size_t>(slot_stride_) * write_idx;
    auto* header = reinterpret_cast<RingSlotHeader*>(slot);

    detail::fill_slot_header(header, event);

    if (!event.payload.empty()) {
        std::ranges::copy(event.payload, slot + ring_slot_metadata_bytes);
    }

    // publish the completed metadata and payload to the consumer.
    write_cursor.position.store(write_pos + 1, std::memory_order::release);
    return PublishResult::Ok;
}

inline std::expected<EventView, AcquireError> SpscRing::try_acquire() noexcept {
    if (header_ == nullptr) {
        return std::unexpected(AcquireError::Closed);
    }
    if (acquired_) {
        return std::unexpected(AcquireError::AlreadyAcquired);
    }

    // only the consumer advances its read cursor.
    const auto read_pos = header_->consumer.position.load(std::memory_order_relaxed);
    // events below the cached write cursor were already acquired, so skip the producer's line.
    if (read_pos >= cached_write_position_) {
        // read closure before the write cursor so the final publication cannot be missed.
        const auto state = header_->preamble.state.load(std::memory_order_acquire);
        // acquire the producer's published slot contents before reading them.
        cached_write_position_ = header_->producer.position.load(std::memory_order_acquire);
        if (read_pos == cached_write_position_) {
            return std::unexpected(
                state == static_cast<std::uint32_t>(RingState::Closed) ? AcquireError::Closed
                                                                       : AcquireError::Empty
            );
        }
    }

    if (read_pos > cached_write_position_ || cached_write_position_ - read_pos > slot_count_) {
        return std::unexpected(AcquireError::InvalidSlot);
    }

    const auto index = read_pos & (slot_count_ - 1);
    const auto* slot = slots_ + index * slot_stride_;
    const auto* slot_header = reinterpret_cast<const RingSlotHeader*>(slot);
    if (detail::invalid_slot(*slot_header, slot_payload_capacity_)) {
        return std::unexpected(AcquireError::InvalidSlot);
    }

    acquired_position_ = read_pos;
    acquired_ = true;
    // keep the slot occupied until release() is called after all payload use.
    return EventView{
        .stream_id = slot_header->stream_id,
        .sequence = slot_header->event_sequence,
        .source_timestamp_ns = slot_header->source_timestamp_ns,
        .payload = {slot + ring_slot_metadata_bytes, slot_header->payload_length}
    };
}

inline bool SpscRing::release() noexcept {
    if (!acquired_) {
        return false;
    }
    // finish all slot reads before allowing the producer to reuse its storage.
    header_->consumer.position.store(acquired_position_ + 1, std::memory_order::release);
    acquired_ = false;
    acquired_position_ = 0;

    return true;
}

} // namespace lle::shm
