#include "byte_io.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace lle::protocol::internals {
namespace {

TEST(ByteIoTest, WritesKnownLittleEndianBytesAtUnalignedOffset) {
    std::array<std::byte, 10> actual{};

    actual.fill(std::byte{0xa5});
    write_u16(actual.data() + 1, 0xcdef);
    EXPECT_EQ(
        actual,
        (std::array{
            std::byte{0xa5},
            std::byte{0xef},
            std::byte{0xcd},
            std::byte{0xa5},
            std::byte{0xa5},
            std::byte{0xa5},
            std::byte{0xa5},
            std::byte{0xa5},
            std::byte{0xa5},
            std::byte{0xa5}
        })
    );

    actual.fill(std::byte{0xa5});
    write_u32(actual.data() + 1, 0x89abcdef);
    EXPECT_EQ(
        actual,
        (std::array{
            std::byte{0xa5},
            std::byte{0xef},
            std::byte{0xcd},
            std::byte{0xab},
            std::byte{0x89},
            std::byte{0xa5},
            std::byte{0xa5},
            std::byte{0xa5},
            std::byte{0xa5},
            std::byte{0xa5}
        })
    );

    actual.fill(std::byte{0xa5});
    write_u64(actual.data() + 1, 0x0123456789abcdefULL);
    EXPECT_EQ(
        actual,
        (std::array{
            std::byte{0xa5},
            std::byte{0xef},
            std::byte{0xcd},
            std::byte{0xab},
            std::byte{0x89},
            std::byte{0x67},
            std::byte{0x45},
            std::byte{0x23},
            std::byte{0x01},
            std::byte{0xa5}
        })
    );
}

TEST(ByteIoTest, ReadsKnownLittleEndianBytesAtUnalignedOffset) {
    constexpr std::array bytes{
        std::byte{0xa5},
        std::byte{0xef},
        std::byte{0xcd},
        std::byte{0xab},
        std::byte{0x89},
        std::byte{0x67},
        std::byte{0x45},
        std::byte{0x23},
        std::byte{0x01},
        std::byte{0xa5}
    };

    EXPECT_EQ(read_u16(bytes.data() + 1), 0xcdefU);
    EXPECT_EQ(read_u32(bytes.data() + 1), 0x89abcdefU);
    EXPECT_EQ(read_u64(bytes.data() + 1), 0x0123456789abcdefULL);
}

TEST(ByteIoTest, ChecksExactFitAndRejectsShortOrInvalidRanges) {
    EXPECT_TRUE(has_bytes(10, 2, 8));
    EXPECT_TRUE(has_bytes(10, 10, 0));
    EXPECT_TRUE(has_bytes(0, 0, 0));
    EXPECT_FALSE(has_bytes(10, 3, 8));
    EXPECT_FALSE(has_bytes(10, 11, 0));
    EXPECT_FALSE(has_bytes(0, 0, 1));
    EXPECT_FALSE(has_bytes(10, std::numeric_limits<std::size_t>::max(), 1));
}

} // namespace
} // namespace lle::protocol::internals
