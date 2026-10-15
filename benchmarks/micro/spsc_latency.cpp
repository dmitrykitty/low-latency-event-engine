// Ping-pong latency benchmark for LLE SpscRing, rigtorp::SPSCQueue and
// boost::lockfree::spsc_queue.
//
// The initiator thread (LLE_PRODUCER_CPU) publishes a request and spins until
// the echo thread (LLE_CONSUMER_CPU) copies its payload into a response and
// publishes it back. One request is outstanding at a time. Each sample is one
// round trip, timed with fenced rdtsc on the initiator core only, so no
// cross-core TSC comparison is involved. TSC ticks are converted to
// nanoseconds with a frequency calibrated against steady_clock at startup.

#include "queue_adapters.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>
#include <x86intrin.h>

#ifndef LLE_BENCH_COMPILE_INFO
#define LLE_BENCH_COMPILE_INFO "unknown"
#endif

namespace {

using lle_bench::BoostQueue;
using lle_bench::LLEQueue;
using lle_bench::Record;
using lle_bench::RigtorpQueue;
namespace util = lle_bench::util;
using SteadyClock = std::chrono::steady_clock;

constexpr std::size_t queue_capacity = 1024;

struct Options {
    std::uint64_t samples{10'000'000};
    std::uint64_t warmup{1'000'000};
    int reps{1};
    std::vector<std::size_t> sizes{16, 64, 256, 1024};
    std::vector<std::string> queues{"lle", "rigtorp", "boost"};
    bool pause{false};
    bool allow_smt_siblings{false};
    std::string csv_path;
};

struct Topology {
    int package{-1};
    int core{-1};
};

struct Result {
    std::string queue;
    std::size_t payload{};
    int rep{};
    std::uint64_t samples{};
    double min{}, p50{}, p90{}, p99{}, p999{}, p9999{}, max{}, mean{};
};

// lfence on both sides stops rdtsc from being reordered with the timed work.
inline std::uint64_t tsc_begin() noexcept {
    _mm_lfence();
    const auto ticks = __rdtsc();
    _mm_lfence();
    return ticks;
}

// rdtscp waits for earlier instructions; the trailing lfence holds back later ones.
inline std::uint64_t tsc_end() noexcept {
    unsigned aux = 0;
    const auto ticks = __rdtscp(&aux);
    _mm_lfence();
    return ticks;
}

// keeps copies into otherwise unread buffers from being optimized away.
inline void escape(const void* pointer) noexcept {
    asm volatile("" : : "g"(pointer) : "memory");
}

inline void spin(bool pause) noexcept {
    if (pause) {
        _mm_pause();
    }
}

bool has_invariant_tsc() {
    std::ifstream cpuinfo{"/proc/cpuinfo"};
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.starts_with("flags")) {
            return line.find(" constant_tsc") != std::string::npos &&
                   line.find(" nonstop_tsc") != std::string::npos;
        }
    }
    return false;
}

int read_topology_value(int cpu, std::string_view name) {
    std::ifstream file{std::format("/sys/devices/system/cpu/cpu{}/topology/{}", cpu, name)};
    int value = -1;
    if (!(file >> value)) {
        throw std::runtime_error(std::format("cannot read topology {} for cpu {}", name, cpu));
    }
    return value;
}

Topology read_topology(int cpu) {
    return {
        read_topology_value(cpu, "physical_package_id"),
        read_topology_value(cpu, "core_id")
    };
}

void pin_and_verify(int cpu) {
    util::pin_to_cpu(cpu);
    if (sched_getcpu() != cpu) {
        throw std::runtime_error(std::format("thread is not running on cpu {}", cpu));
    }
}

// median of several busy-waited windows; min/max show how stable the estimate is.
struct TscCalibration {
    double hz{};
    double min_hz{};
    double max_hz{};
};

TscCalibration calibrate_tsc() {
    constexpr int windows = 7;
    constexpr auto window = std::chrono::milliseconds{100};
    std::vector<double> estimates;
    for (int i = 0; i < windows; ++i) {
        const auto wall_begin = SteadyClock::now();
        const auto tsc0 = tsc_begin();
        while (SteadyClock::now() - wall_begin < window) {
        }
        const auto tsc1 = tsc_end();
        const auto wall_end = SteadyClock::now();
        const std::chrono::duration<double> elapsed = wall_end - wall_begin;
        estimates.push_back(static_cast<double>(tsc1 - tsc0) / elapsed.count());
    }
    std::ranges::sort(estimates);
    return {estimates[estimates.size() / 2], estimates.front(), estimates.back()};
}

struct ClockOverhead {
    double min_ns{};
    double median_ns{};
};

