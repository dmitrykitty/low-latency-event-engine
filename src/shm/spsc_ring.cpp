#include "shm/spsc_ring.hpp"

#include <algorithm>
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

void fill_slot_header(RingSlotHeader* header, const EventView& event) noexcept {
    header->event_sequence = event.sequence;
    header->source_timestamp_ns = event.source_timestamp_ns;
    header->stream_id = event.stream_id;
    header->payload_length = static_cast<std::uint32_t>(event.payload.size());
}

bool invalid_memory(std::span<std::byte> memory) {
    // check minimum header size and alignment; callers check the exact segment size.
    return memory.data() == nullptr || memory.size() < sizeof(RingHeader) ||
           reinterpret_cast<std::uintptr_t>(memory.data()) % alignof(RingHeader) != 0;
}

bool invalid_layout(const RingPreamble& preamble) {
    return preamble.magic != ring_magic || preamble.layout_version != ring_layout_version ||
           preamble.header_bytes != ring_header_bytes || preamble.instance_id == 0 ||
           preamble.reserved != 0;
}

bool invalid_slot(const RingSlotHeader& slot, std::uint32_t payload_capacity) noexcept {
    return slot.payload_length > payload_capacity || slot.reserved[0] != 0 ||
           slot.reserved[1] != 0;
}

std::expected<void, RingError> validate_state(const std::uint32_t state) {
    if (state == static_cast<std::uint32_t>(RingState::Uninitialized) ||
        state == static_cast<std::uint32_t>(RingState::Initializing)) {
        return std::unexpected(RingError::NotReady);
    }
    if (state != static_cast<std::uint32_t>(RingState::Ready) &&
        state != static_cast<std::uint32_t>(RingState::Closed)) {
        return std::unexpected(RingError::InvalidLayout);
    }
    return {};
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
    // the shared-memory layout requires an exact size match.
    if (invalid_memory(memory) || memory.size() != size) {
        return std::unexpected(RingError::InvalidMemory);
    }

    // start the lifetimes of the header and its atomic members in fresh storage.
    auto* header = std::construct_at(reinterpret_cast<RingHeader*>(memory.data()));
    fill_preamble(&header->preamble, size, config, instance_id);
    // relaxed stores are safe because the creator has exclusive access during initialization.
    header->producer.position.store(0, std::memory_order_relaxed);
    header->consumer.position.store(0, std::memory_order_relaxed);

    // slots starts at
    auto* slots = memory.data() + ring_header_bytes;
    for (std::size_t index = 0; index < config.slot_count; ++index) {
        auto* slot = slots + index * header->preamble.slot_stride;
        std::construct_at(reinterpret_cast<RingSlotHeader*>(slot));
    }

    // publish all metadata and initialized slots to an acquire-loading attacher
    header->preamble.state.store(
        static_cast<std::uint32_t>(RingState::Ready),
        std::memory_order_release
    );
    return SpscRing{header, slots, true};
}

std::expected<SpscRing, RingError> SpscRing::attach(std::span<std::byte> memory) noexcept {
    if (invalid_memory(memory)) {
        return std::unexpected(RingError::InvalidMemory);
    }

    // the creator must have constructed the header before attachment is attempted
    auto* header = reinterpret_cast<RingHeader*>(memory.data());
    const RingPreamble& preamble = header->preamble;

    // pairs with initialization's release-store so metadata is visible here.
    const auto state = preamble.state.load(std::memory_order_acquire);
    if (auto state_result = validate_state(state); !state_result) {
        return std::unexpected(state_result.error());
    }

    if (invalid_layout(preamble)) {
        return std::unexpected(RingError::InvalidLayout);
    }
    // according to ABI all reserved bytes have value 0
    const auto is_zero = [](std::byte value) { return value == std::byte{0}; };
    if (!std::ranges::all_of(preamble.reserved_bytes, is_zero) ||
        !std::ranges::all_of(header->producer.reserved_bytes, is_zero) ||
        !std::ranges::all_of(header->consumer.reserved_bytes, is_zero)) {
        return std::unexpected(RingError::InvalidLayout);
    }

    const RingConfig config{preamble.slot_count, preamble.slot_payload_capacity};
    const auto size = required_bytes(config);
    if (!size) {
        return std::unexpected(RingError::InvalidLayout);
    }
    const auto stride = (*size - ring_header_bytes) / config.slot_count;
    if (preamble.slot_stride != stride || preamble.segment_bytes != *size) {
        return std::unexpected(RingError::InvalidLayout);
    }
    if (memory.size() != *size) {
        return std::unexpected(RingError::InvalidMemory);
    }

    // the producer may already be running - no cursors resets needed
    return SpscRing{header, memory.data() + ring_header_bytes, false};
}

