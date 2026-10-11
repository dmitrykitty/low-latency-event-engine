#include "lle/receiver.hpp"
#include "lle/protocol/encoder.hpp"
#include "lle/sender.hpp"
#include "lle/transport/udp_sender.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <sys/mman.h>
#include <unistd.h>

namespace lle {
namespace {

class ReceiverTest : public testing::Test {
protected:
    void SetUp() override {
        name_ = "/lle-receiver-test-" + std::to_string(getpid());
        config_.egress_shm_name = name_;
        config_.egress_ring = {
            2, static_cast<std::uint32_t>(protocol::kMaxSingleEventPayloadBytes)
        };
        config_.listen = {"127.0.0.1", 0};
        auto udp = transport::UdpSender::open();
        ASSERT_TRUE(udp.has_value());
        udp_.emplace(std::move(*udp));
    }

    void TearDown() override {
        stop_worker();
        receiver_.reset();
        // Also clean up when an assertion fails while testing initialization.
        static_cast<void>(shm_unlink(name_.c_str()));
        static_cast<void>(shm_unlink((name_ + "-ingress").c_str()));
    }

    bool initialize_receiver() {
        auto receiver = Receiver::initialize(config_);
        if (!receiver) {
            return false;
        }
        receiver_.emplace(std::move(*receiver));
        auto segment = shm::SharedMemorySegment::attach_shm(name_);
        if (!segment) {
            return false;
        }
        segment_.emplace(std::move(*segment));
        auto consumer = shm::SpscRing::attach(segment_->bytes());
        if (!consumer) {
            return false;
        }
        consumer_.emplace(std::move(*consumer));
        auto destination = transport::UdpAddress::ipv4("127.0.0.1", receiver_->local_port());
        if (!destination) {
            return false;
        }
        destination_ = *destination;
        return true;
    }

    void start_worker() {
        worker_ = std::jthread([&] {
            receiver_->run();
            worker_finished_.store(true, std::memory_order_release);
        });
    }

    void stop_worker() {
        if (receiver_) {
            receiver_->request_stop();
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    EventView event(std::uint64_t sequence = 123) const {
        return EventView{7, sequence, 456, payload_};
    }

    bool send_event(const EventView& event, std::uint64_t packet_sequence = 1,
                    std::uint32_t session_id = 7) {
        const auto encoded = protocol::encode_data(event, session_id, packet_sequence, packet_);
        return encoded && udp_->send_to(std::span{packet_}.first(*encoded), destination_);
    }

    std::expected<EventView, shm::AcquireError> acquire_event() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
        auto result = consumer_->try_acquire();
        while (!result && result.error() == shm::AcquireError::Empty &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
            result = consumer_->try_acquire();
        }
        return result;
    }

    bool wait_for_publication(std::uint64_t count) const {
        const auto* header = reinterpret_cast<const shm::RingHeader*>(segment_->bytes().data());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
        // Acquire pairs with the ring producer's publication, not with EngineStats.
        while (header->producer.position.load(std::memory_order_acquire) < count) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::yield();
        }
        return true;
    }

