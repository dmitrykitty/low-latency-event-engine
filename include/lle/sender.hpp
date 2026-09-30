#pragma once

#include "lle/config.hpp"
#include "lle/init_error.hpp"
#include "lle/stats.hpp"
#include "lle/transport/udp_sender.hpp"
#include "shm/segment.hpp"
#include "shm/spsc_ring.hpp"

#include <atomic>
#include <expected>

namespace lle {

class Sender {
public:
    // The producer must finish initializing ingress SHM before this call.
    // This step supports BestEffort delivery to one numeric IPv4 endpoint.
    [[nodiscard]] static std::expected<Sender, InitError> initialize(SenderConfig config);

    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;
    // Move only before starting run(), or after it has returned.
    Sender(Sender&& other) noexcept;
    Sender& operator=(Sender&&) = delete;
    ~Sender();

    // Call on one thread. Drain a closed ingress, or stop when requested.
    void run();
    // May be called from another thread. An event already acquired finishes first.
    void request_stop() noexcept;

    // Read only after run() returns (join the worker thread first).
    [[nodiscard]] EngineStats stats() const noexcept;

private:
    Sender(
        SenderConfig config,
        shm::SharedMemorySegment segment,
        shm::SpscRing ingress,
        transport::UdpSender udp,
        transport::UdpAddress destination,
        std::uint32_t session_id
    );

    SenderConfig config_;
    // Declared before the ring so its mapping outlives the ring handle.
    shm::SharedMemorySegment segment_;
    shm::SpscRing ingress_;
    transport::UdpSender udp_;
    transport::UdpAddress destination_;
    std::uint32_t session_id_;
    std::uint64_t packet_sequence_{1};
    std::atomic<bool> stop_requested_{false};
    EngineStats stats_{};
};

} // namespace lle
