#include "lle/sender.hpp"

#include "lle/protocol/encoder.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <limits>
#include <thread>
#include <utility>

#include <sys/random.h>

namespace lle {

//Initialize sender with correct config, otherwise return InitError
std::expected<Sender, InitError> Sender::initialize(SenderConfig config) {
    if (config.ingress_shm_name.empty() || config.receivers.empty() ||
        config.max_datagram_bytes < protocol::kMinDataPacketBytes ||
        config.max_datagram_bytes > protocol::kMaxPacketBytes ||
        config.delivery_mode != DeliveryMode::BestEffort || config.cpu != -1) {
        return std::unexpected(InitError::InvalidConfig);
    }

    // Parse and allocate once at startup, before attaching resources or sending.
    std::vector<transport::UdpAddress> destinations;
    destinations.reserve(config.receivers.size());
    for (const auto& endpoint : config.receivers) {
        const auto destination = transport::UdpAddress::ipv4(endpoint.address, endpoint.port);
        if (!destination) {
            return std::unexpected(InitError::AddressResolution);
        }
        destinations.push_back(*destination);
    }

    //open existing POSIX SHM
    auto segment = shm::SharedMemorySegment::attach_shm(config.ingress_shm_name);
    if (!segment) {
        return std::unexpected(InitError::SharedMemory);
    }
    //to read SHM bytes(ingress data) using SpscRing API
    auto ingress = shm::SpscRing::attach(segment->bytes());
    if (!ingress) {
        return std::unexpected(InitError::Ring);
    }
    auto udp = transport::UdpSender::open();
    if (!udp) {
        return std::unexpected(InitError::Socket);
    }

    // LLEP requires a nonzero session ID chosen at startup. No randomness in the loop.
    std::uint32_t session_id = 0;
    while (session_id == 0) {
        const auto bytes = getrandom(&session_id, sizeof(session_id), 0);
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        if (bytes != static_cast<ssize_t>(sizeof(session_id))) {
            return std::unexpected(InitError::SessionId);
        }
    }

    return Sender{
        std::move(config),
        std::move(*segment),
        std::move(*ingress),
        std::move(*udp),
        std::move(destinations),
        session_id
    };
}

Sender::Sender(
    SenderConfig config,
    shm::SharedMemorySegment segment,
    shm::SpscRing ingress,
    transport::UdpSender udp,
    std::vector<transport::UdpAddress> destinations,
    std::uint32_t session_id
)
    : config_(std::move(config)),
      segment_(std::move(segment)),
      ingress_(std::move(ingress)),
      udp_(std::move(udp)),
      destinations_(std::move(destinations)),
      session_id_(session_id) {}

Sender::Sender(Sender&& other) noexcept
    : config_(std::move(other.config_)),
      segment_(std::move(other.segment_)),
      ingress_(std::move(other.ingress_)),
      udp_(std::move(other.udp_)),
      destinations_(std::move(other.destinations_)),
      session_id_(other.session_id_),
      packet_sequence_(other.packet_sequence_),
      // Moving is allowed only when no thread is running the sender.
      stop_requested_(other.stop_requested_.load(std::memory_order_relaxed)),
      stats_(other.stats_) {
    other.request_stop();
}

Sender::~Sender() = default;

void Sender::run() {
    alignas(8) std::array<std::byte, protocol::kMaxPacketBytes> packet{};
    const auto output = std::span{packet}.first(config_.max_datagram_bytes);

    // The stop flag carries no other data, so relaxed ordering is sufficient.
    while (!stop_requested_.load(std::memory_order_relaxed)) {
        auto event = ingress_.try_acquire();
        if (!event) {
            if (event.error() == shm::AcquireError::Empty) {
                if (!config_.busy_spin_ingress) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                continue;
            }
            if (event.error() != shm::AcquireError::Closed) {
                ++stats_.ingress_errors;
            }
            break;
        }

        ++stats_.events_in;
        const auto encoded =
            protocol::encode_data(*event, session_id_, packet_sequence_, output);
        // Encoding copied the payload; release SHM before sending to any destination.
        // BestEffort consumes failed events too. All payload reads finish before release.
        static_cast<void>(ingress_.release());
        if (!encoded) {
            ++stats_.encode_errors;
            ++stats_.events_dropped;
        } else {
            const auto datagram = output.first(*encoded);
            bool sent_to_any = false;
            for (const auto& destination : destinations_) {
                if (udp_.send_to(datagram, destination)) {
                    ++stats_.packets_sent;
                    sent_to_any = true;
                } else {
                    // An error for one destination must not skip the remaining ones.
                    ++stats_.send_errors;
                }
            }
            // Event counters count input events, not the number of destinations.
            if (sent_to_any) {
                ++stats_.events_out;
            } else {
                ++stats_.events_dropped;
            }
        }

        if (encoded) {
            if (packet_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
                request_stop(); // End the session instead of wrapping the sequence to zero.
                break;
            }
            ++packet_sequence_;
        }
    }
}

void Sender::request_stop() noexcept {
    // This flag only requests termination; it does not publish other data.
    stop_requested_.store(true, std::memory_order_relaxed);
}

EngineStats Sender::stats() const noexcept {
    return stats_;
}

} // namespace lle
