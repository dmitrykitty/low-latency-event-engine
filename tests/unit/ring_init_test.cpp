#include "shm/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>

namespace lle::shm {
namespace {

class RingInitTest : public testing::Test {
  protected:
    static constexpr RingConfig config{2, 33};
    static constexpr std::size_t segment_size = 192 + 2 * 128;
    alignas(64) std::array<std::byte, segment_size + 64> storage;

    void SetUp() override {
        storage.fill(std::byte{0x5a});
    }

    std::span<std::byte> memory() {
        return {storage.data(), segment_size};
    }

    void expect_unchanged() {
        EXPECT_TRUE(std::ranges::all_of(storage, [](std::byte value) {
            return value == std::byte{0x5a};
        }));
    }
};

TEST_F(RingInitTest, InitializesEveryHeaderFieldAndSlotMetadata) {
    auto result = SpscRing::initialize(memory(), config, 42);
    ASSERT_TRUE(result.has_value());
    auto* header = std::launder(reinterpret_cast<RingHeader*>(storage.data()));
    const auto& preamble = header->preamble;
    EXPECT_EQ(preamble.state.load(std::memory_order_acquire),
              static_cast<std::uint32_t>(RingState::Ready));
    EXPECT_EQ(preamble.magic, ring_magic);
    EXPECT_EQ(preamble.layout_version, ring_layout_version);
    EXPECT_EQ(preamble.header_bytes, ring_header_bytes);
    EXPECT_EQ(preamble.segment_bytes, segment_size);
    EXPECT_EQ(preamble.slot_count, 2U);
    EXPECT_EQ(preamble.slot_stride, 128U);
    EXPECT_EQ(preamble.slot_payload_capacity, 33U);
    EXPECT_EQ(preamble.instance_id, 42U);
    EXPECT_EQ(preamble.reserved, 0U);
    EXPECT_EQ(preamble.reserved_bytes, (std::array<std::byte, 20>{}));
    EXPECT_EQ(header->producer.position.load(), 0U);
    EXPECT_EQ(header->consumer.position.load(), 0U);
    EXPECT_EQ(header->producer.reserved_bytes, (std::array<std::byte, 56>{}));
    EXPECT_EQ(header->consumer.reserved_bytes, (std::array<std::byte, 56>{}));
    for (std::size_t i = 0; i < config.slot_count; ++i) {
        auto* bytes = storage.data() + ring_header_bytes + i * preamble.slot_stride;
        auto* slot = std::launder(reinterpret_cast<RingSlotHeader*>(bytes));
        EXPECT_EQ(slot->event_sequence, 0U);
        EXPECT_EQ(slot->source_timestamp_ns, 0U);
        EXPECT_EQ(slot->stream_id, 0U);
        EXPECT_EQ(slot->payload_length, 0U);
        EXPECT_EQ(slot->reserved, (std::array<std::uint32_t, 2>{}));
        EXPECT_EQ(bytes[ring_slot_metadata_bytes], std::byte{0x5a});
    }
    EXPECT_EQ(storage[segment_size], std::byte{0x5a});
}

TEST_F(RingInitTest, RejectsInvalidCapacityWithoutWriting) {
    for (auto count : {0U, 1U, 3U}) {
        auto result = SpscRing::initialize(memory(), RingConfig{count, 33}, 42);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), RingError::InvalidConfig);
    }
    expect_unchanged();
}

TEST_F(RingInitTest, RejectsZeroInstanceIdWithoutWriting) {
    auto result = SpscRing::initialize(memory(), config, 0);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), RingError::InvalidConfig);
    expect_unchanged();
}

TEST_F(RingInitTest, RejectsWrongSizeWithoutWriting) {
    for (auto size : {std::size_t{0}, segment_size - 1, segment_size + 1}) {
        auto result = SpscRing::initialize({storage.data(), size}, config, 42);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), RingError::InvalidMemory);
    }
    expect_unchanged();
}

