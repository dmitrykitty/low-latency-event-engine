// lle-sender: attach to an ingress ring created by a producer and send every event as an
// LLEP DATA packet to one or more receivers over UDP. Runs until the producer closes the
// ingress ring (after draining it) or until SIGINT/SIGTERM.

#include "app_support.hpp"
#include "lle/sender.hpp"
#include "lle/version.hpp"

#include <chrono>
#include <exception>
#include <format>
#include <iostream>
#include <string>

namespace {

constexpr std::string_view usage =
    "usage: lle-sender --ingress=/name --dest=address:port [--dest=...] [options]\n"
    "  --ingress=/name        ingress ring created by the producer\n"
    "  --dest=address:port    numeric IPv4 receiver; repeat for fan-out\n"
    "  --max-datagram=N       LLEP packet size limit, 44..1416 (default 1416)\n"
    "  --idle=spin|sleep      empty ingress: busy-spin (default) or sleep 1 ms\n"
    "  --cpu=N                pin the sending thread to CPU N\n"
    "  --wait-ms=N            wait up to N ms for the ingress ring to appear (default 5000)\n";

int run(int argc, char** argv) {
    lle::app::StopSignal stop; // before any thread exists
    const lle::app::Options options(
        argc, argv, {"ingress", "dest", "max-datagram", "idle", "cpu", "wait-ms"});
    if (options.has("help")) {
        std::cout << usage;
        return 0;
    }

    lle::SenderConfig config;
    config.ingress_shm_name = options.required("ingress");
    for (const auto& dest : options.all("dest")) {
        config.receivers.push_back(lle::app::parse_endpoint(dest));
    }
    if (config.receivers.empty()) {
        throw std::runtime_error("at least one --dest is required");
    }
    config.max_datagram_bytes =
        static_cast<std::uint32_t>(options.number("max-datagram", config.max_datagram_bytes));
    const auto idle = options.text("idle", "spin");
    if (idle != "spin" && idle != "sleep") {
        throw std::runtime_error("--idle must be spin or sleep");
    }
    config.busy_spin_ingress = idle == "spin";
    lle::app::pin_if_requested(options);

    // the producer may still be creating the ring; retry until it is ready.
    auto sender = lle::app::retry_until_ready(
        [&] { return lle::Sender::initialize(config); },
        std::chrono::milliseconds{options.number("wait-ms", 5000)}, stop);
    if (!sender && stop.requested()) {
        return 0; // interrupted while waiting for the producer
    }
    if (!sender) {
        std::cerr << "lle-sender: " << lle::app::describe(sender.error()) << '\n';
        return 1;
    }

    std::cout << std::format("lle-sender {}: ingress {}, {} destination(s)\n", lle::version(),
                             config.ingress_shm_name, config.receivers.size())
              << std::flush;
    stop.start([&] { sender->request_stop(); });
    sender->run();
    stop.finish();

    const auto stats = sender->stats();
    lle::app::print_stats("lle-sender", stats, true);
    return stats.ingress_errors == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "lle-sender: " << error.what() << '\n';
        return 2;
    }
}
