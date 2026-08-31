#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace lle {

using StreamId = std::uint32_t;
using Sequence = std::uint64_t;

enum class DeliveryMode : std::uint8_t {
    BestEffort,
    ReliableOrdered,
};

enum class PublishResult : std::uint8_t {
    Ok,
    Full,
    PayloadTooLarge,
    Closed,
};

struct EventView {
    StreamId stream_id{};
    Sequence sequence{};
    std::uint64_t source_timestamp_ns{};
    std::span<const std::byte> payload{};
};

} // namespace lle

