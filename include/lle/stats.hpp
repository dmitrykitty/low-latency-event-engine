#pragma once

#include <cstdint>

namespace lle {

struct EngineStats {
    std::uint64_t events_in{};
    std::uint64_t events_out{};
    std::uint64_t packets_sent{};
    std::uint64_t packets_received{};
    std::uint64_t gaps_detected{};
    std::uint64_t nack_sent{};
    std::uint64_t retransmitted{};
    std::uint64_t recovered{};
    std::uint64_t unrecovered{};
    std::uint64_t ingress_full{};
    std::uint64_t malformed_packets{};
};

} // namespace lle

