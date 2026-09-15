#include "shm/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace lle::shm {
namespace {

class RingTransferTest : public testing::Test {
  protected:
    alignas(64) std::array<std::byte, 320> storage{};
    std::optional<SpscRing> producer;
    std::optional<SpscRing> consumer;
    RingHeader* header{};
    std::array<std::byte, 4> payload{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};

    void SetUp() override {
        auto initialized = SpscRing::initialize(storage, RingConfig{2, 4}, 42);
        ASSERT_TRUE(initialized.has_value());
        producer.emplace(std::move(*initialized));
        auto attached = SpscRing::attach(storage);
        ASSERT_TRUE(attached.has_value());
        consumer.emplace(std::move(*attached));
        header = std::launder(reinterpret_cast<RingHeader*>(storage.data()));
    }

    EventView event() {
        return EventView{7, 123, 456, payload};
    }
};

TEST_F(RingTransferTest, EmptyRingHasNoEvent) {
    auto result = consumer->try_acquire();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AcquireError::Empty);
}

TEST_F(RingTransferTest, TransfersMetadataAndViewsPayloadInPlace) {
    ASSERT_EQ(producer->try_publish(event()), PublishResult::Ok);
    payload[0] = std::byte{9};
    auto result = consumer->try_acquire();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->stream_id, 7U);
    EXPECT_EQ(result->sequence, 123U);
    EXPECT_EQ(result->source_timestamp_ns, 456U);
    ASSERT_EQ(result->payload.size(), 4U);
    EXPECT_EQ(result->payload[0], std::byte{1});
    EXPECT_EQ(result->payload[3], std::byte{4});
    EXPECT_EQ(result->payload.data(), storage.data() + 192 + 32);
    EXPECT_EQ(header->consumer.position.load(), 0U);
    auto second = consumer->try_acquire();
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error(), AcquireError::AlreadyAcquired);
}

TEST_F(RingTransferTest, FullRingDoesNotOverwriteHeldView) {
    ASSERT_EQ(producer->try_publish(event()), PublishResult::Ok);
    auto held = consumer->try_acquire();
    ASSERT_TRUE(held.has_value());
    ASSERT_EQ(producer->try_publish(event()), PublishResult::Ok);
    payload.fill(std::byte{9});
    EXPECT_EQ(producer->try_publish(event()), PublishResult::Full);
    EXPECT_EQ(header->producer.position.load(), 2U);
    EXPECT_EQ(held->payload[0], std::byte{1});
}

TEST_F(RingTransferTest, RejectsOversizedPayloadWithoutPublishing) {
    std::array<std::byte, 5> large{};
    auto value = event();
    value.payload = large;
    EXPECT_EQ(producer->try_publish(value), PublishResult::PayloadTooLarge);
    EXPECT_EQ(header->producer.position.load(), 0U);
    auto result = consumer->try_acquire();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AcquireError::Empty);
}

TEST_F(RingTransferTest, AllowsEmptyPayload) {
    auto value = event();
    value.payload = {};
    ASSERT_EQ(producer->try_publish(value), PublishResult::Ok);
    auto result = consumer->try_acquire();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->payload.empty());
}

TEST_F(RingTransferTest, MapsLogicalPositionToWrappedSlot) {
    // seed an empty ring past its first physical wrap.
    header->producer.position.store(3);
    header->consumer.position.store(3);
    ASSERT_EQ(producer->try_publish(event()), PublishResult::Ok);
    auto result = consumer->try_acquire();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload.data(), storage.data() + 192 + 64 + 32);
    EXPECT_EQ(header->producer.position.load(), 4U);
    EXPECT_EQ(header->consumer.position.load(), 3U);
}

TEST_F(RingTransferTest, ClosedEmptyRingRejectsPublicationAndAcquisition) {
    header->preamble.state.store(static_cast<std::uint32_t>(RingState::Closed));
    EXPECT_EQ(producer->try_publish(event()), PublishResult::Closed);
    auto result = consumer->try_acquire();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AcquireError::Closed);
}

TEST_F(RingTransferTest, ClosedRingStillAllowsQueuedEvent) {
    ASSERT_EQ(producer->try_publish(event()), PublishResult::Ok);
    header->preamble.state.store(static_cast<std::uint32_t>(RingState::Closed));
    auto result = consumer->try_acquire();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sequence, 123U);
}

TEST_F(RingTransferTest, RejectsInvalidSlotWithoutHoldingIt) {
    ASSERT_EQ(producer->try_publish(event()), PublishResult::Ok);
    auto* slot = reinterpret_cast<RingSlotHeader*>(storage.data() + 192);
    slot->payload_length = 5;
    auto result = consumer->try_acquire();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AcquireError::InvalidSlot);
    slot->payload_length = 4;
    for (auto& reserved : slot->reserved) {
        reserved = 1;
        result = consumer->try_acquire();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), AcquireError::InvalidSlot);
        reserved = 0;
    }
    EXPECT_EQ(header->consumer.position.load(), 0U);
    EXPECT_TRUE(consumer->try_acquire().has_value());
}

