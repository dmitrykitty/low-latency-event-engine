#include "shm/segment.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <string>
#include <utility>

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using lle::shm::SegmentOperation;
using lle::shm::SharedMemorySegment;

std::string make_segment_name() {
    static std::atomic_uint64_t next_id{0};
    return "/lle-segment-test-" + std::to_string(getpid()) + "-" +
           std::to_string(next_id.fetch_add(1, std::memory_order_relaxed));
}

class SharedMemorySegmentTest : public testing::Test {
  protected:
    void TearDown() override {
        static_cast<void>(shm_unlink(name_.c_str()));
    }

    std::string name_{make_segment_name()};
};

class InvalidSharedMemoryNameTest : public testing::TestWithParam<std::string> {};

TEST_P(InvalidSharedMemoryNameTest, CreateRejectsInvalidName) {
    const auto result = SharedMemorySegment::create_shm(GetParam(), 4096);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().operation, SegmentOperation::Validate);
    EXPECT_EQ(result.error().error_number, EINVAL);
}

TEST_P(InvalidSharedMemoryNameTest, AttachRejectsInvalidName) {
    const auto result = SharedMemorySegment::attach_shm(GetParam());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().operation, SegmentOperation::Validate);
    EXPECT_EQ(result.error().error_number, EINVAL);
}

INSTANTIATE_TEST_SUITE_P(
    InvalidNames,
    InvalidSharedMemoryNameTest,
    testing::Values(
        "",
        "/",
        "lle-segment",
        "/lle/nested",
        "/lle segment",
        "/.",
        "/..",
        std::string{"/lle\0hidden", 11},
        std::string(256, 'a')
    )
);

TEST_F(SharedMemorySegmentTest, CreateRejectsZeroSize) {
    const auto result = SharedMemorySegment::create_shm(name_, 0);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().operation, SegmentOperation::Validate);
    EXPECT_EQ(result.error().error_number, EINVAL);
}

TEST_F(SharedMemorySegmentTest, CreateMapsRequestedSizeAndMarksOwner) {
    auto result = SharedMemorySegment::create_shm(name_, 4096);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_open());
    EXPECT_TRUE(result->is_owner());
    EXPECT_EQ(result->size(), 4096U);
    EXPECT_EQ(result->bytes().size(), 4096U);
}

TEST_F(SharedMemorySegmentTest, CreateRejectsExistingName) {
    auto first = SharedMemorySegment::create_shm(name_, 4096);
    ASSERT_TRUE(first.has_value());

    const auto duplicate = SharedMemorySegment::create_shm(name_, 4096);

    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(duplicate.error().operation, SegmentOperation::Open);
    EXPECT_EQ(duplicate.error().error_number, EEXIST);
}

TEST_F(SharedMemorySegmentTest, AttachReportsMissingSegment) {
    const auto result = SharedMemorySegment::attach_shm(name_);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().operation, SegmentOperation::Open);
    EXPECT_EQ(result.error().error_number, ENOENT);
}

TEST_F(SharedMemorySegmentTest, AttachUsesExistingSizeAndIsNotOwner) {
    auto created = SharedMemorySegment::create_shm(name_, 4096);
    ASSERT_TRUE(created.has_value());

    auto attached = SharedMemorySegment::attach_shm(name_);

    ASSERT_TRUE(attached.has_value());
    EXPECT_TRUE(attached->is_open());
    EXPECT_FALSE(attached->is_owner());
    EXPECT_EQ(attached->size(), created->size());
}

TEST_F(SharedMemorySegmentTest, CreatorAndAttacherShareWrites) {
    auto created = SharedMemorySegment::create_shm(name_, 4096);
    ASSERT_TRUE(created.has_value());
    auto attached = SharedMemorySegment::attach_shm(name_);
    ASSERT_TRUE(attached.has_value());

    created->bytes()[0] = std::byte{0x2a};
    EXPECT_EQ(attached->bytes()[0], std::byte{0x2a});

    attached->bytes()[1] = std::byte{0x3b};
    EXPECT_EQ(created->bytes()[1], std::byte{0x3b});
}

TEST_F(SharedMemorySegmentTest, AttacherCannotUnlinkSegment) {
    auto created = SharedMemorySegment::create_shm(name_, 4096);
    ASSERT_TRUE(created.has_value());
    auto attached = SharedMemorySegment::attach_shm(name_);
    ASSERT_TRUE(attached.has_value());

    const auto result = attached->unlink();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().operation, SegmentOperation::Unlink);
    EXPECT_EQ(result.error().error_number, EPERM);
    EXPECT_TRUE(SharedMemorySegment::attach_shm(name_).has_value());
}

TEST_F(SharedMemorySegmentTest, OwnerUnlinkPreventsNewAttachments) {
    auto created = SharedMemorySegment::create_shm(name_, 4096);
    ASSERT_TRUE(created.has_value());
    auto attached = SharedMemorySegment::attach_shm(name_);
    ASSERT_TRUE(attached.has_value());
    created->bytes()[0] = std::byte{0x7f};

    ASSERT_TRUE(created->unlink().has_value());

    const auto new_attachment = SharedMemorySegment::attach_shm(name_);
    ASSERT_FALSE(new_attachment.has_value());
    EXPECT_EQ(new_attachment.error().error_number, ENOENT);
    EXPECT_EQ(attached->bytes()[0], std::byte{0x7f});
}

TEST_F(SharedMemorySegmentTest, CloseShmIsIdempotent) {
    auto created = SharedMemorySegment::create_shm(name_, 4096);
    ASSERT_TRUE(created.has_value());

    EXPECT_TRUE(created->close_shm().has_value());
    EXPECT_FALSE(created->is_open());
    EXPECT_TRUE(created->close_shm().has_value());
}

TEST_F(SharedMemorySegmentTest, AttachRejectsExistingZeroSizedObject) {
    constexpr mode_t permissions = S_IRUSR | S_IWUSR;
    const int descriptor =
        shm_open(name_.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, permissions);
    ASSERT_NE(descriptor, -1);
    ASSERT_EQ(close(descriptor), 0);

    const auto attached = SharedMemorySegment::attach_shm(name_);

    ASSERT_FALSE(attached.has_value());
    EXPECT_EQ(attached.error().operation, SegmentOperation::Validate);
    EXPECT_EQ(attached.error().error_number, EINVAL);
}

TEST_F(SharedMemorySegmentTest, MoveConstructionTransfersResources) {
    auto result = SharedMemorySegment::create_shm(name_, 4096);
    ASSERT_TRUE(result.has_value());
    SharedMemorySegment original = std::move(result).value();

    SharedMemorySegment moved{std::move(original)};

    EXPECT_FALSE(original.is_open());
    EXPECT_TRUE(moved.is_open());
    EXPECT_TRUE(moved.is_owner());
    EXPECT_EQ(moved.size(), 4096U);
}

} // namespace
