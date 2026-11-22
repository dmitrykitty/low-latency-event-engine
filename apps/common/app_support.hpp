#pragma once

// Small helpers shared by the command-line programs: option parsing, CPU pinning,
// signal handling, and printing. Nothing here is on an event's hot path.

#include "lle/config.hpp"
#include "lle/init_error.hpp"
#include "lle/stats.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <format>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <map>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace lle::app {

// --name=value options; a name may repeat (for example --dest). Bare --name means "true".
class Options {
  public:
    Options(int argc, char** argv, std::initializer_list<std::string_view> known) {
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg{argv[i]};
            if (!arg.starts_with("--")) {
                throw std::runtime_error(std::format("unexpected argument '{}'", arg));
            }
            const auto eq = arg.find('=');
            const std::string name{arg.substr(2, eq == std::string_view::npos ? arg.npos : eq - 2)};
            if (name != "help" && std::ranges::find(known, name) == known.end()) {
                throw std::runtime_error(std::format("unknown option '--{}'", name));
            }
            values_[name].emplace_back(eq == std::string_view::npos ? "true" : arg.substr(eq + 1));
        }
    }

    [[nodiscard]] bool has(const std::string& name) const { return values_.contains(name); }

    [[nodiscard]] std::string text(const std::string& name, std::string fallback = {}) const {
        const auto found = values_.find(name);
        return found == values_.end() ? std::move(fallback) : found->second.back();
    }

    [[nodiscard]] std::string required(const std::string& name) const {
        if (!has(name)) {
            throw std::runtime_error(std::format("--{} is required", name));
        }
        return text(name);
    }

    [[nodiscard]] std::vector<std::string> all(const std::string& name) const {
        const auto found = values_.find(name);
        return found == values_.end() ? std::vector<std::string>{} : found->second;
    }

    [[nodiscard]] std::uint64_t number(const std::string& name, std::uint64_t fallback) const {
        if (!has(name)) {
            return fallback;
        }
        const auto value = text(name);
        std::uint64_t parsed = 0;
        const auto [ptr, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error != std::errc{} || ptr != value.data() + value.size()) {
            throw std::runtime_error(std::format("--{} must be an unsigned integer", name));
        }
        return parsed;
    }

  private:
    std::map<std::string, std::vector<std::string>, std::less<>> values_;
};

// "address:port" with a numeric IPv4 address; port 0 is allowed only where noted.
inline lle::Endpoint parse_endpoint(std::string_view text) {
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0) {
        throw std::runtime_error(std::format("'{}' is not address:port", text));
    }
    const auto port_text = text.substr(colon + 1);
    unsigned port = 0;
    const auto [ptr, error] =
        std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (error != std::errc{} || ptr != port_text.data() + port_text.size() || port > 65535) {
        throw std::runtime_error(std::format("'{}' has an invalid port", text));
    }
    return {std::string{text.substr(0, colon)}, static_cast<std::uint16_t>(port)};
}

// pins the calling thread; the library leaves thread placement to the application.
inline void pin_to_cpu(int cpu) {
    if (cpu < 0 || cpu >= CPU_SETSIZE) {
        throw std::runtime_error("cpu id outside supported range");
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(cpu), &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0 || sched_getcpu() != cpu) {
        throw std::runtime_error(std::format("cannot run on cpu {}", cpu));
    }
}

inline void pin_if_requested(const Options& options) {
    if (options.has("cpu")) {
        pin_to_cpu(static_cast<int>(options.number("cpu", 0)));
    }
}

// CLOCK_MONOTONIC in nanoseconds: one clock for every process on this host, so a producer
// timestamp and a consumer reading can be subtracted. Not comparable across hosts.
inline std::uint64_t monotonic_ns() noexcept {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000U +
           static_cast<std::uint64_t>(now.tv_nsec);
}

