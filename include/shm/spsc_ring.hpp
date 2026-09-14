#pragma once

#include "lle/event.hpp"
#include "shm/ring_layout.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace lle::shm {

struct RingConfig {
    std::uint32_t slot_count;
    std::uint32_t slot_payload_capacity;
};

enum class RingError: uint8_t {
    InvalidConfig,
    SizeOverflow,
    InvalidMemory,
    InvalidLayout,
    NotReady
};

enum class AcquireError: uint8_t {
    Empty,
    Closed, // Closed and no events remain.
    AlreadyAcquired,
    InvalidSlot
};

// Uses memory owned by SharedMemorySegment. Keep that segment open while using the ring.
// One producer and one consumer, each with its own ring handle.
class SpscRing {
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
    std::uint64_t acquired_position_{0};
    bool acquired_{false};
    bool initializer_{false};
};

} // namespace lle::shm