TEST_F(RingTransferTest, StopsBeforeLogicalCursorOverflow) {
    const auto max = std::numeric_limits<std::uint64_t>::max();
    header->producer.position.store(max);
    header->consumer.position.store(max);
    EXPECT_EQ(producer->try_publish(event()), PublishResult::Closed);
    EXPECT_EQ(header->producer.position.load(), max);
}

TEST_F(RingTransferTest, MovedFromHandlesAreClosed) {
    auto moved_producer = std::move(*producer);
    auto moved_consumer = std::move(*consumer);
    EXPECT_EQ(producer->try_publish(event()), PublishResult::Closed);
    auto result = consumer->try_acquire();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AcquireError::Closed);
    ASSERT_EQ(moved_producer.try_publish(event()), PublishResult::Ok);
    EXPECT_TRUE(moved_consumer.try_acquire().has_value());
}

TEST_F(RingTransferTest, ReleaseRequiresAcquisitionAndAllowsSlotReuse) {
    EXPECT_FALSE(consumer->release());
    ASSERT_EQ(producer->try_publish(event()), PublishResult::Ok);
    ASSERT_EQ(producer->try_publish(event()), PublishResult::Ok);
    ASSERT_TRUE(consumer->try_acquire().has_value());
    ASSERT_TRUE(consumer->release());
    EXPECT_FALSE(consumer->release());
    EXPECT_EQ(header->consumer.position.load(), 1U);
    EXPECT_EQ(producer->try_publish(event()), PublishResult::Ok);
}

TEST_F(RingTransferTest, EveryPayloadLengthFitsAndCanBeReleased) {
    for (std::size_t size = 0; size <= payload.size(); ++size) {
        auto value = event();
        value.payload = std::span{payload}.first(size);
        ASSERT_EQ(producer->try_publish(value), PublishResult::Ok);
        auto received = consumer->try_acquire();
        ASSERT_TRUE(received.has_value());
        ASSERT_EQ(received->payload.size(), size);
        for (std::size_t i = 0; i < size; ++i) {
            EXPECT_EQ(received->payload[i], payload[i]);
        }
        ASSERT_TRUE(consumer->release());
    }
}

TEST_F(RingTransferTest, CloseEmptyRingReportsClosed) {
    ASSERT_TRUE(producer->close_publication());
    auto result = consumer->try_acquire();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AcquireError::Closed);
    EXPECT_FALSE(consumer->release());
}

TEST_F(RingTransferTest, RepeatedWrapsPreserveFifoOrder) {
    for (std::uint64_t batch = 0; batch < 1000; ++batch) {
        for (std::uint64_t i = 0; i < 2; ++i) {
            auto value = event();
            value.sequence = batch * 2 + i;
            ASSERT_EQ(producer->try_publish(value), PublishResult::Ok);
        }
        EXPECT_EQ(producer->try_publish(event()), PublishResult::Full);
        for (std::uint64_t i = 0; i < 2; ++i) {
            auto value = consumer->try_acquire();
            ASSERT_TRUE(value.has_value());
            EXPECT_EQ(value->sequence, batch * 2 + i);
            ASSERT_TRUE(consumer->release());
        }
    }
}

TEST_F(RingTransferTest, OnlyInitializerCanCloseAndQueuedEventsDrain) {
    EXPECT_FALSE(consumer->close_publication());
    ASSERT_EQ(producer->try_publish(event()), PublishResult::Ok);
    ASSERT_EQ(producer->try_publish(event()), PublishResult::Ok);
    ASSERT_TRUE(producer->close_publication());
    EXPECT_TRUE(producer->close_publication());
    EXPECT_EQ(producer->try_publish(event()), PublishResult::Closed);
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(consumer->try_acquire().has_value());
        ASSERT_TRUE(consumer->release());
    }
    auto result = consumer->try_acquire();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AcquireError::Closed);
}

TEST_F(RingTransferTest, MoveTransfersHeldSlotAndCloseAuthority) {
    ASSERT_EQ(producer->try_publish(event()), PublishResult::Ok);
    ASSERT_TRUE(consumer->try_acquire().has_value());
    auto moved_consumer = std::move(*consumer);
    EXPECT_FALSE(consumer->release());
    EXPECT_TRUE(moved_consumer.release());
    EXPECT_EQ(header->consumer.position.load(), 1U);
    auto moved_producer = std::move(*producer);
    EXPECT_FALSE(producer->close_publication());
    EXPECT_TRUE(moved_producer.close_publication());
}

TEST_F(RingTransferTest, FinalCursorValueCanBeConsumedWithoutWrapping) {
    const auto max = std::numeric_limits<std::uint64_t>::max();
    header->producer.position.store(max - 1);
    header->consumer.position.store(max - 1);
    ASSERT_EQ(producer->try_publish(event()), PublishResult::Ok);
    ASSERT_TRUE(consumer->try_acquire().has_value());
    ASSERT_TRUE(consumer->release());
    EXPECT_EQ(header->consumer.position.load(), max);
    EXPECT_EQ(producer->try_publish(event()), PublishResult::Closed);
    ASSERT_TRUE(producer->close_publication());
    auto result = consumer->try_acquire();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), AcquireError::Closed);
}

} // namespace
} // namespace lle::shm
