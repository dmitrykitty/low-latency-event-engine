#include "shm/spsc_ring.hpp"
#include <benchmark/benchmark.h>
#include <algorithm>
#include <boost/lockfree/spsc_queue.hpp>
#include <rigtorp/SPSCQueue.h>

#include <array>
#include <atomic>
#include <cmath>
#include <immintrin.h>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <sched.h>
#include <charconv>
#include <iostream>
#include <string_view>

namespace lle {
struct EventView;
}

namespace benchmark {
class State;
}

namespace {

int producer_cpu = -1;
int consumer_cpu = -1;

void pin_to_cpu(int cpu) {
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

// data to be sent
template <std::size_t N>
struct Record {
    std::uint64_t sequence;
    std::uint64_t timestamp;
    std::uint32_t stream;
    std::uint32_t length;
    std::array<std::byte, N> payload;
};

template <std::size_t N>
class LLEQueue {
    struct Deleter {
        void operator()(std::byte* ptr) const {
            operator delete(ptr, std::align_val_t{64});
        }
    };

public:
    explicit LLEQueue(std::size_t capacity) {
        lle::shm::RingConfig config{
            .slot_count = static_cast<std::uint32_t>(capacity),
            .slot_payload_capacity = static_cast<std::uint32_t>(N)
        };

        const auto size = lle::shm::SpscRing::required_bytes(config);
        if (!size) {
            throw std::runtime_error("invalid configuration");
        }
        storage_.reset(static_cast<std::byte*>(operator new(*size, std::align_val_t{64})));
        auto producer = lle::shm::SpscRing::initialize({storage_.get(), *size}, config, 42);
        if (!producer) {
            throw std::runtime_error("cannot initialize ring");
        }
        producer_.emplace(std::move(*producer));

        auto consumer = lle::shm::SpscRing::attach({storage_.get(), *size});
        if (!consumer) {
            throw std::runtime_error("cannot attach ring");
        }
        consumer_.emplace(std::move(*consumer));
    }

    bool try_push(const Record<N>& r) {
        const lle::EventView event{
            .stream_id = r.stream,
            .sequence = r.sequence,
            .source_timestamp_ns = r.timestamp,
            .payload = r.payload
        };
        return producer_->try_publish(event) == lle::PublishResult::Ok;
    }

    template <class F>
    bool try_consume(F&& f) {
        auto result = consumer_->try_acquire();
        if (!result) {
            return false;
        }
        f(*result);
        return consumer_->release();
    }

private:
    std::unique_ptr<std::byte, Deleter> storage_;
    std::optional<lle::shm::SpscRing> producer_;
    std::optional<lle::shm::SpscRing> consumer_;
};

template <std::size_t N>
class RigtorpQueue {
public:
    explicit RigtorpQueue(std::size_t capacity)
        : queue_(capacity) {}

    bool try_push(const Record<N>& r) {
        return queue_.try_push(r);
    }

    template <class F> bool try_consume(F&& f) {
        Record<N>* r = queue_.front();
        if (r == nullptr) {
            return false;
        }
        lle::EventView event{
            .stream_id = r->stream,
            .sequence = r->sequence,
            .source_timestamp_ns = r->timestamp,
            .payload = r->payload
        };

        f(event);
        queue_.pop();
        return true;
    }

private:
    rigtorp::SPSCQueue<Record<N>> queue_;
};

template <std::size_t N>
class BoostQueue {
public:
    explicit BoostQueue(std::size_t capacity)
        : queue_(capacity) {}
    bool try_push(const Record<N>& r) {
        return queue_.push(r);
    }
    template <class F>
    bool try_consume(F&& f) {
        return queue_.consume_one([&](const Record<N>& r) {
            lle::EventView event{
                .stream_id = r.stream,
                .sequence = r.sequence,
                .source_timestamp_ns = r.timestamp,
                .payload = r.payload
            };
            f(event);
        });
    }

private:
    boost::lockfree::spsc_queue<Record<N>> queue_;
};

template <template <std::size_t> class Queue, std::size_t N>
void run_throughput(benchmark::State& state) {
    constexpr std::uint64_t warmup_count = 10'000;
    constexpr std::uint64_t event_count = 1'000'000;

    //slots amount
    const auto capacity = static_cast<std::size_t>(state.range(0));
    Queue<N> queue{capacity};
    Record<N> record{};

    record.stream = 9;
    record.length = static_cast<std::uint32_t>(N);
    record.payload.fill(std::byte{0x5a});

    //start benchmark
    for (auto _ : state) {
        state.PauseTiming();

        bool correct = true;
        std::atomic consumer_ready{false};
        std::atomic start{false};
        std::atomic warmup_done{false};
        bool pin_ok = true;

        //not like in java. new tread start to work from constructor time
        //new os tread created and lambda is performed
        std::jthread consumer([&] {
            try { pin_to_cpu(consumer_cpu); }
            catch (const std::exception&) {
                pin_ok = false;
                consumer_ready.store(true, std::memory_order_release);
                return;
            }

            //kind of handshake: consumer -> ready to consume, wait for producer
            consumer_ready.store(true, std::memory_order_release);

            //wait for producer: producer -> start
            while (!start.load(std::memory_order_acquire)) {
                _mm_pause();
            }

            for (std::uint64_t expected = 0; expected < warmup_count + event_count; ++expected) {
                //if empty - try_consume -> false , wait for event in ring
                //spin waiting for producer put event into buffer
                while (!queue.try_consume(
                        [&](const lle::EventView& event) {
                            if (event.sequence != expected) {
                                correct = false;
                            }
                        }
                    ))
                {
                    _mm_pause();
                }
                if (expected + 1 == warmup_count) {
                    warmup_done.store(true, std::memory_order_release);
                }
            }
        });

        //waiting for consumer -> ready to consume
        while (!consumer_ready.load(std::memory_order_acquire)) {
            _mm_pause();
        }

        if (!pin_ok) {
            consumer.join();
            state.SkipWithError("cannot pin consumer to requested cpu");
            break;
        }
        start.store(true, std::memory_order_release);

        // warm up the same queue and threads while timing is paused.
        for (std::uint64_t i = 0; i < warmup_count; ++i) {
            record.sequence = i;
            record.timestamp = i * 3;
            while (!queue.try_push(record)) {
                _mm_pause();
            }
        }
        // wait for all warm-up events to be consumed and released.
        while (!warmup_done.load(std::memory_order_acquire)) {
            _mm_pause();
        }

        state.ResumeTiming();
        for (std::uint64_t i = 0; i < event_count; ++i) {
            record.sequence = warmup_count + i;
            record.timestamp = record.sequence * 3;

            //spin waiting until consumer free buffer
            while (!queue.try_push(record)) {
                _mm_pause();
            }
        }

        //waiting for consumer
        consumer.join();
        state.PauseTiming();

        if (!correct) {
            state.SkipWithError("consumer received invalid sequence");
            break;
        }

        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(event_count));
}
// rtt includes both queues, spinning, response preparation, and sequence checks.
template <template<std::size_t> class Queue, std::size_t N>
void run_rtt(benchmark::State& state) {
    constexpr std::size_t capacity = 1024;
    constexpr std::uint64_t warmup_count = 10'000;
    constexpr std::uint64_t sample_count = 100'000;
    constexpr  std::uint64_t total = warmup_count + sample_count;

    Queue<N> requests{capacity};
    Queue<N> responses{capacity};

    Record<N> request{};
    request.stream = 9;
    request.length = static_cast<std::uint32_t>(N);
    request.payload.fill(std::byte{0x5a});

    Record<N> response{};
    response.stream = 9;
    response.length = static_cast<std::uint32_t>(N);
    response.payload.fill(std::byte{0x5a});

    std::vector<std::uint64_t> samples(sample_count);

    for (auto _ : state) {
        //we don't want to calculate consumer create time
        state.PauseTiming();

        std::atomic correct{true};
        std::atomic consumer_ready{false};
        std::atomic start{false};
        bool pin_ok = true;

        std::jthread consumer([&] {
            try { pin_to_cpu(consumer_cpu); }
            catch (const std::exception&) {
                pin_ok = false;
                consumer_ready.store(true, std::memory_order_release);
                return;
            }
            consumer_ready.store(true, std::memory_order_release);

            while (!start.load(std::memory_order_acquire)) {
                _mm_pause();
            }

            for (std::uint64_t expected = 0; expected < total; ++expected) {
                //get request
                while (!requests.try_consume(
                    [&](const lle::EventView& event) {
                        if (event.sequence != expected) {
                            correct.store(false, std::memory_order_relaxed);
                        }
                    }))
                {
                    _mm_pause();
                }
                //send response
                response.sequence = expected;
                while (!responses.try_push(response)) {
                    _mm_pause();
                }
            }
        });

        while (!consumer_ready.load(std::memory_order_acquire)) {
            _mm_pause();
        }

        if (!pin_ok) {
            consumer.join();
            state.SkipWithError("cannot pin consumer to requested cpu");
            break;
        }
        start.store(true, std::memory_order_release);

        //warmup

        for (std::uint64_t i = 0; i < warmup_count; ++i) {
            request.sequence = i;
            while (!requests.try_push(request)) {
                _mm_pause();
            }

            while (!responses.try_consume(
                [&](const lle::EventView& event) {
                    if (event.sequence != i) {
                        correct.store(false, std::memory_order_relaxed);
                    }
                }))
            {
                _mm_pause();
            }
        }

        state.ResumeTiming();
        //benchmark measurements stats

        for (std::uint64_t i = 0; i < sample_count; ++i) {
            request.sequence = i + warmup_count;
            const auto response_sequence = i + warmup_count;

            const auto begin = std::chrono::steady_clock::now();
            while (!requests.try_push(request)) {
                _mm_pause();
            }

            while (!responses.try_consume(
                [&](const lle::EventView& event) {
                    if (event.sequence != response_sequence) {
                        correct = false;
                    }
                }))
            {
                _mm_pause();
            }
            const auto end = std::chrono::steady_clock::now();
            samples[i] = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
        }

        state.PauseTiming();
        consumer.join();

        if (!correct.load(std::memory_order_relaxed)) {
            state.SkipWithError("received invalid sequence");
            break;
        }

        std::ranges::sort(samples);

        auto percentile = [&](const double p) {
            const auto index = static_cast<std::size_t>(
                std::ceil(p * static_cast<double>(samples.size()))) - 1;
            return samples[std::min(index,samples.size() - 1)];
        };

        state.counters["p50_ns"] = static_cast<double>(percentile(0.50));
        state.counters["p90_ns"] = static_cast<double>(percentile(0.90));
        state.counters["p99_ns"] = static_cast<double>(percentile(0.99));
        state.counters["p999_ns"] = static_cast<double>(percentile(0.999));

        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(sample_count));
}

template <std::size_t N>
void BM_LLE(benchmark::State& state) {
    run_throughput<LLEQueue, N>(state);
}

template <std::size_t N>
void BM_Rigtorp(benchmark::State& state) {
    run_throughput<RigtorpQueue, N>(state);
}

template <std::size_t N>
void BM_Boost(benchmark::State& state) {
    run_throughput<BoostQueue, N>(state);
}

void add_capacity_size(benchmark::internal::Benchmark* registration) {
    registration->Arg(64);
    registration->Arg(1024);
    registration->Arg(16384);
    registration->UseRealTime();

}

BENCHMARK_TEMPLATE(BM_LLE, 16)->Apply(add_capacity_size);
BENCHMARK_TEMPLATE(BM_LLE, 64)->Apply(add_capacity_size);
BENCHMARK_TEMPLATE(BM_LLE, 256)->Apply(add_capacity_size);
BENCHMARK_TEMPLATE(BM_LLE, 1024)->Apply(add_capacity_size);

BENCHMARK_TEMPLATE(BM_Rigtorp, 16)->Apply(add_capacity_size);
BENCHMARK_TEMPLATE(BM_Rigtorp, 64)->Apply(add_capacity_size);
BENCHMARK_TEMPLATE(BM_Rigtorp, 256)->Apply(add_capacity_size);
BENCHMARK_TEMPLATE(BM_Rigtorp, 1024)->Apply(add_capacity_size);

BENCHMARK_TEMPLATE(BM_Boost, 16)->Apply(add_capacity_size);
BENCHMARK_TEMPLATE(BM_Boost, 64)->Apply(add_capacity_size);
BENCHMARK_TEMPLATE(BM_Boost, 256)->Apply(add_capacity_size);
BENCHMARK_TEMPLATE(BM_Boost, 1024)->Apply(add_capacity_size);

template <std::size_t N>
void BM_LLE_RTT(benchmark::State& state) {
    run_rtt<LLEQueue, N>(state);
}

template <std::size_t N>
void BM_Rigtorp_RTT(benchmark::State& state) {
    run_rtt<RigtorpQueue, N>(state);
}

template <std::size_t N>
void BM_Boost_RTT(benchmark::State& state) {
    run_rtt<BoostQueue, N>(state);
}

// one iteration keeps percentile counters tied to a single 100,000-sample batch.
void configure_rtt(benchmark::internal::Benchmark* registration) {
    registration->UseRealTime()->Iterations(1);
}

BENCHMARK_TEMPLATE(BM_LLE_RTT, 16)->Apply(configure_rtt);
BENCHMARK_TEMPLATE(BM_LLE_RTT, 64)->Apply(configure_rtt);
BENCHMARK_TEMPLATE(BM_LLE_RTT, 256)->Apply(configure_rtt);
BENCHMARK_TEMPLATE(BM_LLE_RTT, 1024)->Apply(configure_rtt);

BENCHMARK_TEMPLATE(BM_Rigtorp_RTT, 16)->Apply(configure_rtt);
BENCHMARK_TEMPLATE(BM_Rigtorp_RTT, 64)->Apply(configure_rtt);
BENCHMARK_TEMPLATE(BM_Rigtorp_RTT, 256)->Apply(configure_rtt);
BENCHMARK_TEMPLATE(BM_Rigtorp_RTT, 1024)->Apply(configure_rtt);

BENCHMARK_TEMPLATE(BM_Boost_RTT, 16)->Apply(configure_rtt);
BENCHMARK_TEMPLATE(BM_Boost_RTT, 64)->Apply(configure_rtt);
BENCHMARK_TEMPLATE(BM_Boost_RTT, 256)->Apply(configure_rtt);
BENCHMARK_TEMPLATE(BM_Boost_RTT, 1024)->Apply(configure_rtt);

} // namespace

int main(int argc, char** argv) {
    try {
        // remove our options before google benchmark parses its own arguments.
        int remaining = 1;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg{argv[i]};
            int* cpu = nullptr;
            if (arg.starts_with("--producer_cpu=")) { cpu = &producer_cpu; }
            else if (arg.starts_with("--consumer_cpu=")) { cpu = &consumer_cpu; }
            if (cpu == nullptr) {
                argv[remaining++] = argv[i];
                continue;
            }
            const auto value = arg.substr(arg.find('=') + 1);
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), *cpu);
            if (error != std::errc{} || end != value.data() + value.size() ||
                *cpu < 0 || *cpu >= CPU_SETSIZE) {
                throw std::runtime_error("invalid cpu id: " + std::string(value));
            }
        }
        argc = remaining;
        argv[argc] = nullptr;
        benchmark::Initialize(&argc, argv);
        if (benchmark::ReportUnrecognizedArguments(argc, argv)) { return 1; }
        if (producer_cpu < 0 || consumer_cpu < 0 || producer_cpu == consumer_cpu) {
            throw std::runtime_error("provide distinct --producer_cpu=N and --consumer_cpu=N");
        }
        // validate both selections before running; the main thread is the producer.
        pin_to_cpu(consumer_cpu);
        pin_to_cpu(producer_cpu);
        benchmark::AddCustomContext("producer_cpu", std::to_string(producer_cpu));
        benchmark::AddCustomContext("consumer_cpu", std::to_string(consumer_cpu));
        benchmark::RunSpecifiedBenchmarks();
        benchmark::Shutdown();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