TEST_F(RingInitTest, RejectsMisalignmentWithoutWriting) {
    auto result = SpscRing::initialize({storage.data() + 1, segment_size}, config, 42);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), RingError::InvalidMemory);
    expect_unchanged();
}

TEST_F(RingInitTest, RejectsUnrepresentableStrideWithoutWriting) {
    auto result = SpscRing::initialize(
        memory(), RingConfig{2, std::numeric_limits<std::uint32_t>::max()}, 42
    );
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), RingError::SizeOverflow);
    expect_unchanged();
}

TEST(RingSizeTest, AllowsMetadataOnlySlots) {
    auto result = SpscRing::required_bytes(RingConfig{2, 0});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 192U + 2U * 64U);
}

class RingAttachTest : public RingInitTest {
  protected:
    RingHeader* header{nullptr};

    void SetUp() override {
        RingInitTest::SetUp();
        ASSERT_TRUE(SpscRing::initialize(memory(), config, 42).has_value());
        header = std::launder(reinterpret_cast<RingHeader*>(storage.data()));
    }
};

TEST_F(RingAttachTest, PreservesExistingContentsForReadyAndClosedRings) {
    header->producer.position.store(9);
    header->consumer.position.store(7);
    for (auto state : {RingState::Ready, RingState::Closed}) {
        header->preamble.state.store(static_cast<std::uint32_t>(state));
        std::array<std::byte, segment_size> before;
        std::memcpy(before.data(), storage.data(), segment_size);
        ASSERT_TRUE(SpscRing::attach(memory()).has_value());
        EXPECT_EQ(std::memcmp(before.data(), storage.data(), segment_size), 0);
    }
}

TEST_F(RingAttachTest, RejectsShortMisalignedAndWrongSizeMappings) {
    for (auto bytes : {std::span<std::byte>{}, memory().first(191),
                       std::span<std::byte>{storage.data() + 1, segment_size},
                       memory().first(segment_size - 1),
                       std::span<std::byte>{storage.data(), segment_size + 1}}) {
        auto result = SpscRing::attach(bytes);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), RingError::InvalidMemory);
    }
}

TEST_F(RingAttachTest, RejectsNotReadyAndUnknownStates) {
    for (auto state : {RingState::Uninitialized, RingState::Initializing}) {
        header->preamble.state.store(static_cast<std::uint32_t>(state));
        auto result = SpscRing::attach(memory());
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), RingError::NotReady);
    }
    header->preamble.state.store(99);
    auto result = SpscRing::attach(memory());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), RingError::InvalidLayout);
}

TEST_F(RingAttachTest, RejectsInvalidMetadata) {
    auto check_field = [this](auto& field, auto invalid) {
        const auto original = field;
        field = invalid;
        auto result = SpscRing::attach(memory());
        EXPECT_FALSE(result.has_value());
        if (!result) {
            EXPECT_EQ(result.error(), RingError::InvalidLayout);
        }
        field = original;
    };
    auto& p = header->preamble;
    check_field(p.magic[0], 'X');
    check_field(p.layout_version, std::uint16_t{2});
    check_field(p.header_bytes, std::uint16_t{64});
    check_field(p.instance_id, std::uint64_t{0});
    check_field(p.segment_bytes, std::uint64_t{0});
    check_field(p.slot_count, std::uint32_t{0});
    check_field(p.slot_count, std::uint32_t{3});
    check_field(p.slot_stride, std::uint32_t{64});
    check_field(p.slot_payload_capacity, std::numeric_limits<std::uint32_t>::max());
    check_field(p.reserved, std::uint32_t{1});
    check_field(p.reserved_bytes[0], std::byte{1});
    check_field(header->producer.reserved_bytes[0], std::byte{1});
    check_field(header->consumer.reserved_bytes[0], std::byte{1});
}

} // namespace
} // namespace lle::shm