// cost of one back-to-back timestamp pair, i.e. what each sample includes.
template <class Read> ClockOverhead measure_overhead(Read read_pair) {
    constexpr std::size_t count = 1'000'000;
    std::vector<double> values(count);
    for (auto& value : values) {
        value = read_pair();
    }
    std::ranges::sort(values);
    return {values.front(), values[count / 2]};
}

// nearest-rank percentile on sorted data.
std::uint64_t
percentile(const std::vector<std::uint64_t>& sorted, std::uint64_t count, double p) {
    const auto rank = static_cast<std::uint64_t>(std::ceil(p * static_cast<double>(count)));
    return sorted[std::clamp<std::uint64_t>(rank, 1, count) - 1];
}

template <template <std::size_t> class Queue, std::size_t N>
Result run_pingpong(
    const Options& options,
    int echo_cpu,
    double tsc_hz,
    std::vector<std::uint64_t>& ticks,
    std::string_view name
) {
    static_assert(N >= sizeof(std::uint64_t), "payload carries the sequence number");
    const std::uint64_t total = options.warmup + options.samples;
    const bool pause = options.pause;

    Queue<N> requests{queue_capacity};
    Queue<N> responses{queue_capacity};

    std::atomic<bool> ready{false};
    std::atomic<bool> start{false};
    std::exception_ptr echo_error;

    std::jthread echo([&] {
        try {
            pin_and_verify(echo_cpu);
        } catch (...) {
            echo_error = std::current_exception();
            ready.store(true, std::memory_order_release);
            return;
        }
        Record<N> response{};
        response.stream = 9;
        response.length = static_cast<std::uint32_t>(N);
        ready.store(true, std::memory_order_release);

        while (!start.load(std::memory_order_acquire)) {
            _mm_pause();
        }
        for (std::uint64_t i = 0; i < total; ++i) {
            // echo the whole payload so it really crosses cores in both directions.
            while (!requests.try_consume([&](const lle::EventView& event) {
                response.sequence = event.sequence;
                response.timestamp = event.source_timestamp_ns;
                std::memcpy(
                    response.payload.data(),
                    event.payload.data(),
                    std::min(event.payload.size(), N)
                );
            })) {
                spin(pause);
            }
            while (!responses.try_push(response)) {
                spin(pause);
            }
        }
    });

    while (!ready.load(std::memory_order_acquire)) {
        _mm_pause();
    }
    if (echo_error) {
        echo.join();
        std::rethrow_exception(echo_error);
    }

    Record<N> request{};
    request.stream = 9;
    request.length = static_cast<std::uint32_t>(N);
    request.payload.fill(std::byte{0x5a});
    std::array<std::byte, N> sink{};
    bool valid = true;

    start.store(true, std::memory_order_release);
    for (std::uint64_t i = 0; i < total; ++i) {
        const std::uint64_t sequence = i + 1;
        request.sequence = sequence;
        request.timestamp = sequence;
        std::memcpy(request.payload.data(), &sequence, sizeof sequence);
        std::uint64_t echoed_sequence = 0;

        const auto begin = tsc_begin();
        while (!requests.try_push(request)) {
            spin(pause);
        }
        while (!responses.try_consume([&](const lle::EventView& event) {
            echoed_sequence = event.sequence;
            std::memcpy(sink.data(), event.payload.data(), std::min(event.payload.size(), N));
        })) {
            spin(pause);
        }
        escape(sink.data());
        const auto end = tsc_end();

        std::uint64_t payload_sequence = 0;
        std::memcpy(&payload_sequence, sink.data(), sizeof payload_sequence);
        valid = valid && echoed_sequence == sequence && payload_sequence == sequence;
        if (i >= options.warmup) {
            ticks[i - options.warmup] = end - begin;
        }
    }
    echo.join();

    if (!valid) {
        throw std::runtime_error(std::format("{} {}B: echoed sequence mismatch", name, N));
    }

    const auto count = options.samples;
    std::sort(ticks.begin(), ticks.begin() + static_cast<std::ptrdiff_t>(count));
    const double ns_per_tick = 1e9 / tsc_hz;
    const auto to_ns = [&](std::uint64_t value) {
        return static_cast<double>(value) * ns_per_tick;
    };
    const auto sum = std::accumulate(
        ticks.begin(),
        ticks.begin() + static_cast<std::ptrdiff_t>(count),
        0.0L,
        [](long double acc, std::uint64_t value) {
            return acc + static_cast<long double>(value);
        }
    );

    return Result{
        .queue = std::string{name},
        .payload = N,
        .samples = count,
        .min = to_ns(ticks[0]),
        .p50 = to_ns(percentile(ticks, count, 0.50)),
        .p90 = to_ns(percentile(ticks, count, 0.90)),
        .p99 = to_ns(percentile(ticks, count, 0.99)),
        .p999 = to_ns(percentile(ticks, count, 0.999)),
        .p9999 = to_ns(percentile(ticks, count, 0.9999)),
        .max = to_ns(ticks[count - 1]),
        .mean = static_cast<double>(sum / static_cast<long double>(count)) * ns_per_tick,
    };
}

