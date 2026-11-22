// lle-demo-consumer: attach to the egress ring a Receiver created and read every event,
// checking order and contents. Reports how many events arrived, how many sequence numbers
// were skipped, and (on one host) end-to-end latency from the producer's CLOCK_MONOTONIC
// timestamp. Exits when the Receiver closes the ring, after --expect events, or on Ctrl+C.

#include "app_support.hpp"
#include "shm/segment.hpp"
#include "shm/spsc_ring.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <format>
#include <iostream>
#include <optional>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view usage =
    "usage: lle-demo-consumer --egress=/name [options]\n"
    "  --egress=/name         egress ring created by lle-receiver\n"
    "  --expect=N             exit after N events (default: until the ring is closed)\n"
    "  --latency=on|off       report latency from producer timestamps (default on; same host only)\n"
    "  --max-samples=N        latency samples kept (default 2000000)\n"
    "  --idle=spin|sleep      empty ring: busy-spin (default) or sleep 100 us\n"
    "  --progress             print the event count once per second\n"
    "  --cpu=N                pin to CPU N\n"
    "  --wait-ms=N            wait up to N ms for the egress ring to appear (default 5000)\n";

struct Attached {
    lle::shm::SharedMemorySegment segment;
    lle::shm::SpscRing ring;
};

// The Receiver may still be creating the segment: retry on "missing", "empty", "not ready".
std::optional<Attached> attach(const std::string& name, std::chrono::milliseconds wait,
                               lle::app::StopSignal& stop) {
    const auto deadline = std::chrono::steady_clock::now() + wait;
    for (;;) {
        auto segment = lle::shm::SharedMemorySegment::attach_shm(name);
        if (segment) {
            auto ring = lle::shm::SpscRing::attach(segment->bytes());
            if (ring) {
                return Attached{std::move(*segment), std::move(*ring)};
            }
            if (ring.error() != lle::shm::RingError::NotReady) {
                throw std::runtime_error(std::format("{} is not a valid LLE ring", name));
            }
        } else if (segment.error().error_number != ENOENT &&
                   segment.error().error_number != EINVAL) {
            throw std::runtime_error(std::format("cannot open {}: {}", name,
                                                 std::strerror(segment.error().error_number)));
        }
        if (std::chrono::steady_clock::now() >= deadline || stop.requested()) {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}

int run(int argc, char** argv) {
    lle::app::StopSignal stop; // before any thread exists
    stop.start([] {});         // Ctrl+C only sets the flag the read loop checks
    const lle::app::Options options(argc, argv,
        {"egress", "expect", "latency", "max-samples", "idle", "progress", "cpu", "wait-ms"});
    if (options.has("help")) {
        std::cout << usage;
        return 0;
    }
    const auto name = options.required("egress");
    const auto expect = options.number("expect", 0);
    const bool latency = options.text("latency", "on") == "on";
    const bool sleep_when_idle = options.text("idle", "spin") == "sleep";
    const bool progress = options.has("progress");
    lle::app::pin_if_requested(options);

    auto attached = attach(name, std::chrono::milliseconds{options.number("wait-ms", 5000)}, stop);
    if (!attached) {
        if (stop.requested()) {
            return 0;
        }
        std::cerr << std::format("lle-demo-consumer: {} did not appear\n", name);
        return 1;
    }
    std::cout << std::format("lle-demo-consumer: attached to {}\n", name) << std::flush;

    auto& ring = attached->ring;
    std::vector<std::uint64_t> samples;
    samples.reserve(latency ? options.number("max-samples", 2'000'000) : 0); // no growth in the loop
    std::uint64_t received = 0;
    std::uint64_t missing = 0;
    std::uint64_t invalid = 0;
    std::uint64_t last_sequence = 0;
    std::uint64_t first_sequence = 0;
    auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    std::uint64_t reported = 0;

    while (!stop.requested() && (expect == 0 || received < expect)) {
        auto event = ring.try_acquire();
        if (!event) {
            if (event.error() == lle::shm::AcquireError::Closed) {
                break; // the Receiver stopped and everything queued has been read
            }
            if (event.error() != lle::shm::AcquireError::Empty) {
                std::cerr << "lle-demo-consumer: invalid slot in the egress ring\n";
                return 1;
            }
            if (sleep_when_idle) {
                std::this_thread::sleep_for(std::chrono::microseconds{100});
            }
            if (progress && std::chrono::steady_clock::now() >= next_report) {
                std::cout << std::format("lle-demo-consumer: {} events (+{})\n", received,
                                         received - reported) << std::flush;
                reported = received;
                next_report += std::chrono::seconds{1};
            }
            continue;
        }
        const auto now = lle::app::monotonic_ns();
        const auto sequence = event->sequence;
        std::uint64_t payload_sequence = 0;
        if (event->payload.size() >= sizeof payload_sequence) {
            std::memcpy(&payload_sequence, event->payload.data(), sizeof payload_sequence);
        }
        if (payload_sequence != sequence || sequence <= last_sequence) {
            ++invalid; // corrupted, duplicated, or out of order
        } else {
            if (last_sequence != 0) {
                missing += sequence - last_sequence - 1;
            } else {
                first_sequence = sequence;
            }
            last_sequence = sequence;
        }
        if (latency && samples.size() < samples.capacity() && now >= event->source_timestamp_ns) {
            samples.push_back(now - event->source_timestamp_ns);
        }
        static_cast<void>(ring.release());
        ++received;
    }

    std::cout << std::format("lle-demo-consumer: received={} missing={} invalid={} first={} "
                             "last={}\n",
                             received, missing, invalid, first_sequence, last_sequence);
    if (latency && !samples.empty()) {
        std::ranges::sort(samples);
        const auto us = [](std::uint64_t ns) { return static_cast<double>(ns) / 1000.0; };
        std::cout << std::format("lle-demo-consumer: latency_us samples={} p50={:.1f} p90={:.1f} "
                                 "p99={:.1f} p99.9={:.1f} max={:.1f}\n",
                                 samples.size(), us(lle::app::percentile(samples, 0.50)),
                                 us(lle::app::percentile(samples, 0.90)),
                                 us(lle::app::percentile(samples, 0.99)),
                                 us(lle::app::percentile(samples, 0.999)), us(samples.back()));
    }
    return invalid == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "lle-demo-consumer: " << error.what() << '\n';
        return 2;
    }
}
