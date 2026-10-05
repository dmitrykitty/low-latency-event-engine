#pragma once

#include "lle/config.hpp"
#include "lle/init_error.hpp"
#include "lle/stats.hpp"
#include "lle/transport/udp_receiver.hpp"
#include "shm/segment.hpp"
#include "shm/spsc_ring.hpp"

#include <atomic>
#include <expected>

namespace lle {

class Receiver {
public:
    // Validate configuration, create and initialize egress SHM, and bind UDP.
    // This step supports BestEffort delivery of one event per DATA packet.
    // The consumer may attach only after initialization succeeds.
    [[nodiscard]] static std::expected<Receiver, InitError> initialize(ReceiverConfig config);

    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    // Move only before starting run(), or after it has returned.
    Receiver(Receiver&& other) noexcept;
    Receiver& operator=(Receiver&&) = delete;
    // Join the worker first. The receiver owns egress SHM and must unlink it.
    ~Receiver();

    // Call on one thread. Decode DATA packets and publish their events to egress.
    // Drop malformed packets and events that cannot be published (BestEffort).
    // Close egress publication before returning; queued events remain readable.
    void run();
    // May be called from another thread. A datagram already received finishes first.
    // run() must use a finite receive timeout to observe this flag when UDP is idle.
    void request_stop() noexcept;

    // Read only after run() returns (join the worker thread first).
    [[nodiscard]] EngineStats stats() const noexcept;

private:
    Receiver(
        ReceiverConfig config,
        shm::SharedMemorySegment segment,
        shm::SpscRing egress,
        transport::UdpReceiver udp
    );

    ReceiverConfig config_;
    // Declared before the ring so its mapping outlives the ring handle.
    shm::SharedMemorySegment segment_;
    shm::SpscRing egress_;
    transport::UdpReceiver udp_;
    // Shared with request_stop(); the data-path state is used by run() alone.
    std::atomic<bool> stop_requested_{false};
    EngineStats stats_{};
};

} // namespace lle