    ReceiverConfig config_;
    std::string name_;
    std::optional<Receiver> receiver_;
    std::optional<shm::SharedMemorySegment> segment_;
    std::optional<shm::SpscRing> consumer_;
    std::optional<transport::UdpSender> udp_;
    transport::UdpAddress destination_{};
    std::array<std::byte, 4> payload_{
        std::byte{0x00}, std::byte{0x7f}, std::byte{0x80}, std::byte{0xff}
    };
    alignas(8) std::array<std::byte, protocol::kMaxPacketBytes> packet_{};
    std::atomic<bool> worker_finished_{false};
    std::jthread worker_;
};

TEST_F(ReceiverTest, ReceivesMetadataAndBinaryPayload) {
    ASSERT_TRUE(initialize_receiver());
    ASSERT_NE(receiver_->local_port(), 0U);
    start_worker();
    ASSERT_TRUE(send_event(event()));
    const auto received = acquire_event();
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->stream_id, event().stream_id);
    EXPECT_EQ(received->sequence, event().sequence);
    EXPECT_EQ(received->source_timestamp_ns, event().source_timestamp_ns);
    EXPECT_TRUE(std::ranges::equal(received->payload, payload_));
    EXPECT_TRUE(consumer_->release());
    stop_worker();
    const auto stats = receiver_->stats();
    EXPECT_EQ(stats.packets_received, 1U);
    EXPECT_EQ(stats.events_in, 1U);
    EXPECT_EQ(stats.events_out, 1U);
    EXPECT_EQ(stats.events_dropped, 0U);
}

TEST_F(ReceiverTest, PreservesOrderAndCopiesPayloadsAcrossWraps) {
    ASSERT_TRUE(initialize_receiver());
    start_worker();
    for (std::uint64_t sequence = 1; sequence <= 20; ++sequence) {
        const std::array payload{static_cast<std::byte>(sequence), std::byte{0xff}};
        const EventView expected{static_cast<StreamId>(sequence), sequence, sequence * 10, payload};
        ASSERT_TRUE(send_event(expected, sequence));
        const auto received = acquire_event();
        ASSERT_TRUE(received.has_value());
        EXPECT_EQ(received->stream_id, expected.stream_id);
        EXPECT_EQ(received->sequence, expected.sequence);
        EXPECT_EQ(received->source_timestamp_ns, expected.source_timestamp_ns);
        EXPECT_TRUE(std::ranges::equal(received->payload, payload));
        ASSERT_TRUE(consumer_->release());
    }
    stop_worker();
    EXPECT_EQ(receiver_->stats().events_out, 20U);
    EXPECT_EQ(receiver_->stats().events_dropped, 0U);
}

TEST_F(ReceiverTest, ReceivesEmptyAndMaximumPayloadsAndDrainsAfterStop) {
    ASSERT_TRUE(initialize_receiver());
    auto empty = event(1);
    empty.payload = {};
    std::array<std::byte, protocol::kMaxSingleEventPayloadBytes> payload{};
    payload.front() = std::byte{0x12};
    payload.back() = std::byte{0xfe};
    auto maximum = event(2);
    maximum.payload = payload;
    ASSERT_TRUE(send_event(empty, 1));
    ASSERT_TRUE(send_event(maximum, 2));
    start_worker();
    ASSERT_TRUE(wait_for_publication(2));
    stop_worker();

    for (const auto& expected : {empty, maximum}) {
        const auto received = consumer_->try_acquire();
        ASSERT_TRUE(received.has_value());
        EXPECT_EQ(received->sequence, expected.sequence);
        EXPECT_TRUE(std::ranges::equal(received->payload, expected.payload));
        ASSERT_TRUE(consumer_->release());
    }
    const auto closed = consumer_->try_acquire();
    ASSERT_FALSE(closed.has_value());
    EXPECT_EQ(closed.error(), shm::AcquireError::Closed);
    EXPECT_EQ(receiver_->stats().events_out, 2U);
}

TEST_F(ReceiverTest, CountsGapRangesSeparatelyFromMissingPackets) {
    ASSERT_TRUE(initialize_receiver());
    start_worker();
    // Event sequences and stream IDs are independent of transport sequencing.
    for (const auto sequence : {1ULL, 6ULL, 9ULL}) {
        auto expected = event(100 + sequence);
        expected.stream_id = static_cast<StreamId>(sequence);
        ASSERT_TRUE(send_event(expected, sequence));
        const auto received = acquire_event();
        ASSERT_TRUE(received.has_value());
        EXPECT_EQ(received->sequence, expected.sequence);
        EXPECT_EQ(received->stream_id, expected.stream_id);
        ASSERT_TRUE(consumer_->release());
    }
    stop_worker();
    const auto stats = receiver_->stats();
    EXPECT_EQ(stats.packets_received, 3U);
    EXPECT_EQ(stats.events_out, 3U);
    EXPECT_EQ(stats.gaps_detected, 2U);
    EXPECT_EQ(stats.packets_missed, 6U);
    EXPECT_EQ(stats.events_dropped, 0U);
}

TEST_F(ReceiverTest, DropsDuplicatesLatePacketsAndForeignSessionWithoutRedelivery) {
    config_.egress_ring.slot_count = 4;
    ASSERT_TRUE(initialize_receiver());
    ASSERT_TRUE(send_event(event(100), 1));
    ASSERT_TRUE(send_event(event(200), 1)); // Duplicate.
    ASSERT_TRUE(send_event(event(300), 3)); // Gap: packet 2.
    ASSERT_TRUE(send_event(event(400), 2)); // Late arrival.
    ASSERT_TRUE(send_event(event(500), 99, 8)); // Other session.
    ASSERT_TRUE(send_event(event(600), 4)); // Proves earlier packets were processed.
    start_worker();
    ASSERT_TRUE(wait_for_publication(3));
    stop_worker();

    for (const auto sequence : {100ULL, 300ULL, 600ULL}) {
        const auto received = consumer_->try_acquire();
        ASSERT_TRUE(received.has_value());
        EXPECT_EQ(received->sequence, sequence);
        ASSERT_TRUE(consumer_->release());
    }
    const auto closed = consumer_->try_acquire();
    ASSERT_FALSE(closed.has_value());
    EXPECT_EQ(closed.error(), shm::AcquireError::Closed);
    const auto stats = receiver_->stats();
    EXPECT_EQ(stats.packets_received, 6U);
    EXPECT_EQ(stats.events_in, 3U);
    EXPECT_EQ(stats.events_out, 3U);
    EXPECT_EQ(stats.events_dropped, 3U);
    EXPECT_EQ(stats.duplicate_or_old_packets, 2U);
    EXPECT_EQ(stats.wrong_session_packets, 1U);
    EXPECT_EQ(stats.gaps_detected, 1U);
    EXPECT_EQ(stats.packets_missed, 1U);
}

TEST_F(ReceiverTest, RemembersSequenceEvenWhenEventCannotBePublished) {
    config_.egress_ring.slot_payload_capacity = 2;
    ASSERT_TRUE(initialize_receiver());
    ASSERT_TRUE(send_event(event(1), 1)); // Payload does not fit egress.
    auto empty = event(2);
    empty.payload = {};
    ASSERT_TRUE(send_event(empty, 1)); // Same packet cannot be retried with another payload.
    ASSERT_TRUE(send_event(empty, 2));
    start_worker();
    const auto received = acquire_event();
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->sequence, 2U);
    EXPECT_TRUE(received->payload.empty());
    ASSERT_TRUE(consumer_->release());
    stop_worker();
    const auto closed = consumer_->try_acquire();
    ASSERT_FALSE(closed.has_value());
    EXPECT_EQ(closed.error(), shm::AcquireError::Closed);
    const auto stats = receiver_->stats();
    EXPECT_EQ(stats.packets_received, 3U);
    EXPECT_EQ(stats.events_in, 2U);
    EXPECT_EQ(stats.events_out, 1U);
    EXPECT_EQ(stats.events_dropped, 2U);
    EXPECT_EQ(stats.egress_errors, 1U);
    EXPECT_EQ(stats.duplicate_or_old_packets, 1U);
    EXPECT_EQ(stats.gaps_detected, 0U);
    EXPECT_EQ(stats.packets_missed, 0U);
}

