#include "lle/gap_detector.hpp"

namespace lle {
CheckResult
GapDetector::check(std::uint32_t session_id, std::uint64_t packet_sequence) noexcept {
    CheckResult result;
    if (active_session_ == 0) {
        active_session_ = session_id;
    } else if (session_id != active_session_) {
        result.status = PacketStatus::WrongSession;
        return result;
    } else if (packet_sequence <= last_sequence_) {
        result.status = PacketStatus::DuplicateOrOld;
        return result;
    }

    result.gap = packet_sequence - last_sequence_ - 1;
    if (result.gap > 0) {
        result.first_missing = last_sequence_ + 1;
        result.last_missing = packet_sequence - 1;
    }
    last_sequence_ = packet_sequence;
    return result;
}

} // namespace lle
