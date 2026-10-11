#pragma once

#include <cstdint>

namespace lle {

enum class PacketStatus { Accepted, DuplicateOrOld, WrongSession };

struct CheckResult {
    PacketStatus status{PacketStatus::Accepted};
    std::uint64_t gap{0};
    // Inclusive range; both endpoints are zero when there is no gap.
    std::uint64_t first_missing{0};
    std::uint64_t last_missing{0};
};

class GapDetector {
public:
    // The decoder must validate nonzero session_id and packet_sequence first.
    // The first packet selects the session for this detector's lifetime.
    CheckResult check(std::uint32_t session_id, std::uint64_t packet_sequence) noexcept;

private:
    std::uint32_t active_session_{0};
    std::uint64_t last_sequence_{0};
};
} // namespace lle