// SIGINT/SIGTERM handling without a signal handler. The constructor blocks the signals,
// so it must run in main() before any thread starts: every later thread inherits the mask
// and no thread is interrupted at a random point. A waiter thread then receives them with
// sigwait() and runs ordinary code, such as calling Sender::request_stop().
class StopSignal {
  public:
    StopSignal() {
        sigemptyset(&set_);
        sigaddset(&set_, SIGINT);
        sigaddset(&set_, SIGTERM);
        sigaddset(&set_, SIGUSR1); // finish() uses it to release the waiter
        if (pthread_sigmask(SIG_BLOCK, &set_, nullptr) != 0) {
            throw std::runtime_error("cannot block signals");
        }
    }

    StopSignal(const StopSignal&) = delete;
    StopSignal& operator=(const StopSignal&) = delete;

    ~StopSignal() { finish(); }

    // Consume a stop signal that arrived before the waiter started (non-blocking).
    bool poll() {
        const timespec zero{};
        int received = 0;
        while ((received = sigtimedwait(&set_, nullptr, &zero)) > 0) {
            if (received != SIGUSR1) {
                requested_.store(true, std::memory_order_relaxed);
            }
        }
        return requested();
    }

    // Call on_stop once, from the waiter thread, when SIGINT or SIGTERM arrives. If one
    // already arrived, call it now instead.
    void start(std::function<void()> on_stop) {
        if (poll()) {
            on_stop();
            return;
        }
        waiter_ = std::thread([this, on_stop = std::move(on_stop)] {
            int received = 0;
            while (sigwait(&set_, &received) == 0) {
                if (received == SIGUSR1) {
                    return;
                }
                requested_.store(true, std::memory_order_relaxed);
                on_stop();
                return;
            }
        });
    }

    // Stop waiting (the work ended on its own) and join the waiter.
    void finish() {
        if (waiter_.joinable()) {
            static_cast<void>(pthread_kill(waiter_.native_handle(), SIGUSR1));
            waiter_.join();
        }
    }

    // Set once a stop signal was received; loops without an engine poll this flag.
    [[nodiscard]] bool requested() const noexcept {
        return requested_.load(std::memory_order_relaxed);
    }

  private:
    sigset_t set_{};
    std::atomic<bool> requested_{false};
    std::thread waiter_;
};

inline std::string_view describe(lle::InitError error) {
    switch (error) {
    case lle::InitError::InvalidConfig: return "invalid configuration";
    case lle::InitError::SharedMemory: return "shared memory: cannot create or open the segment";
    case lle::InitError::Ring: return "ring: segment is not a ready LLE ring";
    case lle::InitError::AddressResolution: return "invalid IPv4 address";
    case lle::InitError::Socket: return "socket: cannot open or bind";
    case lle::InitError::SessionId: return "cannot draw a random id";
    }
    return "unknown error";
}

// Retry initialization while the peer that owns the segment has not created it yet.
template <class Init>
auto retry_until_ready(Init init, std::chrono::milliseconds wait, StopSignal& stop) {
    const auto deadline = std::chrono::steady_clock::now() + wait;
    for (;;) {
        auto result = init();
        const bool not_yet = !result && (result.error() == lle::InitError::SharedMemory ||
                                         result.error() == lle::InitError::Ring);
        if (!not_yet || std::chrono::steady_clock::now() >= deadline || stop.poll()) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}

inline void print_stats(std::string_view who, const lle::EngineStats& s, bool sender) {
    std::cout << std::format("{}: events_in={} events_out={} events_dropped={}\n", who,
                             s.events_in, s.events_out, s.events_dropped);
    if (sender) {
        std::cout << std::format("{}: packets_sent={} send_errors={} encode_errors={} "
                                 "ingress_errors={}\n",
                                 who, s.packets_sent, s.send_errors, s.encode_errors,
                                 s.ingress_errors);
    } else {
        std::cout << std::format("{}: packets_received={} gaps_detected={} packets_missed={} "
                                 "duplicate_or_old={} wrong_session={} malformed={} "
                                 "egress_full={} egress_errors={} receive_errors={}\n",
                                 who, s.packets_received, s.gaps_detected, s.packets_missed,
                                 s.duplicate_or_old_packets, s.wrong_session_packets,
                                 s.malformed_packets, s.egress_full, s.egress_errors,
                                 s.receive_errors);
    }
}

// nearest-rank percentile of sorted values
inline std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double p) {
    const auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(sorted.size())));
    return sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1];
}

} // namespace lle::app
