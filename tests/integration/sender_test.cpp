#include "lle/sender.hpp"
#include "lle/protocol/decoder.hpp"
#include "lle/transport/udp_receiver.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

namespace lle {
namespace {

class SenderTest : public testing::Test {
protected:
    void SetUp() override {
        name_ = "/lle-sender-test-" + std::to_string(getpid());
        const shm::RingConfig ring_config{
            2, static_cast<std::uint32_t>(protocol::kMaxSingleEventPayloadBytes + 1)
        };
        const auto size = shm::SpscRing::required_bytes(ring_config);
        ASSERT_TRUE(size.has_value());
        auto segment = shm::SharedMemorySegment::create_shm(name_, *size);
        ASSERT_TRUE(segment.has_value());
        segment_.emplace(std::move(*segment));
        auto producer = shm::SpscRing::initialize(segment_->bytes(), ring_config, 42);
        ASSERT_TRUE(producer.has_value());
        producer_.emplace(std::move(*producer));
        auto receiver = transport::UdpReceiver::bind(0);
        ASSERT_TRUE(receiver.has_value());
        receiver_.emplace(std::move(*receiver));
        config_.ingress_shm_name = name_;
        config_.receivers.push_back({"127.0.0.1", receiver_->local_port()});
    }

    void TearDown() override {
        stop_worker();
        static_cast<void>(shm_unlink(name_.c_str()));
    }

