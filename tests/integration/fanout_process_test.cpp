#include "lle/receiver.hpp"
#include "lle/sender.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace lle {
namespace {

constexpr std::array payload{
    std::byte{0x00}, std::byte{0x7f}, std::byte{0x80}, std::byte{0xff}
};

bool receive_and_check(Receiver& receiver, const std::string& egress_name) {
    // Each child opens its own consumer attachment after fork.
    auto segment = shm::SharedMemorySegment::attach_shm(egress_name);
    if (!segment) {
        return false;
    }
    auto consumer = shm::SpscRing::attach(segment->bytes());
    if (!consumer) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    std::jthread worker([&] { receiver.run(); });
    std::uint64_t count = 0;
    while (count < 2 && std::chrono::steady_clock::now() < deadline) {
        const auto event = consumer->try_acquire();
        if (!event) {
            if (event.error() != shm::AcquireError::Empty) {
                break;
            }
            std::this_thread::yield();
            continue;
        }
        const auto sequence = count + 1;
        if (event->stream_id != 7 || event->sequence != sequence * 100 ||
            event->source_timestamp_ns != sequence * 10 ||
            !std::ranges::equal(event->payload, payload) || !consumer->release()) {
            break;
        }
        ++count;
    }
    receiver.request_stop();
    worker.join();
    // EngineStats are read only after the receiver worker has finished.
    const auto stats = receiver.stats();
    const auto drained = consumer->try_acquire();
    return count == 2 && stats.packets_received == 2 && stats.events_out == 2 &&
           stats.events_dropped == 0 && stats.gaps_detected == 0 &&
           stats.duplicate_or_old_packets == 0 && stats.wrong_session_packets == 0 &&
           !drained && drained.error() == shm::AcquireError::Closed;
}

class FanoutProcessTest : public testing::TestWithParam<unsigned> {
protected:
    void SetUp() override {
        name_ = "/lle-fanout-test-" + std::to_string(getpid());
        const shm::RingConfig config{2, 4};
        const auto size = shm::SpscRing::required_bytes(config);
        ASSERT_TRUE(size.has_value());
        auto segment = shm::SharedMemorySegment::create_shm(name_, *size);
        ASSERT_TRUE(segment.has_value());
        ingress_.emplace(std::move(*segment));
        auto producer = shm::SpscRing::initialize(ingress_->bytes(), config, 42);
        ASSERT_TRUE(producer.has_value());
        producer_.emplace(std::move(*producer));
    }

    void TearDown() override {
        // An early ASSERT must not leave any test child running or unreaped.
        for (const auto child : children_) {
            if (child > 0) {
                static_cast<void>(kill(child, SIGKILL));
                while (waitpid(child, nullptr, 0) == -1 && errno == EINTR) {}
            }
        }
        // Parent keeps resource ownership until every receiver process is stopped.
        receivers_.clear();
        static_cast<void>(shm_unlink(name_.c_str()));
    }

    std::string name_;
    std::optional<shm::SharedMemorySegment> ingress_;
    std::optional<shm::SpscRing> producer_;
    std::vector<Receiver> receivers_;
    std::vector<pid_t> children_;
};

TEST_P(FanoutProcessTest, SeparateProcessesReceiveEveryEventThroughEgressShm) {
    SenderConfig sender_config;
    sender_config.ingress_shm_name = name_;
    receivers_.reserve(GetParam());
    for (unsigned i = 0; i < GetParam(); ++i) {
        ReceiverConfig config;
        config.egress_shm_name = name_ + "-egress-" + std::to_string(i);
        config.egress_ring = {2, 4};
        config.listen = {"127.0.0.1", 0};
        auto receiver = Receiver::initialize(config);
        ASSERT_TRUE(receiver.has_value());
        sender_config.receivers.push_back({"127.0.0.1", receiver->local_port()});
        receivers_.push_back(std::move(*receiver));
    }
    auto sender = Sender::initialize(sender_config);
    ASSERT_TRUE(sender.has_value());
    for (std::uint64_t sequence = 1; sequence <= 2; ++sequence) {
        const EventView event{7, sequence * 100, sequence * 10, payload};
        ASSERT_EQ(producer_->try_publish(event), PublishResult::Ok);
    }
    ASSERT_TRUE(producer_->close_publication());

    // Bind and initialize before fork, and fork before starting any worker threads.
    // Only child i runs receiver i; the parent retains ownership for cleanup.
    for (unsigned i = 0; i < GetParam(); ++i) {
        const auto egress_name = name_ + "-egress-" + std::to_string(i);
        const auto child = fork();
        ASSERT_NE(child, -1);
        if (child == 0) {
            _exit(receive_and_check(receivers_[i], egress_name) ? 0 : 1);
        }
        children_.push_back(child);
    }
    sender->run();
    for (auto& child : children_) {
        int status = 0;
        pid_t waited;
        do {
            waited = waitpid(child, &status, 0);
        } while (waited == -1 && errno == EINTR);
        ASSERT_EQ(waited, child);
        child = -1; // Already reaped; TearDown must not send a signal to this PID.
        ASSERT_TRUE(WIFEXITED(status));
        EXPECT_EQ(WEXITSTATUS(status), 0);
    }
    const auto stats = sender->stats();
    EXPECT_EQ(stats.events_in, 2U);
    EXPECT_EQ(stats.events_out, 2U);
    EXPECT_EQ(stats.packets_sent, 2U * GetParam());
    EXPECT_EQ(stats.events_dropped, 0U);
}

INSTANTIATE_TEST_SUITE_P(ReceiverCounts, FanoutProcessTest, testing::Values(1U, 2U, 4U));

} // namespace
} // namespace lle
