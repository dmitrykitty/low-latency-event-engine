#include "lle/gap_detector.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace lle {
namespace {

TEST(GapDetectorTest, AcceptsConsecutivePacketsWithoutGaps) {
    GapDetector detector;
    for (std::uint64_t sequence = 1; sequence <= 4; ++sequence) {
        const auto result = detector.check(7, sequence);
        EXPECT_EQ(result.status, PacketStatus::Accepted);
        EXPECT_EQ(result.gap, 0U);
        EXPECT_EQ(result.first_missing, 0U);
        EXPECT_EQ(result.last_missing, 0U);
    }
}

TEST(GapDetectorTest, ReportsMissingPrefixOnFirstPacket) {
    GapDetector detector;
    const auto result = detector.check(7, 4);
    EXPECT_EQ(result.status, PacketStatus::Accepted);
    EXPECT_EQ(result.gap, 3U);
    EXPECT_EQ(result.first_missing, 1U);
    EXPECT_EQ(result.last_missing, 3U);
    EXPECT_EQ(detector.check(7, 5).gap, 0U);
}

TEST(GapDetectorTest, ReportsExactRangesForSeparateGaps) {
    GapDetector detector;
    static_cast<void>(detector.check(7, 1));
    const auto first = detector.check(7, 6);
    EXPECT_EQ(first.status, PacketStatus::Accepted);
    EXPECT_EQ(first.gap, 4U);
    EXPECT_EQ(first.first_missing, 2U);
    EXPECT_EQ(first.last_missing, 5U);
    const auto second = detector.check(7, 9);
    EXPECT_EQ(second.status, PacketStatus::Accepted);
    EXPECT_EQ(second.gap, 2U);
    EXPECT_EQ(second.first_missing, 7U);
    EXPECT_EQ(second.last_missing, 8U);
}

TEST(GapDetectorTest, RejectsDuplicateWithoutAdvancingSequence) {
    GapDetector detector;
    static_cast<void>(detector.check(7, 1));
    const auto duplicate = detector.check(7, 1);
    EXPECT_EQ(duplicate.status, PacketStatus::DuplicateOrOld);
    EXPECT_EQ(duplicate.gap, 0U);
    EXPECT_EQ(duplicate.first_missing, 0U);
    EXPECT_EQ(duplicate.last_missing, 0U);
    const auto next = detector.check(7, 2);
    EXPECT_EQ(next.status, PacketStatus::Accepted);
    EXPECT_EQ(next.gap, 0U);
}

TEST(GapDetectorTest, RejectsLatePacketWithoutMovingSequenceBackwards) {
    GapDetector detector;
    static_cast<void>(detector.check(7, 1));
    static_cast<void>(detector.check(7, 3));
    const auto late = detector.check(7, 2);
    EXPECT_EQ(late.status, PacketStatus::DuplicateOrOld);
    EXPECT_EQ(late.gap, 0U);
    const auto next = detector.check(7, 4);
    EXPECT_EQ(next.status, PacketStatus::Accepted);
    EXPECT_EQ(next.gap, 0U);
}

TEST(GapDetectorTest, RejectsOtherSessionWithoutChangingActiveSession) {
    GapDetector detector;
    static_cast<void>(detector.check(7, 1));
    for (const auto sequence : {1ULL, 100ULL}) {
        const auto foreign = detector.check(8, sequence);
        EXPECT_EQ(foreign.status, PacketStatus::WrongSession);
        EXPECT_EQ(foreign.gap, 0U);
    }
    const auto next = detector.check(7, 2);
    EXPECT_EQ(next.status, PacketStatus::Accepted);
    EXPECT_EQ(next.gap, 0U);
}

TEST(GapDetectorTest, HandlesMaximumSequenceWithoutOverflow) {
    GapDetector detector;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    static_cast<void>(detector.check(7, 1));
    const auto result = detector.check(7, maximum);
    EXPECT_EQ(result.status, PacketStatus::Accepted);
    EXPECT_EQ(result.gap, maximum - 2);
    EXPECT_EQ(result.first_missing, 2U);
    EXPECT_EQ(result.last_missing, maximum - 1);
    EXPECT_EQ(detector.check(7, maximum).status, PacketStatus::DuplicateOrOld);
    EXPECT_EQ(detector.check(7, 1).status, PacketStatus::DuplicateOrOld);
}

} // namespace
} // namespace lle