    void stop_worker() {
        if (sender_) {
            sender_->request_stop();
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    EventView event(std::uint64_t sequence = 123) const {
        return EventView{7, sequence, 456, payload_};
    }

    SenderConfig config_;
    std::string name_;
    std::optional<shm::SharedMemorySegment> segment_;
    std::optional<shm::SpscRing> producer_;
    std::optional<transport::UdpReceiver> receiver_;
    std::array<std::byte, 4> payload_{
        std::byte{0x00}, std::byte{0x7f}, std::byte{0x80}, std::byte{0xff}
    };
    std::optional<Sender> sender_;
    std::jthread worker_;
};

class SenderFanoutTest : public SenderTest, public testing::WithParamInterface<unsigned> {};

TEST_P(SenderFanoutTest, SendsIdenticalDatagramsAndSequencesToEveryDestination) {
    std::vector<transport::UdpReceiver> receivers;
    config_.receivers.clear();
    for (unsigned i = 0; i < GetParam(); ++i) {
        auto receiver = transport::UdpReceiver::bind(0);
        ASSERT_TRUE(receiver.has_value());
        config_.receivers.push_back({"127.0.0.1", receiver->local_port()});
        receivers.push_back(std::move(*receiver));
    }
    auto sender = Sender::initialize(config_);
    ASSERT_TRUE(sender.has_value());
    // Moving the initialized sender must preserve the full destination list.
    sender_.emplace(std::move(*sender));
    auto empty = event(101);
    empty.payload = {};
    std::array<std::byte, protocol::kMaxSingleEventPayloadBytes> payload{};
    payload.front() = std::byte{0x12};
    payload.back() = std::byte{0xfe};
    const EventView maximum{8, 999, 1234, payload};
    ASSERT_EQ(producer_->try_publish(empty), PublishResult::Ok);
    ASSERT_EQ(producer_->try_publish(maximum), PublishResult::Ok);
    ASSERT_TRUE(producer_->close_publication());
    sender_->run();

    std::uint32_t session_id = 0;
    std::uint64_t packet_sequence = 1;
    for (const auto& expected : {empty, maximum}) {
        alignas(8) std::array<std::byte, protocol::kMaxPacketBytes> reference{};
        std::size_t reference_size = 0;
        for (auto& receiver : receivers) {
            alignas(8) std::array<std::byte, protocol::kMaxPacketBytes> packet{};
            const auto received = receiver.receive(packet, 1000);
            ASSERT_TRUE(received.has_value());
            const auto datagram = std::span{packet}.first(received->size);
            if (reference_size == 0) {
                reference = packet;
                reference_size = received->size;
            } else {
                EXPECT_TRUE(std::ranges::equal(datagram, std::span{reference}.first(reference_size)));
            }
            const auto decoded = protocol::decode_data(datagram);
            ASSERT_TRUE(decoded.has_value());
            if (session_id == 0) {
                session_id = decoded->session_id;
            }
            EXPECT_NE(session_id, 0U);
            EXPECT_EQ(decoded->session_id, session_id);
            EXPECT_EQ(decoded->packet_sequence, packet_sequence);
            EXPECT_EQ(decoded->event.stream_id, expected.stream_id);
            EXPECT_EQ(decoded->event.sequence, expected.sequence);
            EXPECT_EQ(decoded->event.source_timestamp_ns, expected.source_timestamp_ns);
            EXPECT_TRUE(std::ranges::equal(decoded->event.payload, expected.payload));
        }
        ++packet_sequence;
    }
    for (auto& receiver : receivers) {
        std::array<std::byte, protocol::kMaxPacketBytes> packet{};
        const auto extra = receiver.receive(packet, 10);
        ASSERT_FALSE(extra.has_value());
        EXPECT_EQ(extra.error().code, transport::UdpErrorCode::Timeout);
    }
    const auto stats = sender_->stats();
    EXPECT_EQ(stats.events_in, 2U);
    EXPECT_EQ(stats.events_out, 2U);
    EXPECT_EQ(stats.packets_sent, 2U * GetParam());
    EXPECT_EQ(stats.events_dropped, 0U);
    EXPECT_EQ(stats.send_errors, 0U);
}

INSTANTIATE_TEST_SUITE_P(ReceiverCounts, SenderFanoutTest, testing::Values(1U, 2U, 4U));

TEST_F(SenderTest, SendsQueuedEventAndDrainsClosedIngress) {
    auto sender = Sender::initialize(config_);
    ASSERT_TRUE(sender.has_value());
    ASSERT_EQ(producer_->try_publish(event()), PublishResult::Ok);
    ASSERT_TRUE(producer_->close_publication());
    sender->run();

    alignas(8) std::array<std::byte, protocol::kMaxPacketBytes> packet{};
    const auto received = receiver_->receive(packet, 1000);
    ASSERT_TRUE(received.has_value());
    const auto decoded = protocol::decode_data(std::span{packet}.first(received->size));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_NE(decoded->session_id, 0U);
    EXPECT_EQ(decoded->packet_sequence, 1U);
    EXPECT_EQ(decoded->event.stream_id, event().stream_id);
    EXPECT_EQ(decoded->event.sequence, event().sequence);
    EXPECT_EQ(decoded->event.source_timestamp_ns, event().source_timestamp_ns);
    EXPECT_TRUE(std::ranges::equal(decoded->event.payload, payload_));

    // A separate consumer view proves that run() released the final slot.
    auto consumer = shm::SpscRing::attach(segment_->bytes());
    ASSERT_TRUE(consumer.has_value());
    const auto remaining = consumer->try_acquire();
    ASSERT_FALSE(remaining.has_value());
    EXPECT_EQ(remaining.error(), shm::AcquireError::Closed);
    const auto stats = sender->stats();
    EXPECT_EQ(stats.events_in, 1U);
    EXPECT_EQ(stats.events_out, 1U);
    EXPECT_EQ(stats.packets_sent, 1U);
    EXPECT_EQ(stats.events_dropped, 0U);
}

TEST_F(SenderTest, SendsInOrderAndReleasesSlotsAcrossWraps) {
    auto sender = Sender::initialize(config_);
    ASSERT_TRUE(sender.has_value());
    sender_.emplace(std::move(*sender));
    // Fill both slots before starting the consumer.
    ASSERT_EQ(producer_->try_publish(event(1)), PublishResult::Ok);
    ASSERT_EQ(producer_->try_publish(event(2)), PublishResult::Ok);
    ASSERT_EQ(producer_->try_publish(event(3)), PublishResult::Full);
    worker_ = std::jthread([&] { sender_->run(); });

    std::uint32_t session_id = 0;
    for (std::uint64_t sequence = 1; sequence <= 20; ++sequence) {
        alignas(8) std::array<std::byte, protocol::kMaxPacketBytes> packet{};
        const auto received = receiver_->receive(packet, 1000);
        ASSERT_TRUE(received.has_value());
        const auto decoded = protocol::decode_data(std::span{packet}.first(received->size));
        ASSERT_TRUE(decoded.has_value());
        if (sequence == 1) {
            session_id = decoded->session_id;
        }
        EXPECT_EQ(decoded->session_id, session_id);
        EXPECT_EQ(decoded->packet_sequence, sequence);
        EXPECT_EQ(decoded->event.sequence, sequence);
        EXPECT_TRUE(std::ranges::equal(decoded->event.payload, payload_));

        if (sequence <= 18) {
            // UDP arrival may precede release by a few instructions.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
            PublishResult result;
            do {
                result = producer_->try_publish(event(sequence + 2));
                if (result == PublishResult::Full) {
                    std::this_thread::yield();
                }
            } while (result == PublishResult::Full &&
                     std::chrono::steady_clock::now() < deadline);
            ASSERT_EQ(result, PublishResult::Ok);
        }
    }
    ASSERT_TRUE(producer_->close_publication());
    worker_.join();
    EXPECT_EQ(sender_->stats().events_out, 20U);
    EXPECT_EQ(sender_->stats().events_dropped, 0U);
}

TEST_F(SenderTest, SendsEmptyAndMaximumPayloads) {
    auto sender = Sender::initialize(config_);
    ASSERT_TRUE(sender.has_value());
    auto empty = event(1);
    empty.payload = {};
    std::array<std::byte, protocol::kMaxSingleEventPayloadBytes> large{};
    large.front() = std::byte{0x12};
    large.back() = std::byte{0xfe};
    auto maximum = event(2);
    maximum.payload = large;
    ASSERT_EQ(producer_->try_publish(empty), PublishResult::Ok);
    ASSERT_EQ(producer_->try_publish(maximum), PublishResult::Ok);
    ASSERT_TRUE(producer_->close_publication());
    sender->run();

    for (const auto& expected : {empty, maximum}) {
        alignas(8) std::array<std::byte, protocol::kMaxPacketBytes> packet{};
        const auto received = receiver_->receive(packet, 1000);
        ASSERT_TRUE(received.has_value());
        const auto decoded = protocol::decode_data(std::span{packet}.first(received->size));
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(decoded->event.sequence, expected.sequence);
        EXPECT_TRUE(std::ranges::equal(decoded->event.payload, expected.payload));
    }
    EXPECT_EQ(sender->stats().events_out, 2U);
}

TEST_F(SenderTest, DropsInvalidEventAndContinuesWithNextSlot) {
    auto sender = Sender::initialize(config_);
    ASSERT_TRUE(sender.has_value());
    ASSERT_EQ(producer_->try_publish(event(0)), PublishResult::Ok);
    ASSERT_EQ(producer_->try_publish(event(1)), PublishResult::Ok);
    ASSERT_TRUE(producer_->close_publication());
    sender->run();

    alignas(8) std::array<std::byte, protocol::kMaxPacketBytes> packet{};
    const auto received = receiver_->receive(packet, 1000);
    ASSERT_TRUE(received.has_value());
    const auto decoded = protocol::decode_data(std::span{packet}.first(received->size));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->event.sequence, 1U);
    EXPECT_EQ(decoded->packet_sequence, 1U);
    EXPECT_EQ(sender->stats().events_in, 2U);
    EXPECT_EQ(sender->stats().events_out, 1U);
    EXPECT_EQ(sender->stats().encode_errors, 1U);
    EXPECT_EQ(sender->stats().events_dropped, 1U);
    EXPECT_FALSE(receiver_->receive(packet, 10).has_value());
}

TEST_F(SenderTest, DropsPayloadThatExceedsProtocolLimit) {
    auto sender = Sender::initialize(config_);
    ASSERT_TRUE(sender.has_value());
    std::array<std::byte, protocol::kMaxSingleEventPayloadBytes + 1> large{};
    auto oversized = event();
    oversized.payload = large;
    ASSERT_EQ(producer_->try_publish(oversized), PublishResult::Ok);
    ASSERT_TRUE(producer_->close_publication());
    sender->run();
    EXPECT_EQ(sender->stats().encode_errors, 1U);
    EXPECT_EQ(sender->stats().events_dropped, 1U);
    EXPECT_EQ(sender->stats().packets_sent, 0U);
    auto* header = reinterpret_cast<shm::RingHeader*>(segment_->bytes().data());
    EXPECT_EQ(header->consumer.position.load(), 1U);
}

TEST_F(SenderTest, RespectsConfiguredDatagramCapIncludingPadding) {
    config_.max_datagram_bytes = static_cast<std::uint32_t>(protocol::kMinDataPacketBytes);
    auto sender = Sender::initialize(config_);
    ASSERT_TRUE(sender.has_value());
    ASSERT_EQ(producer_->try_publish(event(1)), PublishResult::Ok); // 48 bytes: too big.
    auto small = event(2);
    small.payload = std::span{payload_}.first(2); // Exactly 44 bytes.
    ASSERT_EQ(producer_->try_publish(small), PublishResult::Ok);
    ASSERT_TRUE(producer_->close_publication());
    sender->run();

    alignas(8) std::array<std::byte, protocol::kMaxPacketBytes> packet{};
    const auto received = receiver_->receive(packet, 1000);
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->size, config_.max_datagram_bytes);
    EXPECT_EQ(sender->stats().encode_errors, 1U);
    EXPECT_EQ(sender->stats().events_out, 1U);
}

TEST_F(SenderTest, CountsSendFailuresAndReleasesSlots) {
    // Linux refuses broadcast sends without SO_BROADCAST. No packet is transmitted.
    config_.receivers.front().address = "255.255.255.255";
    auto sender = Sender::initialize(config_);
    ASSERT_TRUE(sender.has_value());
    ASSERT_EQ(producer_->try_publish(event(1)), PublishResult::Ok);
    ASSERT_EQ(producer_->try_publish(event(2)), PublishResult::Ok);
    ASSERT_TRUE(producer_->close_publication());
    sender->run();
    EXPECT_EQ(sender->stats().events_in, 2U);
    EXPECT_EQ(sender->stats().send_errors, 2U);
    EXPECT_EQ(sender->stats().events_dropped, 2U);
    EXPECT_EQ(sender->stats().events_out, 0U);
    auto* header = reinterpret_cast<shm::RingHeader*>(segment_->bytes().data());
    EXPECT_EQ(header->consumer.position.load(), 2U);
}

TEST_F(SenderTest, ContinuesFanoutAfterSendErrorAndCountsPartialSuccess) {
    auto second = transport::UdpReceiver::bind(0);
    ASSERT_TRUE(second.has_value());
    // One valid receiver before the failing address and another after it.
    config_.receivers.push_back({"255.255.255.255", receiver_->local_port()});
    config_.receivers.push_back({"127.0.0.1", second->local_port()});
    auto sender = Sender::initialize(config_);
    ASSERT_TRUE(sender.has_value());
    ASSERT_EQ(producer_->try_publish(event(100)), PublishResult::Ok);
    ASSERT_EQ(producer_->try_publish(event(200)), PublishResult::Ok);
    ASSERT_TRUE(producer_->close_publication());
    sender->run();

    std::uint32_t session_id = 0;
    for (std::uint64_t sequence = 1; sequence <= 2; ++sequence) {
        for (auto* receiver : {&*receiver_, &*second}) {
            alignas(8) std::array<std::byte, protocol::kMaxPacketBytes> packet{};
            const auto received = receiver->receive(packet, 1000);
            ASSERT_TRUE(received.has_value());
            const auto decoded = protocol::decode_data(std::span{packet}.first(received->size));
            ASSERT_TRUE(decoded.has_value());
            if (session_id == 0) {
                session_id = decoded->session_id;
            }
            EXPECT_EQ(decoded->session_id, session_id);
            EXPECT_EQ(decoded->packet_sequence, sequence);
            EXPECT_EQ(decoded->event.sequence, sequence * 100);
        }
    }
    const auto stats = sender->stats();
    EXPECT_EQ(stats.events_in, 2U);
    EXPECT_EQ(stats.events_out, 2U);
    EXPECT_EQ(stats.packets_sent, 4U);
    EXPECT_EQ(stats.send_errors, 2U);
    EXPECT_EQ(stats.events_dropped, 0U);
    auto* header = reinterpret_cast<shm::RingHeader*>(segment_->bytes().data());
    EXPECT_EQ(header->consumer.position.load(), 2U);
}

TEST_F(SenderTest, CountsEventDropOnceWhenEveryDestinationFails) {
    config_.receivers = {{"255.255.255.255", 9000}, {"255.255.255.255", 9001}};
    auto sender = Sender::initialize(config_);
    ASSERT_TRUE(sender.has_value());
    ASSERT_EQ(producer_->try_publish(event(1)), PublishResult::Ok);
    ASSERT_EQ(producer_->try_publish(event(2)), PublishResult::Ok);
    ASSERT_TRUE(producer_->close_publication());
    sender->run();
    const auto stats = sender->stats();
    EXPECT_EQ(stats.events_in, 2U);
    EXPECT_EQ(stats.events_out, 0U);
    EXPECT_EQ(stats.packets_sent, 0U);
    EXPECT_EQ(stats.send_errors, 4U);
    EXPECT_EQ(stats.events_dropped, 2U);
}

TEST_F(SenderTest, CountsEncodingFailureOnceWithMultipleDestinations) {
    auto second = transport::UdpReceiver::bind(0);
    ASSERT_TRUE(second.has_value());
    config_.receivers.push_back({"127.0.0.1", second->local_port()});
    auto sender = Sender::initialize(config_);
    ASSERT_TRUE(sender.has_value());
    ASSERT_EQ(producer_->try_publish(event(0)), PublishResult::Ok);
    ASSERT_TRUE(producer_->close_publication());
    sender->run();
    const auto stats = sender->stats();
    EXPECT_EQ(stats.events_in, 1U);
    EXPECT_EQ(stats.encode_errors, 1U);
    EXPECT_EQ(stats.events_dropped, 1U);
    EXPECT_EQ(stats.packets_sent, 0U);
    EXPECT_EQ(stats.events_out, 0U);
    EXPECT_EQ(stats.send_errors, 0U);
    for (auto* receiver : {&*receiver_, &*second}) {
        std::array<std::byte, protocol::kMaxPacketBytes> packet{};
        const auto received = receiver->receive(packet, 10);
        ASSERT_FALSE(received.has_value());
        EXPECT_EQ(received.error().code, transport::UdpErrorCode::Timeout);
    }
}

TEST_F(SenderTest, StopsOnInvalidIngressSlotWithoutSending) {
    auto sender = Sender::initialize(config_);
    ASSERT_TRUE(sender.has_value());
    ASSERT_EQ(producer_->try_publish(event()), PublishResult::Ok);
    auto* slot = reinterpret_cast<shm::RingSlotHeader*>(
        segment_->bytes().data() + shm::ring_header_bytes
    );
    slot->reserved[0] = 1;
    sender->run();
    EXPECT_EQ(sender->stats().ingress_errors, 1U);
    EXPECT_EQ(sender->stats().events_in, 0U);
    EXPECT_EQ(sender->stats().packets_sent, 0U);
}

TEST_F(SenderTest, StopBeforeRunLeavesQueuedEventUntouched) {
    auto sender = Sender::initialize(config_);
    ASSERT_TRUE(sender.has_value());
    ASSERT_EQ(producer_->try_publish(event()), PublishResult::Ok);
    sender->request_stop();
    sender->run();
    EXPECT_EQ(sender->stats().events_in, 0U);
    auto consumer = shm::SpscRing::attach(segment_->bytes());
    ASSERT_TRUE(consumer.has_value());
    ASSERT_TRUE(consumer->try_acquire().has_value());
    EXPECT_TRUE(consumer->release());
}

TEST_F(SenderTest, CanStopWhileIngressIsEmpty) {
    for (const bool spin : {true, false}) {
        config_.busy_spin_ingress = spin;
        auto sender = Sender::initialize(config_);
        ASSERT_TRUE(sender.has_value());
        sender_.emplace(std::move(*sender));
        worker_ = std::jthread([&] { sender_->run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        stop_worker();
        EXPECT_EQ(sender_->stats().events_in, 0U);
        EXPECT_EQ(sender_->stats().ingress_errors, 0U);
    }
}

TEST_F(SenderTest, RejectsUnsupportedOrInvalidConfiguration) {
    const auto expect_error = [](const SenderConfig& config, InitError error) {
        const auto result = Sender::initialize(config);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), error);
    };
    auto invalid = config_;
    invalid.ingress_shm_name.clear();
    expect_error(invalid, InitError::InvalidConfig);
    invalid = config_;
    invalid.receivers.clear();
    expect_error(invalid, InitError::InvalidConfig);
    invalid = config_;
    invalid.max_datagram_bytes = 43;
    expect_error(invalid, InitError::InvalidConfig);
    invalid.max_datagram_bytes = 1417;
    expect_error(invalid, InitError::InvalidConfig);
    invalid = config_;
    invalid.delivery_mode = DeliveryMode::ReliableOrdered;
    expect_error(invalid, InitError::InvalidConfig);
    invalid = config_;
    invalid.cpu = 0;
    expect_error(invalid, InitError::InvalidConfig);
    invalid = config_;
    invalid.receivers.front().address = "not-an-ip";
    expect_error(invalid, InitError::AddressResolution);
    invalid = config_;
    invalid.receivers.front().port = 0;
    expect_error(invalid, InitError::AddressResolution);
    invalid = config_;
    invalid.receivers.push_back({"not-an-ip", 9000});
    expect_error(invalid, InitError::AddressResolution);
    invalid = config_;
    invalid.receivers.push_back({"127.0.0.1", 0});
    expect_error(invalid, InitError::AddressResolution);
}

TEST_F(SenderTest, ReportsMissingSharedMemoryAndInvalidRing) {
    config_.ingress_shm_name += "-missing";
    auto missing = Sender::initialize(config_);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error(), InitError::SharedMemory);
    config_.ingress_shm_name = name_;
    auto* header = reinterpret_cast<shm::RingHeader*>(segment_->bytes().data());
    header->preamble.magic[0] = 'X';
    auto invalid = Sender::initialize(config_);
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error(), InitError::Ring);
}

} // namespace
} // namespace lle