TEST_F(ReceiverTest, RejectsMalformedAndOversizedPacketsAndContinues) {
    ASSERT_TRUE(initialize_receiver());
    const auto encoded = protocol::encode_data(event(), 7, 1, packet_);
    ASSERT_TRUE(encoded.has_value());
    packet_[0] = std::byte{0}; // Invalid magic.
    ASSERT_TRUE(udp_->send_to(std::span{packet_}.first(*encoded), destination_));
    ASSERT_TRUE(udp_->send_to(std::span{packet_}.first(1), destination_)); // Truncated.
    ASSERT_TRUE(udp_->send_to({}, destination_)); // Empty UDP datagram.
    const std::array<std::byte, protocol::kMaxPacketBytes + 1> oversized{};
    ASSERT_TRUE(udp_->send_to(oversized, destination_));
    ASSERT_TRUE(send_event(event(5), 5));
    start_worker();
    const auto received = acquire_event();
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->sequence, 5U);
    EXPECT_TRUE(consumer_->release());
    stop_worker();
    const auto stats = receiver_->stats();
    EXPECT_EQ(stats.packets_received, 5U);
    EXPECT_EQ(stats.malformed_packets, 4U);
    EXPECT_EQ(stats.events_in, 1U);
    EXPECT_EQ(stats.events_out, 1U);
    EXPECT_EQ(stats.receive_errors, 0U);
    EXPECT_EQ(stats.gaps_detected, 1U);
    EXPECT_EQ(stats.packets_missed, 4U);
}