template <std::size_t N>
Result run_queue(
    std::string_view queue,
    const Options& options,
    int echo_cpu,
    double tsc_hz,
    std::vector<std::uint64_t>& ticks
) {
    if (queue == "lle") {
        return run_pingpong<LLEQueue, N>(options, echo_cpu, tsc_hz, ticks, queue);
    }
    if (queue == "rigtorp") {
        return run_pingpong<RigtorpQueue, N>(options, echo_cpu, tsc_hz, ticks, queue);
    }
    if (queue == "boost") {
        return run_pingpong<BoostQueue, N>(options, echo_cpu, tsc_hz, ticks, queue);
    }
    throw std::runtime_error(std::format("unknown queue '{}'", queue));
}

Result run_case(
    std::string_view queue,
    std::size_t size,
    const Options& options,
    int echo_cpu,
    double tsc_hz,
    std::vector<std::uint64_t>& ticks,
    int rep
) {
    auto run = [&]<std::size_t N>() {
        return run_queue<N>(queue, options, echo_cpu, tsc_hz, ticks);
    };
    Result result;
    switch (size) {
    case 16:
        result = run.template operator()<16>();
        break;
    case 64:
        result = run.template operator()<64>();
        break;
    case 256:
        result = run.template operator()<256>();
        break;
    case 1024:
        result = run.template operator()<1024>();
        break;
    default:
        throw std::runtime_error(std::format("unsupported payload size {}", size));
    }
    result.rep = rep;
    return result;
}

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

