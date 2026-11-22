// lle-receiver: listen for LLEP DATA packets on UDP and publish their events into an egress
// ring that a local consumer attaches to. Creates the egress ring and removes it on exit.
// Runs until SIGINT/SIGTERM; then it closes the ring so the consumer can drain and finish.

#include "app_support.hpp"
#include "lle/receiver.hpp"
#include "lle/version.hpp"

#include <exception>
#include <format>
#include <iostream>
#include <string>

namespace {

constexpr std::string_view usage =
    "usage: lle-receiver --listen=address:port --egress=/name [options]\n"
    "  --listen=address:port  IPv4 address to bind (0.0.0.0 for all); port 0 picks a free one\n"
    "  --egress=/name         egress ring to create for the consumer\n"
    "  --slots=N              egress ring capacity, power of two (default 1024)\n"
    "  --capacity=N           largest payload in bytes (default 1374, the LLEP maximum)\n"
    "  --cpu=N                pin the receiving thread to CPU N\n";

int run(int argc, char** argv) {
    lle::app::StopSignal stop; // before any thread exists
    const lle::app::Options options(argc, argv, {"listen", "egress", "slots", "capacity", "cpu"});
    if (options.has("help")) {
        std::cout << usage;
        return 0;
    }

    lle::ReceiverConfig config;
    config.listen = lle::app::parse_endpoint(options.required("listen"));
    config.egress_shm_name = options.required("egress");
    config.egress_ring = {
        static_cast<std::uint32_t>(options.number("slots", 1024)),
        static_cast<std::uint32_t>(options.number("capacity", 1374)),
    };
    lle::app::pin_if_requested(options);

    auto receiver = lle::Receiver::initialize(config);
    if (!receiver) {
        std::cerr << "lle-receiver: " << lle::app::describe(receiver.error()) << '\n';
        return 1;
    }

    // scripts read the port from this line when --listen used port 0.
    std::cout << std::format("lle-receiver {}: listening on {}:{}, egress {} ({} slots, {} B)\n",
                             lle::version(), config.listen.address, receiver->local_port(),
                             config.egress_shm_name, config.egress_ring.slot_count,
                             config.egress_ring.slot_payload_capacity)
              << std::flush;
    stop.start([&] { receiver->request_stop(); });
    receiver->run();
    stop.finish();

    const auto stats = receiver->stats();
    lle::app::print_stats("lle-receiver", stats, false);
    return stats.receive_errors == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "lle-receiver: " << error.what() << '\n';
        return 2;
    }
}