TEST_F(ReceiverTest, DropsPayloadThatDoesNotFitEgressAndContinues) {
    config_.egress_ring.slot_payload_capacity = 2;
    ASSERT_TRUE(initialize_receiver());
    ASSERT_TRUE(send_event(event(1), 1));
    auto empty = event(2);
    empty.payload = {};
    ASSERT_TRUE(send_event(empty, 2));
    start_worker();
    const auto received = acquire_event();
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->sequence, 2U);
    EXPECT_TRUE(received->payload.empty());
    EXPECT_TRUE(consumer_->release());
    stop_worker();
    const auto stats = receiver_->stats();
    EXPECT_EQ(stats.events_in, 2U);
    EXPECT_EQ(stats.events_out, 1U);
    EXPECT_EQ(stats.events_dropped, 1U);
    EXPECT_EQ(stats.egress_errors, 1U);
    EXPECT_EQ(stats.egress_full, 0U);
}

TEST_F(ReceiverTest, DropsOnFullEgressWithoutOverwritingQueuedEvents) {
    ASSERT_TRUE(initialize_receiver());
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
        ASSERT_TRUE(send_event(event(sequence), sequence));
    }
    start_worker();
    ASSERT_TRUE(wait_for_publication(2));
    // Keep both slots occupied while the receiver consumes the final queued datagram.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    stop_worker();
    for (std::uint64_t sequence = 1; sequence <= 2; ++sequence) {
        const auto received = consumer_->try_acquire();
        ASSERT_TRUE(received.has_value());
        EXPECT_EQ(received->sequence, sequence);
        EXPECT_TRUE(std::ranges::equal(received->payload, payload_));
        ASSERT_TRUE(consumer_->release());
    }
    const auto closed = consumer_->try_acquire();
    ASSERT_FALSE(closed.has_value());
    EXPECT_EQ(closed.error(), shm::AcquireError::Closed);
    const auto stats = receiver_->stats();
    EXPECT_EQ(stats.events_in, 3U);
    EXPECT_EQ(stats.events_out, 2U);
    EXPECT_EQ(stats.events_dropped, 1U);
    EXPECT_EQ(stats.egress_full, 1U);
}

TEST_F(ReceiverTest, StopBeforeRunClosesEgressWithoutReceivingPackets) {
    ASSERT_TRUE(initialize_receiver());
    ASSERT_TRUE(send_event(event()));
    receiver_->request_stop();
    receiver_->run();
    EXPECT_EQ(receiver_->stats().packets_received, 0U);
    const auto closed = consumer_->try_acquire();
    ASSERT_FALSE(closed.has_value());
    EXPECT_EQ(closed.error(), shm::AcquireError::Closed);
}

TEST_F(ReceiverTest, StopsWhenEgressIsClosed) {
    ASSERT_TRUE(initialize_receiver());
    auto* header = reinterpret_cast<shm::RingHeader*>(segment_->bytes().data());
    // Simulate an unavailable ring before starting its producer.
    header->preamble.state.store(
        static_cast<std::uint32_t>(shm::RingState::Closed), std::memory_order_release
    );
    ASSERT_TRUE(send_event(event()));
    receiver_->run();
    const auto stats = receiver_->stats();
    EXPECT_EQ(stats.events_in, 1U);
    EXPECT_EQ(stats.events_out, 0U);
    EXPECT_EQ(stats.events_dropped, 1U);
    EXPECT_EQ(stats.egress_errors, 1U);
}

