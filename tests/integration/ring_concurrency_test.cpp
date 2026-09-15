#include "shm/segment.hpp"
#include "shm/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

namespace lle::shm {
namespace {

using Clock = std::chrono::steady_clock;

bool produce(SpscRing& ring, std::uint64_t count, Clock::time_point deadline) {
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::array<std::uint64_t, 2> payload{i, ~i};
        const EventView event{7, i, i * 3, std::as_bytes(std::span{payload})};
        for (;;) {
            const auto result = ring.try_publish(event);
            if (result == PublishResult::Ok) {
                break;
            }
            if (result != PublishResult::Full || Clock::now() >= deadline) {
                return false;
            }
            std::this_thread::yield();
        }
        if (i % 4096 == 0 && Clock::now() >= deadline) {
            return false;
        }
    }
    return ring.close_publication();
}

bool consume(SpscRing& ring, std::uint64_t count, Clock::time_point deadline) {
    std::uint64_t expected = 0;
    for (;;) {
        auto result = ring.try_acquire();
        if (!result) {
            if (result.error() == AcquireError::Closed) {
                return expected == count;
            }
            if (result.error() != AcquireError::Empty || Clock::now() >= deadline) {
                return false;
            }
            std::this_thread::yield();
            continue;
        }
        if (expected >= count || result->sequence != expected || result->stream_id != 7 ||
            result->source_timestamp_ns != expected * 3 || result->payload.size() != 16) {
            return false;
        }
        std::array<std::uint64_t, 2> payload{};
        std::memcpy(payload.data(), result->payload.data(), sizeof(payload));
        if (payload[0] != expected || payload[1] != ~expected || !ring.release()) {
            return false;
        }
        ++expected;
        if (expected % 4096 == 0 && Clock::now() >= deadline) {
            return false;
        }
    }
}

TEST(RingConcurrencyTest, ThreadsTransferOrderedEventsAndObserveFinalClosure) {
    alignas(64) std::array<std::byte, 192 + 64 * 64> memory{};
    auto producer = SpscRing::initialize(memory, RingConfig{64, 16}, 42);
    ASSERT_TRUE(producer.has_value());
    auto consumer = SpscRing::attach(memory);
    ASSERT_TRUE(consumer.has_value());
    const auto deadline = Clock::now() + std::chrono::seconds{45};
    bool consumed = false;
    std::jthread reader([&] { consumed = consume(*consumer, 200000, deadline); });
    const bool produced = produce(*producer, 200000, deadline);
    reader.join();
    EXPECT_TRUE(produced);
    EXPECT_TRUE(consumed);
}

TEST(RingConcurrencyTest, SeparateMappingTransfersTenMillionEvents) {
    const std::string name = "/lle-ring-integration-" + std::to_string(getpid());
    auto size = SpscRing::required_bytes(RingConfig{64, 16});
    ASSERT_TRUE(size.has_value());
    auto segment = SharedMemorySegment::create_shm(name, *size);
    ASSERT_TRUE(segment.has_value());
    struct Cleanup {
        SharedMemorySegment& segment;
        ~Cleanup() { static_cast<void>(segment.unlink()); }
    } cleanup{*segment};
    auto ring = SpscRing::initialize(segment->bytes(), RingConfig{64, 16}, 42);
    ASSERT_TRUE(ring.has_value());
    const auto deadline = Clock::now() + std::chrono::seconds{45};
    // fork after initialization establishes startup ordering; the child remaps by name.
    const auto child = fork();
    ASSERT_NE(child, -1);
    if (child == 0) {
        if (!segment->close_shm()) {
            _exit(2);
        }
        auto attached = SharedMemorySegment::attach_shm(name);
        if (!attached) {
            _exit(3);
        }
        auto consumer = SpscRing::attach(attached->bytes());
        if (!consumer) {
            _exit(4);
        }
        const bool ok = consume(*consumer, 10000000, deadline);
        static_cast<void>(attached->close_shm());
        _exit(ok ? 0 : 5);
    }
    const bool produced = produce(*ring, 10000000, deadline);
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited == -1 && errno == EINTR);
    EXPECT_TRUE(produced);
    ASSERT_EQ(waited, child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

} // namespace
} // namespace lle::shm
