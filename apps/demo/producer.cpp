// lle-demo-producer: create an ingress ring and publish numbered events into it at a chosen
// rate. Stands in for a real application (for example a market-data handler). Each payload
// starts with the event's sequence number; source_timestamp_ns is CLOCK_MONOTONIC, so a
// consumer on the same host can compute end-to-end latency.

#include "app_support.hpp"
#include "shm/segment.hpp"
#include "shm/spsc_ring.hpp"

#include <chrono>
#include <cstring>
#include <exception>
#include <format>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view usage =
    "usage: lle-demo-producer --ingress=/name [options]\n"
    "  --ingress=/name        ingress ring to create (the Sender attaches to it)\n"
    "  --count=N              events to publish; 0 = until Ctrl+C (default 100000)\n"
    "  --rate=N               events per second; 0 = as fast as the ring accepts (default 100000)\n"
    "  --payload=N            payload bytes, 8..1374 (default 64)\n"
    "  --slots=N              ring capacity, power of two (default 1024)\n"
    "  --stream=N             stream id (default 1)\n"
    "  --on-full=wait|drop    full ring: wait for space (default) or drop the event\n"
    "  --start-delay-ms=N     wait before the first event, e.g. for the Sender to attach\n"
    "  --cpu=N                pin to CPU N\n";

std::uint64_t random_nonzero() {
    std::random_device device;
    std::uint64_t value = 0;
    while (value == 0) {
        value = (static_cast<std::uint64_t>(device()) << 32U) | device();
    }
    return value;
}

int run(int argc, char** argv) {
    lle::app::StopSignal stop; // before any thread exists
    stop.start([] {}); // Ctrl+C only sets the flag the publishing loop checks
    const lle::app::Options options(argc, argv,
        {"ingress", "count", "rate", "payload", "slots", "stream", "on-full", "start-delay-ms",
         "cpu"});
    if (options.has("help")) {
        std::cout << usage;
        return 0;
    }

    const auto name = options.required("ingress");
    const auto count = options.number("count", 100'000);
    const auto rate = options.number("rate", 100'000);
    const auto payload_size = options.number("payload", 64);
    if (payload_size < sizeof(std::uint64_t) || payload_size > 1374) {
        throw std::runtime_error("--payload must be between 8 and 1374");
    }
    const auto on_full = options.text("on-full", "wait");
    if (on_full != "wait" && on_full != "drop") {
        throw std::runtime_error("--on-full must be wait or drop");
    }
    const lle::shm::RingConfig ring_config{
        static_cast<std::uint32_t>(options.number("slots", 1024)),
        static_cast<std::uint32_t>(payload_size)};
    lle::app::pin_if_requested(options);

    const auto size = lle::shm::SpscRing::required_bytes(ring_config);
    if (!size) {
        throw std::runtime_error("--slots must be a power of two of at least 2");
    }
    auto segment = lle::shm::SharedMemorySegment::create_shm(name, *size);
    if (!segment) {
        throw std::runtime_error(std::format(
            "cannot create {}: {} (a stale segment can be removed with rm /dev/shm{})", name,
            std::strerror(segment.error().error_number), name));
    }
    // remove the name whatever happens next; an attached Sender keeps its own mapping.
    struct Unlink {
        lle::shm::SharedMemorySegment& segment;
        ~Unlink() { static_cast<void>(segment.unlink()); }
    } unlink{*segment};

    auto ring = lle::shm::SpscRing::initialize(segment->bytes(), ring_config, random_nonzero());
    if (!ring) {
        throw std::runtime_error("cannot initialize the ingress ring");
    }
    std::cout << std::format("lle-demo-producer: ingress {} ready ({} slots, {} B payload)\n",
                             name, ring_config.slot_count, payload_size)
              << std::flush;
    std::this_thread::sleep_for(std::chrono::milliseconds{options.number("start-delay-ms", 0)});

    std::vector<std::byte> payload(payload_size, std::byte{0x5a});
    lle::EventView event{
        .stream_id = static_cast<lle::StreamId>(options.number("stream", 1)),
        .payload = payload};
    const bool drop_when_full = on_full == "drop";
    const auto period_ns = rate == 0 ? 0 : 1'000'000'000U / rate;
    std::uint64_t published = 0;
    std::uint64_t dropped = 0;
    std::uint64_t full_waits = 0;
    const auto start = lle::app::monotonic_ns();

    for (std::uint64_t sequence = 1; (count == 0 || sequence <= count) && !stop.requested();
         ++sequence) {
        if (period_ns != 0) {
            // pace by schedule, not by "sleep after each event", so the rate does not drift.
            const auto due = start + (sequence - 1) * period_ns;
            auto now = lle::app::monotonic_ns();
            if (due > now + 200'000) {
                std::this_thread::sleep_for(std::chrono::nanoseconds{due - now - 100'000});
            }
            while ((now = lle::app::monotonic_ns()) < due) {
            }
        }
        std::memcpy(payload.data(), &sequence, sizeof sequence);
        event.sequence = sequence;
        event.source_timestamp_ns = lle::app::monotonic_ns();
        auto result = ring->try_publish(event);
        while (result == lle::PublishResult::Full && !drop_when_full && !stop.requested()) {
            ++full_waits;
            result = ring->try_publish(event);
        }
        if (result == lle::PublishResult::Ok) {
            ++published;
        } else if (result == lle::PublishResult::Full) {
            ++dropped;
        } else {
            throw std::runtime_error("ingress ring rejected an event");
        }
    }
    const auto elapsed_s = static_cast<double>(lle::app::monotonic_ns() - start) / 1e9;

    // the Sender drains what is queued, then sees Closed and exits.
    static_cast<void>(ring->close_publication());
    std::cout << std::format("lle-demo-producer: published={} dropped={} full_spins={} "
                             "rate={:.0f}/s\n",
                             published, dropped, full_waits,
                             elapsed_s > 0 ? static_cast<double>(published) / elapsed_s : 0.0);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "lle-demo-producer: " << error.what() << '\n';
        return 2;
    }
}