TEST_F(ReceiverTest, CanStopWhileUdpIsIdle) {
    ASSERT_TRUE(initialize_receiver());
    start_worker();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    receiver_->request_stop();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (!worker_finished_.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_TRUE(worker_finished_.load(std::memory_order_acquire));
    // Wake an incorrectly blocking implementation too, so a failure can clean up.
    if (!worker_finished_.load(std::memory_order_acquire)) {
        ASSERT_TRUE(send_event(event()));
    }
    stop_worker();
    EXPECT_EQ(receiver_->stats().packets_received, 0U);
    EXPECT_EQ(receiver_->stats().receive_errors, 0U);
}

TEST_F(ReceiverTest, DestructorClosesPublicationUnlinksShmAndReleasesPort) {
    ASSERT_TRUE(initialize_receiver());
    config_.listen.port = receiver_->local_port();
    receiver_.reset();
    const auto closed = consumer_->try_acquire();
    ASSERT_FALSE(closed.has_value());
    EXPECT_EQ(closed.error(), shm::AcquireError::Closed);
    EXPECT_FALSE(shm::SharedMemorySegment::attach_shm(name_).has_value());
    EXPECT_TRUE(Receiver::initialize(config_).has_value());
}

TEST_F(ReceiverTest, MovePreservesStatsAndTransfersUnlinkOwnership) {
    ASSERT_TRUE(initialize_receiver());
    start_worker();
    ASSERT_TRUE(send_event(event()));
    ASSERT_TRUE(wait_for_publication(1));
    stop_worker();
    {
        auto moved = std::move(*receiver_);
        receiver_.reset();
        EXPECT_EQ(moved.stats().events_out, 1U);
        EXPECT_TRUE(shm::SharedMemorySegment::attach_shm(name_).has_value());
    }
    EXPECT_FALSE(shm::SharedMemorySegment::attach_shm(name_).has_value());
    const auto queued = consumer_->try_acquire();
    ASSERT_TRUE(queued.has_value());
    EXPECT_TRUE(std::ranges::equal(queued->payload, payload_));
    EXPECT_TRUE(consumer_->release());
}

TEST_F(ReceiverTest, RejectsUnsupportedOrInvalidConfiguration) {
    const auto expect_error = [](const ReceiverConfig& config, InitError expected) {
        const auto receiver = Receiver::initialize(config);
        ASSERT_FALSE(receiver.has_value());
        EXPECT_EQ(receiver.error(), expected);
    };
    auto invalid = config_;
    invalid.egress_shm_name.clear();
    expect_error(invalid, InitError::InvalidConfig);
    invalid = config_;
    invalid.egress_ring.slot_count = 3;
    expect_error(invalid, InitError::InvalidConfig);
    invalid = config_;
    invalid.delivery_mode = DeliveryMode::ReliableOrdered;
    expect_error(invalid, InitError::InvalidConfig);
    invalid = config_;
    invalid.cpu = 0;
    expect_error(invalid, InitError::InvalidConfig);
    invalid = config_;
    invalid.busy_poll_us = 1;
    expect_error(invalid, InitError::InvalidConfig);
    invalid = config_;
    invalid.listen.address = "not-an-ip";
    expect_error(invalid, InitError::AddressResolution);
    invalid = config_;
    invalid.egress_shm_name = "missing-leading-slash";
    expect_error(invalid, InitError::SharedMemory);
    EXPECT_FALSE(shm::SharedMemorySegment::attach_shm(name_).has_value());
}

TEST_F(ReceiverTest, FailedInitializationDoesNotRemoveExistingShm) {
    const auto size = shm::SpscRing::required_bytes(config_.egress_ring);
    ASSERT_TRUE(size.has_value());
    auto existing = shm::SharedMemorySegment::create_shm(name_, *size);
    ASSERT_TRUE(existing.has_value());
    existing->bytes().front() = std::byte{0x42};
    const auto receiver = Receiver::initialize(config_);
    ASSERT_FALSE(receiver.has_value());
    EXPECT_EQ(receiver.error(), InitError::SharedMemory);
    const auto attached = shm::SharedMemorySegment::attach_shm(name_);
    ASSERT_TRUE(attached.has_value());
    EXPECT_EQ(attached->bytes().front(), std::byte{0x42});
}

TEST_F(ReceiverTest, SocketBindFailureLeavesNoSharedMemory) {
    {
        auto occupied = transport::UdpReceiver::bind(0);
        ASSERT_TRUE(occupied.has_value());
        config_.listen.port = occupied->local_port();
        const auto receiver = Receiver::initialize(config_);
        ASSERT_FALSE(receiver.has_value());
        EXPECT_EQ(receiver.error(), InitError::Socket);
        EXPECT_FALSE(shm::SharedMemorySegment::attach_shm(name_).has_value());
    }
    EXPECT_TRUE(Receiver::initialize(config_).has_value());
}

TEST_F(ReceiverTest, EmptyListenAddressAcceptsLoopbackPackets) {
    config_.listen.address.clear();
    ASSERT_TRUE(initialize_receiver());
    start_worker();
    ASSERT_TRUE(send_event(event()));
    const auto received = acquire_event();
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->sequence, event().sequence);
    EXPECT_TRUE(consumer_->release());
    stop_worker();
    EXPECT_EQ(receiver_->stats().events_out, 1U);
}

