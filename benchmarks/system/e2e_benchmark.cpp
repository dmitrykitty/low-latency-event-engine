// End-to-end benchmark of the whole engine path on one host:
//
//   producer -> ingress ring (SHM) -> lle::Sender -> UDP loopback -> lle::Receiver
//            -> egress ring (SHM) -> consumer
//
// Four threads, each pinned to its own CPU (LLE_E2E_CPUS=producer,sender,receiver,consumer).
//
// latency: closed loop, one event in flight. The producer stamps a fenced rdtsc into
//   source_timestamp_ns just before try_publish; the consumer reads rdtsc right after
//   try_acquire. The difference is one-way latency across two cores, so the TSC offset
//   between those cores is measured and reported before the run.
// throughput: the producer publishes as fast as the ingress ring accepts; the run reports
//   what reached the consumer and where the rest was lost.

#include "bench_util.hpp"
#include "lle/receiver.hpp"
#include "lle/sender.hpp"
#include "shm/segment.hpp"
#include "shm/spsc_ring.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef LLE_BENCH_COMPILE_INFO
#define LLE_BENCH_COMPILE_INFO "unknown"
#endif

namespace {

namespace util = lle_bench::util;
using SteadyClock = std::chrono::steady_clock;

constexpr lle::StreamId stream_id = 7;
constexpr std::uint32_t ingress_slots = 1024;
constexpr std::int64_t lost_sample = std::numeric_limits<std::int64_t>::min();

struct Options {
    std::uint64_t samples{200'000};
    std::uint64_t warmup{20'000};
    std::uint64_t events{2'000'000};
    std::uint32_t egress_slots{1024};
    std::uint64_t timeout_ms{100};
    std::vector<std::size_t> sizes{16, 64, 256, 1024};
    bool latency{true};
    bool throughput{true};
    bool allow_smt_siblings{false};
    std::string latency_csv;
    std::string throughput_csv;
};

struct Cpus {
    int producer{-1};
    int sender{-1};
    int receiver{-1};
    int consumer{-1};
};

struct LatencyResult {
    std::size_t payload{};
    std::uint64_t samples{};
    std::uint64_t lost{};
    std::uint64_t negative{};
    double min{}, p50{}, p90{}, p99{}, p999{}, p9999{}, max{}, mean{};
    lle::EngineStats sender{};
    lle::EngineStats receiver{};
};

struct ThroughputResult {
    std::size_t payload{};
    std::uint64_t published{};
    std::uint64_t ingress_full{};
    std::uint64_t delivered{};
    double offered_mps{};   // events per second accepted by the ingress ring
    double delivered_mps{}; // events per second that reached the consumer
    std::uint64_t kernel_rcvbuf_errors{};
    lle::EngineStats sender{};
    lle::EngineStats receiver{};
};

// state written by one thread and polled by another lives in its own 128-byte block, away
// from every thread's stack, so polling it never shares a line or a prefetch pair.
struct alignas(128) Counter {
    std::atomic<std::uint64_t> value{0};
};

std::vector<std::string> split(std::string_view text) {
    std::vector<std::string> parts;
    while (!text.empty()) {
        const auto comma = text.find(',');
        parts.emplace_back(text.substr(0, comma));
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    return parts;
}

std::uint64_t parse_u64(std::string_view text, std::string_view flag) {
    std::uint64_t value = 0;
    const auto [ptr, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || ptr != text.data() + text.size()) {
        throw std::runtime_error(std::format("invalid value for {}: '{}'", flag, text));
    }
    return value;
}

Cpus read_cpus() {
    const char* value = std::getenv("LLE_E2E_CPUS");
    const auto parts = split(value != nullptr ? value : "2,3,4,5");
    if (parts.size() != 4) {
        throw std::runtime_error("LLE_E2E_CPUS must list four CPUs: producer,sender,receiver,consumer");
    }
    const Cpus cpus{
        util::parse_cpu(parts[0], "LLE_E2E_CPUS"),
        util::parse_cpu(parts[1], "LLE_E2E_CPUS"),
        util::parse_cpu(parts[2], "LLE_E2E_CPUS"),
        util::parse_cpu(parts[3], "LLE_E2E_CPUS"),
    };
    const std::array all{cpus.producer, cpus.sender, cpus.receiver, cpus.consumer};
    for (std::size_t i = 0; i < all.size(); ++i) {
        for (std::size_t j = i + 1; j < all.size(); ++j) {
            if (all[i] == all[j]) {
                throw std::runtime_error("LLE_E2E_CPUS must name four different CPUs");
            }
        }
    }
    return cpus;
}

bool any_smt_siblings(const Cpus& cpus) {
    const std::array all{cpus.producer, cpus.sender, cpus.receiver, cpus.consumer};
    for (std::size_t i = 0; i < all.size(); ++i) {
        for (std::size_t j = i + 1; j < all.size(); ++j) {
            if (util::same_physical_core(all[i], all[j])) {
                return true;
            }
        }
    }
    return false;
}

std::string read_line(const std::string& path) {
    std::ifstream file{path};
    std::string line;
    std::getline(file, line);
    return line.empty() ? std::string{"n/a"} : line;
}

// idle states a sleeping thread's CPU may enter, with their exit latency; the receiver
// waits in poll(), so its CPU can be idle between packets.
std::string idle_states(int cpu) {
    std::string out;
    for (int state = 0;; ++state) {
        const auto base = std::format("/sys/devices/system/cpu/cpu{}/cpuidle/state{}/", cpu, state);
        std::ifstream name_file{base + "name"};
        std::string name;
        if (!(name_file >> name)) {
            break;
        }
        const auto disabled = read_line(base + "disable");
        out += std::format("{}{} {}us{}", out.empty() ? "" : ", ", name,
                           read_line(base + "latency"), disabled == "1" ? " (disabled)" : "");
    }
    return out.empty() ? std::string{"n/a"} : out;
}

// system-wide count of datagrams the kernel dropped because a socket buffer was full.
std::uint64_t udp_rcvbuf_errors() {
    std::ifstream snmp{"/proc/net/snmp"};
    std::string header;
    std::string values;
    std::string line;
    while (std::getline(snmp, line)) {
        if (line.starts_with("Udp:")) {
            if (header.empty()) {
                header = line;
            } else {
                values = line;
                break;
            }
        }
    }
    std::istringstream names{header};
    std::istringstream numbers{values};
    std::string name;
    std::string number;
    while (names >> name && numbers >> number) {
        if (name == "RcvbufErrors") {
            return std::stoull(number);
        }
    }
    return 0;
}

// Offset of b's TSC relative to a's, estimated from the fastest of many request/reply
// exchanges: offset = t_b - (t_a_send + t_a_reply) / 2, accurate to half that round trip.
struct TscOffset {
    double offset_ns{};
    double bound_ns{};
};

TscOffset measure_tsc_offset(int cpu_a, int cpu_b, double tsc_hz) {
    constexpr std::uint64_t rounds = 100'000;
    const auto request = std::make_unique<Counter>();
    const auto reply = std::make_unique<Counter>();
    const auto reply_tsc = std::make_unique<Counter>();
    std::exception_ptr error;

    util::pin_and_verify(cpu_a);
    std::jthread peer([&error, cpu_b, req = request.get(), rep = reply.get(),
                       stamp = reply_tsc.get()] {
        try {
            util::pin_and_verify(cpu_b);
        } catch (...) {
            error = std::current_exception();
            rep->value.store(std::numeric_limits<std::uint64_t>::max(), std::memory_order_release);
            return;
        }
        for (std::uint64_t round = 1; round <= rounds; ++round) {
            while (req->value.load(std::memory_order_acquire) != round) {
            }
            stamp->value.store(util::tsc_end(), std::memory_order_relaxed);
            rep->value.store(round, std::memory_order_release);
        }
    });

    std::uint64_t best_rtt = std::numeric_limits<std::uint64_t>::max();
    double best_offset = 0.0;
    for (std::uint64_t round = 1; round <= rounds; ++round) {
        const auto sent = util::tsc_begin();
        request->value.store(round, std::memory_order_release);
        std::uint64_t answered = 0;
        while ((answered = reply->value.load(std::memory_order_acquire)) != round) {
            if (answered == std::numeric_limits<std::uint64_t>::max()) {
                peer.join();
                std::rethrow_exception(error);
            }
        }
        const auto received = util::tsc_end();
        const auto stamped = reply_tsc->value.load(std::memory_order_relaxed);
        if (received - sent < best_rtt) {
            best_rtt = received - sent;
            best_offset = static_cast<double>(stamped) -
                          (static_cast<double>(sent) + static_cast<double>(received)) / 2.0;
        }
    }
    peer.join();
    const double ns_per_tick = 1e9 / tsc_hz;
    return {best_offset * ns_per_tick, static_cast<double>(best_rtt) / 2.0 * ns_per_tick};
}

std::uint64_t random_instance_id() {
    std::random_device device;
    std::uint64_t id = 0;
    while (id == 0) {
        id = (static_cast<std::uint64_t>(device()) << 32U) | device();
    }
    return id;
}

// One complete engine: producer-owned ingress, Sender, Receiver (owns egress), consumer
// attachment. Built in the order the ownership rules require.
class Engine {
  public:
    Engine(std::size_t payload, std::uint32_t egress_slots, int index)
        : ingress_name_(std::format("/lle-e2e-{}-{}-in", getpid(), index)),
          egress_name_(std::format("/lle-e2e-{}-{}-out", getpid(), index)) {
        const lle::shm::RingConfig ingress_config{
            ingress_slots, static_cast<std::uint32_t>(payload)};
        const auto ingress_size = lle::shm::SpscRing::required_bytes(ingress_config);
        if (!ingress_size) {
            throw std::runtime_error("invalid ingress ring configuration");
        }
        auto segment = lle::shm::SharedMemorySegment::create_shm(ingress_name_, *ingress_size);
        if (!segment) {
            throw std::runtime_error("cannot create ingress shared memory");
        }
        ingress_segment_.emplace(std::move(*segment));
        auto producer = lle::shm::SpscRing::initialize(
            ingress_segment_->bytes(), ingress_config, random_instance_id());
        if (!producer) {
            throw std::runtime_error("cannot initialize ingress ring");
        }
        producer_.emplace(std::move(*producer));

        lle::ReceiverConfig receiver_config;
        receiver_config.listen = {"127.0.0.1", 0};
        receiver_config.egress_shm_name = egress_name_;
        receiver_config.egress_ring = {egress_slots, static_cast<std::uint32_t>(payload)};
        auto receiver = lle::Receiver::initialize(receiver_config);
        if (!receiver) {
            throw std::runtime_error("cannot initialize receiver");
        }
        receiver_.emplace(std::move(*receiver));

        lle::SenderConfig sender_config;
        sender_config.ingress_shm_name = ingress_name_;
        sender_config.receivers = {{"127.0.0.1", receiver_->local_port()}};
        auto sender = lle::Sender::initialize(sender_config);
        if (!sender) {
            throw std::runtime_error("cannot initialize sender");
        }
        sender_.emplace(std::move(*sender));

        auto egress = lle::shm::SharedMemorySegment::attach_shm(egress_name_);
        if (!egress) {
            throw std::runtime_error("cannot attach egress shared memory");
        }
        egress_segment_.emplace(std::move(*egress));
        auto consumer = lle::shm::SpscRing::attach(egress_segment_->bytes());
        if (!consumer) {
            throw std::runtime_error("cannot attach egress ring");
        }
        consumer_.emplace(std::move(*consumer));
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    ~Engine() {
        // handles go before the mappings they point into; the receiver unlinks egress.
        consumer_.reset();
        egress_segment_.reset();
        sender_.reset();
        receiver_.reset();
        producer_.reset();
        if (ingress_segment_) {
            static_cast<void>(ingress_segment_->unlink());
        }
    }

    lle::shm::SpscRing& producer() { return *producer_; }
    lle::shm::SpscRing& consumer() { return *consumer_; }
    lle::Sender& sender() { return *sender_; }
    lle::Receiver& receiver() { return *receiver_; }

  private:
    std::string ingress_name_;
    std::string egress_name_;
    std::optional<lle::shm::SharedMemorySegment> ingress_segment_;
    std::optional<lle::shm::SpscRing> producer_;
    std::optional<lle::Receiver> receiver_;
    std::optional<lle::Sender> sender_;
    std::optional<lle::shm::SharedMemorySegment> egress_segment_;
    std::optional<lle::shm::SpscRing> consumer_;
};

// Runs the sender and receiver loops on their own pinned threads. stop() ends both:
// closing ingress lets the sender drain and return; the receiver is asked to stop once
// the caller has seen everything it expects (or given up waiting).
class EngineThreads {
  public:
    EngineThreads(Engine& engine, const Cpus& cpus) {
        sender_ = std::jthread([this, &sender = engine.sender(), cpu = cpus.sender] {
            if (pin(cpu, sender_error_)) {
                sender.run();
            }
        });
        receiver_ = std::jthread([this, &receiver = engine.receiver(), cpu = cpus.receiver] {
            if (pin(cpu, receiver_error_)) {
                receiver.run();
            }
        });
        while (started_.value.load(std::memory_order_acquire) != 2) {
            _mm_pause();
        }
        if (sender_error_ || receiver_error_) {
            engine.receiver().request_stop();
            static_cast<void>(engine.producer().close_publication());
            sender_.join();
            receiver_.join();
            std::rethrow_exception(sender_error_ ? sender_error_ : receiver_error_);
        }
    }

    void stop_sender(Engine& engine) {
        static_cast<void>(engine.producer().close_publication());
        sender_.join();
    }

    void stop_receiver(Engine& engine) {
        engine.receiver().request_stop();
        receiver_.join();
    }

  private:
    bool pin(int cpu, std::exception_ptr& error) {
        try {
            util::pin_and_verify(cpu);
        } catch (...) {
            error = std::current_exception();
        }
        started_.value.fetch_add(1, std::memory_order_acq_rel);
        return !error;
    }

    Counter started_;
    std::exception_ptr sender_error_;
    std::exception_ptr receiver_error_;
    std::jthread sender_;
    std::jthread receiver_;
};

template <std::size_t N>
void fill_payload(std::array<std::byte, N>& payload, std::uint64_t sequence) {
    std::memcpy(payload.data(), &sequence, sizeof sequence);
}

template <std::size_t N>
LatencyResult run_latency(const Options& options, const Cpus& cpus, double tsc_hz, int index) {
    static_assert(N >= sizeof(std::uint64_t), "payload carries the sequence number");
    const std::uint64_t total = options.warmup + options.samples;
    Engine engine(N, options.egress_slots, index);
    // one slot per event; the consumer writes it, the producer reads it after the run.
    std::vector<std::int64_t> latency(total, lost_sample);
    const auto delivered = std::make_unique<Counter>();
    bool consumer_valid = true;
    std::exception_ptr consumer_error;
    const auto consumer_started = std::make_unique<Counter>();

    EngineThreads threads(engine, cpus);
    std::jthread consumer([&consumer_valid, &consumer_error, cpu = cpus.consumer, total,
                           ring = &engine.consumer(), out = latency.data(),
                           done = delivered.get(), started = consumer_started.get()] {
        try {
            util::pin_and_verify(cpu);
        } catch (...) {
            consumer_error = std::current_exception();
        }
        started->value.store(1, std::memory_order_release);
        if (consumer_error) {
            return;
        }
        std::array<std::byte, N> sink{};
        bool valid = true;
        for (;;) {
            auto event = ring->try_acquire();
            if (!event) {
                if (event.error() == lle::shm::AcquireError::Empty) {
                    continue;
                }
                valid = valid && event.error() == lle::shm::AcquireError::Closed;
                break;
            }
            const auto now = util::tsc_end();
            const auto sequence = event->sequence;
            std::memcpy(sink.data(), event->payload.data(), std::min(event->payload.size(), N));
            util::escape(sink.data());
            std::uint64_t payload_sequence = 0;
            std::memcpy(&payload_sequence, sink.data(), sizeof payload_sequence);
            valid = valid && sequence >= 1 && sequence <= total &&
                    payload_sequence == sequence && event->stream_id == stream_id &&
                    event->payload.size() == N;
            if (sequence >= 1 && sequence <= total) {
                out[sequence - 1] = static_cast<std::int64_t>(now) -
                                    static_cast<std::int64_t>(event->source_timestamp_ns);
            }
            static_cast<void>(ring->release());
            done->value.store(sequence, std::memory_order_release);
        }
        consumer_valid = valid;
    });
    while (consumer_started->value.load(std::memory_order_acquire) == 0) {
        _mm_pause();
    }
    if (consumer_error) {
        threads.stop_sender(engine);
        threads.stop_receiver(engine);
        consumer.join();
        std::rethrow_exception(consumer_error);
    }

    // main() already pinned this thread to the producer CPU.
    std::array<std::byte, N> payload{};
    payload.fill(std::byte{0x5a});
    const auto timeout_ticks =
        static_cast<std::uint64_t>(tsc_hz * static_cast<double>(options.timeout_ms) / 1000.0);
    for (std::uint64_t sequence = 1; sequence <= total; ++sequence) {
        fill_payload(payload, sequence);
        lle::EventView event{
            .stream_id = stream_id, .sequence = sequence, .source_timestamp_ns = 0,
            .payload = payload};
        event.source_timestamp_ns = util::tsc_begin();
        while (engine.producer().try_publish(event) != lle::PublishResult::Ok) {
        }
        // closed loop: wait for this event (or give up on it) before sending the next.
        const auto deadline = util::tsc_begin() + timeout_ticks;
        while (delivered->value.load(std::memory_order_acquire) < sequence &&
               util::tsc_begin() < deadline) {
        }
    }

    threads.stop_sender(engine);
    threads.stop_receiver(engine);
    consumer.join();
    if (!consumer_valid) {
        throw std::runtime_error(std::format("{}B: consumer saw an invalid event", N));
    }

    std::vector<std::int64_t> measured;
    measured.reserve(options.samples);
    LatencyResult result{.payload = N};
    for (std::uint64_t i = options.warmup; i < total; ++i) {
        if (latency[i] == lost_sample) {
            ++result.lost;
        } else {
            measured.push_back(latency[i]);
            if (latency[i] < 0) {
                ++result.negative;
            }
        }
    }
    result.sender = engine.sender().stats();
    result.receiver = engine.receiver().stats();
    result.samples = measured.size();
    if (measured.empty()) {
        return result;
    }
    std::ranges::sort(measured);
    const double ns_per_tick = 1e9 / tsc_hz;
    const auto to_ns = [&](std::int64_t ticks) { return static_cast<double>(ticks) * ns_per_tick; };
    const auto count = measured.size();
    const auto sum = std::accumulate(measured.begin(), measured.end(), 0.0L,
        [](long double acc, std::int64_t value) { return acc + static_cast<long double>(value); });
    result.min = to_ns(measured.front());
    result.p50 = to_ns(util::percentile(measured, count, 0.50));
    result.p90 = to_ns(util::percentile(measured, count, 0.90));
    result.p99 = to_ns(util::percentile(measured, count, 0.99));
    result.p999 = to_ns(util::percentile(measured, count, 0.999));
    result.p9999 = to_ns(util::percentile(measured, count, 0.9999));
    result.max = to_ns(measured.back());
    result.mean = static_cast<double>(sum / static_cast<long double>(count)) * ns_per_tick;
    return result;
}

template <std::size_t N>
ThroughputResult run_throughput(const Options& options, const Cpus& cpus, double tsc_hz, int index) {
    static_assert(N >= sizeof(std::uint64_t), "payload carries the sequence number");
    const std::uint64_t total = options.events;
    Engine engine(N, options.egress_slots, index);
    const auto received = std::make_unique<Counter>();
    std::uint64_t last_rx_tsc = 0;
    bool consumer_valid = true;
    std::exception_ptr consumer_error;
    const auto consumer_started = std::make_unique<Counter>();
    const auto rcvbuf_before = udp_rcvbuf_errors();

    EngineThreads threads(engine, cpus);
    std::jthread consumer([&consumer_valid, &consumer_error, &last_rx_tsc, cpu = cpus.consumer,
                           total, ring = &engine.consumer(), count = received.get(),
                           started = consumer_started.get()] {
        try {
            util::pin_and_verify(cpu);
        } catch (...) {
            consumer_error = std::current_exception();
        }
        started->value.store(1, std::memory_order_release);
        if (consumer_error) {
            return;
        }
        std::array<std::byte, N> sink{};
        bool valid = true;
        std::uint64_t previous = 0;
        std::uint64_t events = 0;
        std::uint64_t last = 0;
        for (;;) {
            auto event = ring->try_acquire();
            if (!event) {
                if (event.error() == lle::shm::AcquireError::Empty) {
                    continue;
                }
                valid = valid && event.error() == lle::shm::AcquireError::Closed;
                break;
            }
            last = util::tsc_end();
            std::memcpy(sink.data(), event->payload.data(), std::min(event->payload.size(), N));
            util::escape(sink.data());
            std::uint64_t payload_sequence = 0;
            std::memcpy(&payload_sequence, sink.data(), sizeof payload_sequence);
            // losses are allowed here; order and contents are not negotiable.
            valid = valid && event->sequence > previous && event->sequence <= total &&
                    payload_sequence == event->sequence;
            previous = event->sequence;
            static_cast<void>(ring->release());
            count->value.store(++events, std::memory_order_relaxed);
        }
        last_rx_tsc = last;
        consumer_valid = valid;
    });
    while (consumer_started->value.load(std::memory_order_acquire) == 0) {
        _mm_pause();
    }
    if (consumer_error) {
        threads.stop_sender(engine);
        threads.stop_receiver(engine);
        consumer.join();
        std::rethrow_exception(consumer_error);
    }

    // main() already pinned this thread to the producer CPU.
    std::array<std::byte, N> payload{};
    payload.fill(std::byte{0x5a});
    std::uint64_t ingress_full = 0;
    const auto start = util::tsc_begin();
    for (std::uint64_t sequence = 1; sequence <= total; ++sequence) {
        fill_payload(payload, sequence);
        const lle::EventView event{
            .stream_id = stream_id, .sequence = sequence, .source_timestamp_ns = sequence,
            .payload = payload};
        while (engine.producer().try_publish(event) != lle::PublishResult::Ok) {
            ++ingress_full;
        }
    }
    const auto published_end = util::tsc_end();
    threads.stop_sender(engine);

    // give datagrams still in the socket buffer time to arrive: stop once the consumer's
    // count has not moved for 50 ms.
    auto last_count = received->value.load(std::memory_order_relaxed);
    auto quiet_since = SteadyClock::now();
    while (SteadyClock::now() - quiet_since < std::chrono::milliseconds{50}) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        const auto count = received->value.load(std::memory_order_relaxed);
        if (count != last_count) {
            last_count = count;
            quiet_since = SteadyClock::now();
        }
    }
    threads.stop_receiver(engine);
    consumer.join();
    if (!consumer_valid) {
        throw std::runtime_error(std::format("{}B: consumer saw an invalid or reordered event", N));
    }

    ThroughputResult result{.payload = N, .published = total, .ingress_full = ingress_full};
    result.delivered = received->value.load(std::memory_order_relaxed);
    result.offered_mps = static_cast<double>(total) /
                         (static_cast<double>(published_end - start) / tsc_hz) / 1e6;
    if (result.delivered > 0 && last_rx_tsc > start) {
        result.delivered_mps = static_cast<double>(result.delivered) /
                               (static_cast<double>(last_rx_tsc - start) / tsc_hz) / 1e6;
    }
    result.kernel_rcvbuf_errors = udp_rcvbuf_errors() - rcvbuf_before;
    result.sender = engine.sender().stats();
    result.receiver = engine.receiver().stats();
    return result;
}

template <class Run>
auto dispatch(std::size_t size, Run run) {
    switch (size) {
    case 16:
        return run.template operator()<16>();
    case 64:
        return run.template operator()<64>();
    case 256:
        return run.template operator()<256>();
    case 1024:
        return run.template operator()<1024>();
    default:
        throw std::runtime_error(std::format("unsupported payload size {}", size));
    }
}

void print_usage() {
    std::cout
        << "usage: LLE_E2E_CPUS=producer,sender,receiver,consumer lle-e2e-benchmark [options]\n"
           "  --modes=latency,throughput\n"
           "  --samples=N            measured events per latency case (default 200000)\n"
           "  --warmup=N             unmeasured events per latency case (default 20000)\n"
           "  --events=N             events per throughput case (default 2000000)\n"
           "  --sizes=16,64,256,1024 payload sizes in bytes\n"
           "  --egress-slots=N       egress ring capacity, power of two (default 1024)\n"
           "  --timeout-ms=N         latency: give up on an event after N ms (default 100)\n"
           "  --latency-csv=PATH     --throughput-csv=PATH\n"
           "  --allow-smt-siblings   permit logical CPUs of one physical core\n"
           "LLE_E2E_CPUS defaults to 2,3,4,5.\n";
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        const auto eq = arg.find('=');
        const auto flag = arg.substr(0, eq);
        const auto value = eq == std::string_view::npos ? std::string_view{} : arg.substr(eq + 1);
        if (flag == "--samples") {
            options.samples = parse_u64(value, flag);
        } else if (flag == "--warmup") {
            options.warmup = parse_u64(value, flag);
        } else if (flag == "--events") {
            options.events = parse_u64(value, flag);
        } else if (flag == "--egress-slots") {
            options.egress_slots = static_cast<std::uint32_t>(parse_u64(value, flag));
        } else if (flag == "--timeout-ms") {
            options.timeout_ms = parse_u64(value, flag);
        } else if (flag == "--sizes") {
            options.sizes.clear();
            for (const auto& part : split(value)) {
                options.sizes.push_back(static_cast<std::size_t>(parse_u64(part, flag)));
            }
        } else if (flag == "--modes") {
            options.latency = false;
            options.throughput = false;
            for (const auto& mode : split(value)) {
                if (mode == "latency") {
                    options.latency = true;
                } else if (mode == "throughput") {
                    options.throughput = true;
                } else {
                    throw std::runtime_error(std::format("unknown mode '{}'", mode));
                }
            }
        } else if (flag == "--latency-csv") {
            options.latency_csv = std::string{value};
        } else if (flag == "--throughput-csv") {
            options.throughput_csv = std::string{value};
        } else if (flag == "--allow-smt-siblings") {
            options.allow_smt_siblings = true;
        } else if (flag == "--help" || flag == "-h") {
            print_usage();
            std::exit(0);
        } else {
            throw std::runtime_error(std::format("unknown option '{}'", arg));
        }
    }
    if (options.samples == 0 || options.events == 0 || options.sizes.empty() ||
        (!options.latency && !options.throughput)) {
        throw std::runtime_error("samples, events, sizes and modes must be nonempty");
    }
    return options;
}

void write_csv(const std::string& path, const std::string& content) {
    std::ofstream csv{path};
    csv << content;
    if (!csv) {
        throw std::runtime_error("cannot write " + path);
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        const auto cpus = read_cpus();
        const bool siblings = any_smt_siblings(cpus);
        if (siblings && !options.allow_smt_siblings) {
            throw std::runtime_error("two of the CPUs are SMT siblings of one physical core");
        }
        if (!util::has_invariant_tsc()) {
            throw std::runtime_error("constant_tsc/nonstop_tsc missing; rdtsc is not usable");
        }

        util::pin_and_verify(cpus.producer);
        const auto calibration = util::calibrate_tsc();
        const double ns_per_tick = 1e9 / calibration.hz;
        const auto overhead = util::measure_overhead([&] {
            const auto a = util::tsc_begin();
            const auto b = util::tsc_end();
            return static_cast<double>(b - a) * ns_per_tick;
        });
        const auto offset = measure_tsc_offset(cpus.producer, cpus.consumer, calibration.hz);
        util::pin_and_verify(cpus.producer);

        std::cout
            << "## End-to-end: producer -> SHM -> Sender -> UDP loopback -> Receiver -> SHM -> consumer\n\n"
            << std::format("- cpus: producer {}, sender {}, receiver {}, consumer {}{}\n",
                           cpus.producer, cpus.sender, cpus.receiver, cpus.consumer,
                           siblings ? " **SMT siblings**" : "")
            << std::format("- compiler: {}; build: {}\n", __VERSION__, LLE_BENCH_COMPILE_INFO)
            << std::format("- TSC: {:.3f} MHz (calibration windows {:.3f}..{:.3f} MHz)\n",
                           calibration.hz / 1e6, calibration.min_hz / 1e6, calibration.max_hz / 1e6)
            << std::format("- timer overhead (fenced rdtsc pair): min {:.1f} ns, median {:.1f} ns\n",
                           overhead.min_ns, overhead.median_ns)
            << std::format("- TSC offset consumer cpu - producer cpu: {:+.1f} ns (bound +-{:.1f} ns, "
                           "fastest of 100000 exchanges)\n", offset.offset_ns, offset.bound_ns)
            << std::format("- receiver cpu idle states: {}\n", idle_states(cpus.receiver))
            << std::format("- latency: closed loop, {} samples after {} warm-up per size; one-way, "
                           "includes one timer pair and the TSC offset above\n",
                           options.samples, options.warmup)
            << std::format("- throughput: {} events per size; ingress {} slots, egress {} slots\n\n",
                           options.events, ingress_slots, options.egress_slots)
            << std::flush;

        int index = 0;
        if (options.latency) {
            std::vector<LatencyResult> results;
            for (const auto size : options.sizes) {
                std::cerr << std::format("latency {}B\n", size);
                results.push_back(dispatch(size, [&]<std::size_t N>() {
                    return run_latency<N>(options, cpus, calibration.hz, index++);
                }));
            }
            std::cout << "### One-way latency, ns\n\n"
                         "| payload B | samples | lost | min | p50 | p90 | p99 | p99.9 | p99.99 | "
                         "max | mean |\n"
                         "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
            std::string csv = "payload_bytes,samples,lost,negative,min_ns,p50_ns,p90_ns,p99_ns,"
                              "p999_ns,p9999_ns,max_ns,mean_ns,packets_missed,malformed,"
                              "egress_full\n";
            for (const auto& r : results) {
                std::cout << std::format(
                    "| {} | {} | {} | {:.0f} | {:.0f} | {:.0f} | {:.0f} | {:.0f} | {:.0f} | "
                    "{:.0f} | {:.1f} |\n",
                    r.payload, r.samples, r.lost, r.min, r.p50, r.p90, r.p99, r.p999, r.p9999,
                    r.max, r.mean);
                csv += std::format("{},{},{},{},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},"
                                   "{:.2f},{},{},{}\n",
                                   r.payload, r.samples, r.lost, r.negative, r.min, r.p50, r.p90,
                                   r.p99, r.p999, r.p9999, r.max, r.mean,
                                   r.receiver.packets_missed, r.receiver.malformed_packets,
                                   r.receiver.egress_full);
            }
            std::cout << "\nEngine counters (latency runs):\n\n"
                         "| payload B | sender events_out | send_errors | receiver events_out | "
                         "packets_missed | malformed | egress_full | negative samples |\n"
                         "|---:|---:|---:|---:|---:|---:|---:|---:|\n";
            for (const auto& r : results) {
                std::cout << std::format("| {} | {} | {} | {} | {} | {} | {} | {} |\n", r.payload,
                                         r.sender.events_out, r.sender.send_errors,
                                         r.receiver.events_out, r.receiver.packets_missed,
                                         r.receiver.malformed_packets, r.receiver.egress_full,
                                         r.negative);
            }
            std::cout << '\n';
            if (!options.latency_csv.empty()) {
                write_csv(options.latency_csv, csv);
            }
        }

        if (options.throughput) {
            std::vector<ThroughputResult> results;
            for (const auto size : options.sizes) {
                std::cerr << std::format("throughput {}B\n", size);
                results.push_back(dispatch(size, [&]<std::size_t N>() {
                    return run_throughput<N>(options, cpus, calibration.hz, index++);
                }));
            }
            std::cout << "### Throughput\n\n"
                         "| payload B | published | delivered | offered M/s | delivered M/s | "
                         "lost in network (gap) | dropped at egress | kernel rcvbuf drops |\n"
                         "|---:|---:|---:|---:|---:|---:|---:|---:|\n";
            std::string csv = "payload_bytes,published,delivered,offered_mps,delivered_mps,"
                              "ingress_full_spins,sender_events_out,send_errors,packets_received,"
                              "packets_missed,egress_full,kernel_rcvbuf_errors\n";
            for (const auto& r : results) {
                std::cout << std::format("| {} | {} | {} | {:.3f} | {:.3f} | {} | {} | {} |\n",
                                         r.payload, r.published, r.delivered, r.offered_mps,
                                         r.delivered_mps, r.receiver.packets_missed,
                                         r.receiver.egress_full, r.kernel_rcvbuf_errors);
                csv += std::format("{},{},{},{:.4f},{:.4f},{},{},{},{},{},{},{}\n", r.payload,
                                   r.published, r.delivered, r.offered_mps, r.delivered_mps,
                                   r.ingress_full, r.sender.events_out, r.sender.send_errors,
                                   r.receiver.packets_received, r.receiver.packets_missed,
                                   r.receiver.egress_full, r.kernel_rcvbuf_errors);
            }
            std::cout << "\nkernel rcvbuf drops are system-wide UDP RcvbufErrors from /proc/net/snmp "
                         "during the case.\n";
            if (!options.throughput_csv.empty()) {
                write_csv(options.throughput_csv, csv);
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