PublishResult SpscRing::try_publish(const EventView& event) noexcept {
    if (header_ == nullptr) {
        return PublishResult::Closed;
    }
    const auto& preamble = header_->preamble;
    const auto state = preamble.state.load(std::memory_order::acquire);

    if (state != static_cast<std::uint32_t>(RingState::Ready)) {
        return PublishResult::Closed;
    }

    if (event.payload.size() > preamble.slot_payload_capacity) {
        return PublishResult::PayloadTooLarge;
    }

    auto& write_cursor = header_->producer;
    auto& read_cursor = header_->consumer;
    // only the producer writes this cursor, so its own load can be relaxed.
    const auto write_pos = write_cursor.position.load(std::memory_order::relaxed);
    // acquire the consumer's release before reusing its slot.
    const auto read_pos = read_cursor.position.load(std::memory_order::acquire);

    if (write_pos == std::numeric_limits<std::uint64_t>::max()) {
        return PublishResult::Closed;
    }
    if (read_pos > write_pos || write_pos - read_pos > preamble.slot_count) {
        return PublishResult::Closed;
    }
    if (write_pos - read_pos == preamble.slot_count) {
        return PublishResult::Full;
    }

    const auto write_idx = write_pos & (preamble.slot_count - 1);
    auto* slot = slots_ + preamble.slot_stride * write_idx;
    auto* header = reinterpret_cast<RingSlotHeader*>(slot);

    fill_slot_header(header, event);

    if (!event.payload.empty()) {
        std::ranges::copy(event.payload, slot + ring_slot_metadata_bytes);
    }

    // publish the completed metadata and payload to the consumer.
    write_cursor.position.store(write_pos + 1, std::memory_order::release);
    return PublishResult::Ok;
}

std::expected<EventView, AcquireError> SpscRing::try_acquire() noexcept {
    if (header_ == nullptr) {
        return std::unexpected(AcquireError::Closed);
    }
    if (acquired_) {
        return std::unexpected(AcquireError::AlreadyAcquired);
    }

    // read closure before the write cursor so the final publication cannot be missed.
    const auto state = header_->preamble.state.load(std::memory_order_acquire);
    // only the consumer advances its read cursor.
    const auto read_pos = header_->consumer.position.load(std::memory_order_relaxed);
    // acquire the producer's published slot contents before reading them.
    const auto write_pos = header_->producer.position.load(std::memory_order_acquire);
    if (read_pos == write_pos) {
        return std::unexpected(
            state == static_cast<std::uint32_t>(RingState::Closed) ? AcquireError::Closed
                                                                   : AcquireError::Empty
        );
    }

    const auto& preamble = header_->preamble;
    if (read_pos > write_pos || write_pos - read_pos > preamble.slot_count) {
        return std::unexpected(AcquireError::InvalidSlot);
    }

    const auto index = read_pos & (preamble.slot_count - 1);
    const auto* slot = slots_ + index * preamble.slot_stride;
    const auto* slot_header = reinterpret_cast<const RingSlotHeader*>(slot);
    if (invalid_slot(*slot_header, preamble.slot_payload_capacity)) {
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

bool SpscRing::release() noexcept {
    if (!acquired_) {
        return false;
    }
    header_->consumer.position.store(acquired_position_ + 1, std::memory_order::release);
    acquired_ = false;
    acquired_position_ = 0;

    return true;
}

bool SpscRing::close_publication() noexcept {
    if (!initializer_) {
        return false;
    }

    header_->preamble.state.store(
        static_cast<std::uint32_t>(RingState::Closed),
        std::memory_order::release
    );
    return true;
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
