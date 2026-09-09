#include "lle/config.hpp"
#include "lle/event.hpp"
#include "lle/version.hpp"

#include <gtest/gtest.h>

TEST(ProjectSmokeTest, VersionIsNotEmpty) {
    EXPECT_FALSE(lle::version().empty());
}

TEST(ProjectSmokeTest, DefaultDatagramLimitMatchesProtocol) {
    const lle::SenderConfig config{};
    EXPECT_EQ(config.max_datagram_bytes, 1416U);
}

TEST(ProjectSmokeTest, PublicIdentifierWidthsMatchProtocol) {
    EXPECT_EQ(sizeof(lle::Sequence), 8U);
    EXPECT_EQ(sizeof(lle::StreamId), 4U);
}