void print_usage() {
    std::cout << "usage: LLE_PRODUCER_CPU=a LLE_CONSUMER_CPU=b lle-spsc-latency [options]\n"
                 "  --samples=N            measured round trips per case (default 10000000)\n"
                 "  --warmup=N             unmeasured round trips per case (default 1000000)\n"
                 "  --reps=N               repetitions of every case (default 1)\n"
                 "  --sizes=16,64,256,1024 payload sizes in bytes\n"
                 "  --queues=lle,rigtorp,boost\n"
                 "  --spin=busy|pause      wait loop; pause issues _mm_pause (default busy)\n"
                 "  --csv=PATH             also write results as CSV\n"
                 "  --allow-smt-siblings   permit two logical CPUs of one physical core\n";
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        const auto eq = arg.find('=');
        const auto flag = arg.substr(0, eq);
        const auto value =
            eq == std::string_view::npos ? std::string_view{} : arg.substr(eq + 1);
        if (flag == "--samples") {
            options.samples = parse_u64(value, flag);
        } else if (flag == "--warmup") {
            options.warmup = parse_u64(value, flag);
        } else if (flag == "--reps") {
            options.reps = static_cast<int>(parse_u64(value, flag));
        } else if (flag == "--sizes") {
            options.sizes.clear();
            for (const auto& part : split(value)) {
                options.sizes.push_back(static_cast<std::size_t>(parse_u64(part, flag)));
            }
        } else if (flag == "--queues") {
            options.queues = split(value);
        } else if (flag == "--spin") {
            if (value != "busy" && value != "pause") {
                throw std::runtime_error("--spin must be busy or pause");
            }
            options.pause = value == "pause";
        } else if (flag == "--csv") {
            options.csv_path = std::string{value};
        } else if (flag == "--allow-smt-siblings") {
            options.allow_smt_siblings = true;
        } else if (flag == "--help" || flag == "-h") {
            print_usage();
            std::exit(0);
        } else {
            throw std::runtime_error(std::format("unknown option '{}'", arg));
        }
    }
    if (options.samples == 0 || options.reps <= 0 || options.sizes.empty() ||
        options.queues.empty()) {
        throw std::runtime_error("samples, reps, sizes and queues must be nonempty");
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        const int initiator_cpu = util::read_cpu("LLE_PRODUCER_CPU");
        const int echo_cpu = util::read_cpu("LLE_CONSUMER_CPU");
        if (initiator_cpu == echo_cpu) {
            throw std::runtime_error("producer and consumer must use different CPUs");
        }
        const auto initiator_topology = read_topology(initiator_cpu);
        const auto echo_topology = read_topology(echo_cpu);
        const bool same_core = initiator_topology.package == echo_topology.package &&
                               initiator_topology.core == echo_topology.core;
        if (same_core && !options.allow_smt_siblings) {
            throw std::runtime_error("CPUs are SMT siblings of one physical core");
        }
        if (!has_invariant_tsc()) {
            throw std::runtime_error("constant_tsc/nonstop_tsc missing; rdtsc is not usable");
        }

        pin_and_verify(initiator_cpu);
        const auto calibration = calibrate_tsc();
        const double ns_per_tick = 1e9 / calibration.hz;
        const auto tsc_overhead = measure_overhead([&] {
            const auto a = tsc_begin();
            const auto b = tsc_end();
            return static_cast<double>(b - a) * ns_per_tick;
        });
        const auto steady_overhead = measure_overhead([] {
            const auto a = SteadyClock::now();
            const auto b = SteadyClock::now();
            return std::chrono::duration<double, std::nano>(b - a).count();
        });

        std::cout
            << "## SPSC ping-pong latency (round trip)\n\n"
            << std::format(
                   "- initiator cpu {} (package {}, core {}), echo cpu {} (package {}, core "
                   "{}){}\n",
                   initiator_cpu,
                   initiator_topology.package,
                   initiator_topology.core,
                   echo_cpu,
                   echo_topology.package,
                   echo_topology.core,
                   same_core ? " **SMT siblings**" : ""
               )
            << std::format("- compiler: {}; build: {}\n", __VERSION__, LLE_BENCH_COMPILE_INFO)
            << std::format(
                   "- TSC: {:.3f} MHz (calibration windows {:.3f}..{:.3f} MHz)\n",
                   calibration.hz / 1e6,
                   calibration.min_hz / 1e6,
                   calibration.max_hz / 1e6
               )
            << std::format(
                   "- timer overhead per sample (fenced rdtsc pair): min {:.1f} ns, median "
                   "{:.1f} ns\n",
                   tsc_overhead.min_ns,
                   tsc_overhead.median_ns
               )
            << std::format(
                   "- for reference, steady_clock::now() pair: min {:.1f} ns, median {:.1f} "
                   "ns\n",
                   steady_overhead.min_ns,
                   steady_overhead.median_ns
               )
            << std::format(
                   "- samples {} per case after {} warm-up round trips; reps {}; capacity {}; "
                   "spin {}\n",
                   options.samples,
                   options.warmup,
                   options.reps,
                   queue_capacity,
                   options.pause ? "pause" : "busy"
               )
            << "- values are round-trip ns and include the timer overhead above; not halved\n\n"
            << std::flush;

        // pre-faulted once and reused so no page faults land in timed loops.
        std::vector<std::uint64_t> ticks(options.samples);
        std::vector<Result> results;
        for (int rep = 0; rep < options.reps; ++rep) {
            for (std::size_t s = 0; s < options.sizes.size(); ++s) {
                // rotate queue order so no queue always runs first.
                const auto offset = (static_cast<std::size_t>(rep) + s) % options.queues.size();
                for (std::size_t q = 0; q < options.queues.size(); ++q) {
                    const auto& queue = options.queues[(q + offset) % options.queues.size()];
                    std::cerr
                        << std::format("running {} {}B rep {}\n", queue, options.sizes[s], rep);
                    results.push_back(run_case(
                        queue,
                        options.sizes[s],
                        options,
                        echo_cpu,
                        calibration.hz,
                        ticks,
                        rep
                    ));
                }
            }
        }

        std::ranges::sort(results, {}, [&](const Result& r) {
            const auto qi = std::ranges::find(options.queues, r.queue) - options.queues.begin();
            return std::tuple{r.payload, qi, r.rep};
        });

        std::cout << "| queue | payload B | rep | samples | min | p50 | p90 | p99 | p99.9 | "
                     "p99.99 | max | mean |\n"
                  << "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
        for (const auto& r : results) {
            std::cout << std::format(
                "| {} | {} | {} | {} | {:.0f} | {:.0f} | {:.0f} | {:.0f} | {:.0f} | {:.0f} | "
                "{:.0f} | {:.1f} |\n",
                r.queue,
                r.payload,
                r.rep,
                r.samples,
                r.min,
                r.p50,
                r.p90,
                r.p99,
                r.p999,
                r.p9999,
                r.max,
                r.mean
            );
        }
        std::cout << "\nAll latency columns are nanoseconds.\n";

        if (!options.csv_path.empty()) {
            std::ofstream csv{options.csv_path};
            csv << "queue,payload_bytes,rep,samples,min_ns,p50_ns,p90_ns,p99_ns,p999_ns,p9999_"
                   "ns,max_ns,mean_ns\n";
            for (const auto& r : results) {
                csv << std::format(
                    "{},{},{},{},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f}\n",
                    r.queue,
                    r.payload,
                    r.rep,
                    r.samples,
                    r.min,
                    r.p50,
                    r.p90,
                    r.p99,
                    r.p999,
                    r.p9999,
                    r.max,
                    r.mean
                );
            }
            if (!csv) {
                throw std::runtime_error("cannot write " + options.csv_path);
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
