#include "lle/receiver.hpp"

#include "lle/protocol/decoder.hpp"

#include <array>
#include <cerrno>
#include <utility>

#include <arpa/inet.h>
#include <sys/random.h>

namespace lle {

std::expected<Receiver, InitError> Receiver::initialize(ReceiverConfig config) {
    if (config.egress_shm_name.empty() || config.delivery_mode != DeliveryMode::BestEffort ||
        config.cpu != -1 || config.busy_poll_us != 0) {
        return std::unexpected(InitError::InvalidConfig);
    }

    const auto size = shm::SpscRing::required_bytes(config.egress_ring);
    if (!size) {
        return std::unexpected(InitError::InvalidConfig);
    }

    // An empty listen address means all local IPv4 addresses. Port 0 is allowed.
    transport::UdpAddress listen{};
    listen.native.sin_family = AF_INET;
    listen.native.sin_port = htons(config.listen.port);
    const char* address = config.listen.address.empty() ? "0.0.0.0"
                                                       : config.listen.address.c_str();
    if (inet_pton(AF_INET, address, &listen.native.sin_addr) != 1) {
        return std::unexpected(InitError::AddressResolution);
    }
    // Bind before creating SHM so a socket failure leaves no segment to unlink.
    auto udp = transport::UdpReceiver::bind(listen);
    if (!udp) {
        return std::unexpected(InitError::Socket);
    }

    // The ring needs its own random identity; this is not the sender's session ID.
    std::uint64_t instance_id = 0;
    while (instance_id == 0) {
        const auto bytes = getrandom(&instance_id, sizeof(instance_id), 0);
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        if (bytes != static_cast<ssize_t>(sizeof(instance_id))) {
            return std::unexpected(InitError::Ring);
        }
    }

    auto segment = shm::SharedMemorySegment::create_shm(config.egress_shm_name, *size);
    if (!segment) {
        return std::unexpected(InitError::SharedMemory);
    }
    auto egress = shm::SpscRing::initialize(segment->bytes(), config.egress_ring, instance_id);
    if (!egress) {
        // The segment destructor only closes/unmaps; its owner must unlink the name.
        static_cast<void>(segment->unlink());
        return std::unexpected(InitError::Ring);
    }

    return Receiver{
        std::move(config),
        std::move(*segment),
        std::move(*egress),
        std::move(*udp)
    };
}

Receiver::Receiver(
    ReceiverConfig config,
    shm::SharedMemorySegment segment,
    shm::SpscRing egress,
    transport::UdpReceiver udp
)
    : config_(std::move(config)),
      segment_(std::move(segment)),
      egress_(std::move(egress)),
      udp_(std::move(udp)) {}

Receiver::Receiver(Receiver&& other) noexcept
    : config_(std::move(other.config_)),
      segment_(std::move(other.segment_)),
      egress_(std::move(other.egress_)),
      udp_(std::move(other.udp_)),
      // Moving is allowed only when no thread is running the receiver.
      stop_requested_(other.stop_requested_.load(std::memory_order_relaxed)),
      stats_(other.stats_),
      gap_detector_(other.gap_detector_) {
    other.request_stop();
}

Receiver::~Receiver() {
    // A moved-from receiver no longer owns the ring or the segment name.
    if (segment_.is_owner()) {
        static_cast<void>(egress_.close_publication());
        static_cast<void>(segment_.unlink());
    }
}

void Receiver::run() {
    alignas(8) std::array<std::byte, protocol::kMaxPacketBytes> packet{};
    // poll() wakes immediately for data; the timeout only bounds idle stop latency.
    constexpr int receive_timeout_ms = 10;

    // The stop flag carries no other data, so relaxed ordering is sufficient.
    while (!stop_requested_.load(std::memory_order_relaxed)) {
        const auto datagram = udp_.receive(packet, receive_timeout_ms);
        if (!datagram) {
            const auto error = datagram.error();
            if (error.code == transport::UdpErrorCode::Timeout || error.error_number == EINTR) {
                continue;
            }
            if (error.code == transport::UdpErrorCode::DatagramTooLarge) {
                ++stats_.packets_received;
                ++stats_.malformed_packets;
                continue;
            }
            ++stats_.receive_errors;
            break;
        }

        ++stats_.packets_received;
        const auto decoded = protocol::decode_data(std::span{packet}.first(datagram->size));
        if (!decoded) {
            ++stats_.malformed_packets;
            continue;
        }

        const auto checked = gap_detector_.check(decoded->session_id, decoded->packet_sequence);
        if (checked.status != PacketStatus::Accepted) {
            if (checked.status == PacketStatus::DuplicateOrOld) {
                ++stats_.duplicate_or_old_packets;
            } else {
                ++stats_.wrong_session_packets;
            }
            ++stats_.events_dropped;
            continue;
        }

        if (checked.gap > 0) {
            ++stats_.gaps_detected;
            stats_.packets_missed += checked.gap;
        }

        ++stats_.events_in;
        // try_publish copies the payload before the next receive reuses packet.
        const auto published = egress_.try_publish(decoded->event);
        if (published == PublishResult::Ok) {
            ++stats_.events_out;
        } else {
            ++stats_.events_dropped;
            if (published == PublishResult::Full) {
                ++stats_.egress_full;
            } else {
                ++stats_.egress_errors;
            }
            if (published == PublishResult::Closed) {
                break;
            }
        }
    }

    // Publish closure after the final event so the consumer can drain and finish.
    static_cast<void>(egress_.close_publication());
}

void Receiver::request_stop() noexcept {
    // This flag only requests termination; it does not publish other data.
    stop_requested_.store(true, std::memory_order_relaxed);
}

EngineStats Receiver::stats() const noexcept {
    return stats_;
}

} // namespace lle
