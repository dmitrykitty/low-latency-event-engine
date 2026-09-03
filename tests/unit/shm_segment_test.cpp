#include "shm/segment.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include <sys/mman.h>
#include <unistd.h>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void expect_invalid_name(std::string name) {
    const auto result = lle::shm::SharedMemorySegment::create_shm(std::move(name), 4096);
    check(!result, "invalid shared-memory name was accepted");
    if (!result) {
        check(result.error().operation == lle::shm::SegmentOperation::Validate,
              "invalid name did not report validation failure");
        check(result.error().error_number == EINVAL,
              "invalid name did not report EINVAL");
    }
}

} // namespace

int main() {
    expect_invalid_name("");
    expect_invalid_name("/");
    expect_invalid_name("lle-segment");
    expect_invalid_name("/lle/nested");
    expect_invalid_name("/lle segment");
    expect_invalid_name("/.");
    expect_invalid_name("/..");
    expect_invalid_name(std::string{"/lle\0hidden", 11});

    std::string long_name(256, 'a');
    long_name.front() = '/';
    expect_invalid_name(std::move(long_name));

    const std::string name = "/lle-segment-test-" + std::to_string(::getpid());
    static_cast<void>(::shm_unlink(name.c_str()));

    const auto zero_size = lle::shm::SharedMemorySegment::create_shm(name, 0);
    check(!zero_size, "zero-sized segment was accepted");
    if (!zero_size) {
        check(zero_size.error().operation == lle::shm::SegmentOperation::Validate,
              "zero size did not report validation failure");
    }

    auto created = lle::shm::SharedMemorySegment::create_shm(name, 4096);
    check(created.has_value(), "valid segment creation failed");

    if (created) {
        check(created->is_open(), "created segment is not open");
        check(created->is_owner(), "created segment is not marked as owner");
        check(created->size() == 4096, "created segment has incorrect size");
        check(created->bytes().size() == 4096, "mapped byte span has incorrect size");

        created->bytes().front() = std::byte{0x2a};
        created->bytes().back() = std::byte{0x7f};

        const auto duplicate = lle::shm::SharedMemorySegment::create_shm(name, 4096);
        check(!duplicate, "duplicate creation unexpectedly succeeded");
        if (!duplicate) {
            check(duplicate.error().operation == lle::shm::SegmentOperation::Open,
                  "duplicate creation did not fail during open");
            check(duplicate.error().error_number == EEXIST,
                  "duplicate creation did not report EEXIST");
        }

        const auto unlink_result = created->unlink();
        check(unlink_result.has_value(), "owner could not unlink segment");

        const auto close_result = created->close();
        check(close_result.has_value(), "created segment could not close");
        check(!created->is_open(), "segment remained open after close");
        check(created->close().has_value(), "second close was not idempotent");
    }

    static_cast<void>(::shm_unlink(name.c_str()));
    return failures == 0 ? 0 : 1;
}
