#pragma once

// CPU pinning, topology checks and fenced-TSC timing shared by the benchmark programs.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <x86intrin.h>

namespace lle_bench::util {

inline void pin_to_cpu(int cpu) {
    if (cpu < 0 || cpu >= CPU_SETSIZE) {
        throw std::runtime_error("cpu id outside supported range");
    }
    cpu_set_t cpuset;
    //reset all cpus
    CPU_ZERO(&cpuset);
    CPU_SET(static_cast<std::size_t>(cpu), &cpuset);

    //             current_thread pid
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
        throw std::runtime_error("cannot pin to cpu " + std::to_string(cpu));
    }
}

inline int parse_cpu(std::string_view text, std::string_view what) {
    int cpu = -1;
    const auto* end = text.data() + text.size();
    const auto [ptr, error] = std::from_chars(text.data(), end, cpu);
    if (error != std::errc{} || ptr != end || cpu < 0 || cpu >= CPU_SETSIZE) {
        throw std::runtime_error(std::string{what} + " has invalid CPU id");
    }
    return cpu;
}

inline int read_cpu(const char* name) {
    const char* value = std::getenv(name);

    if (value == nullptr) {
        throw std::runtime_error(std::string{name} + " is not set");
    }
    return parse_cpu(value, name);
}

inline void pin_and_verify(int cpu) {
    pin_to_cpu(cpu);
    if (sched_getcpu() != cpu) {
        throw std::runtime_error(std::format("thread is not running on cpu {}", cpu));
    }
}

struct Topology {
    int package{-1};
    int core{-1};
};

inline int read_topology_value(int cpu, std::string_view name) {
    std::ifstream file{std::format("/sys/devices/system/cpu/cpu{}/topology/{}", cpu, name)};
    int value = -1;
    if (!(file >> value)) {
        throw std::runtime_error(std::format("cannot read topology {} for cpu {}", name, cpu));
    }
    return value;
}

inline Topology read_topology(int cpu) {
    return {
        read_topology_value(cpu, "physical_package_id"),
        read_topology_value(cpu, "core_id")
    };
}

inline bool same_physical_core(int a, int b) {
    const auto ta = read_topology(a);
    const auto tb = read_topology(b);
    return ta.package == tb.package && ta.core == tb.core;
}

inline bool has_invariant_tsc() {
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

// median of several busy-waited windows; min/max show how stable the estimate is.
struct TscCalibration {
    double hz{};
    double min_hz{};
    double max_hz{};
};

inline TscCalibration calibrate_tsc() {
    using SteadyClock = std::chrono::steady_clock;
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
template <class T>
T percentile(const std::vector<T>& sorted, std::uint64_t count, double p) {
    const auto rank = static_cast<std::uint64_t>(std::ceil(p * static_cast<double>(count)));
    return sorted[std::clamp<std::uint64_t>(rank, 1, count) - 1];
}

} // namespace lle_bench::util