TEST_F(ReceiverTest, BindsOnlyTheConfiguredIpv4Address) {
    ASSERT_TRUE(initialize_receiver());
    auto other_destination = transport::UdpAddress::ipv4("127.0.0.2", receiver_->local_port());
    ASSERT_TRUE(other_destination.has_value());
    auto other_sender = transport::UdpSender::open();
    ASSERT_TRUE(other_sender.has_value());
    const auto encoded = protocol::encode_data(event(999), 7, 1, packet_);
    ASSERT_TRUE(encoded.has_value());
    ASSERT_TRUE(other_sender->send_to(std::span{packet_}.first(*encoded), *other_destination));
    ASSERT_TRUE(send_event(event(), 2));
    start_worker();
    const auto received = acquire_event();
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->sequence, event().sequence);
    EXPECT_TRUE(consumer_->release());
    stop_worker();
    EXPECT_EQ(receiver_->stats().packets_received, 1U);
}

TEST_F(ReceiverTest, ProducerSenderReceiverConsumerPreserveEvent) {
    ASSERT_TRUE(initialize_receiver());
    const shm::RingConfig ring_config{2, 4};
    const auto size = shm::SpscRing::required_bytes(ring_config);
    ASSERT_TRUE(size.has_value());
    auto ingress = shm::SharedMemorySegment::create_shm(name_ + "-ingress", *size);
    ASSERT_TRUE(ingress.has_value());
    auto producer = shm::SpscRing::initialize(ingress->bytes(), ring_config, 42);
    ASSERT_TRUE(producer.has_value());
    ASSERT_EQ(producer->try_publish(event()), PublishResult::Ok);
    ASSERT_TRUE(producer->close_publication());
    SenderConfig sender_config;
    sender_config.ingress_shm_name = name_ + "-ingress";
    sender_config.receivers.push_back({"127.0.0.1", receiver_->local_port()});
    auto sender = Sender::initialize(sender_config);
    ASSERT_TRUE(sender.has_value());
    start_worker();
    sender->run();
    const auto received = acquire_event();
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->stream_id, event().stream_id);
    EXPECT_EQ(received->sequence, event().sequence);
    EXPECT_EQ(received->source_timestamp_ns, event().source_timestamp_ns);
    EXPECT_TRUE(std::ranges::equal(received->payload, payload_));
    EXPECT_TRUE(consumer_->release());
    stop_worker();
    EXPECT_EQ(sender->stats().events_out, 1U);
    EXPECT_EQ(receiver_->stats().events_out, 1U);
    EXPECT_TRUE(ingress->unlink().has_value());
}

} // namespace
} // namespace lle
