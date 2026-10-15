#pragma once

// Shared by the throughput and latency benchmarks so every harness drives the
// three queues through identical adapters.

#include "shm/spsc_ring.hpp"

#include <boost/lockfree/spsc_queue.hpp>
#include <rigtorp/SPSCQueue.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lle_bench {

namespace util {
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

inline int read_cpu(const char* name) {
    const char* value = std::getenv(name);

    if (value == nullptr) {
        throw std::runtime_error(std::string{name} + " is not set");
    }

    int cpu = -1;

    const std::string_view text{value};
    const auto* end = text.data() + text.size();

    const auto [ptr, error] = std::from_chars(
        text.data(),
        end, //to get exactly const char*
        cpu
        );

    if (error != std::errc{} || ptr != end || cpu < 0 || cpu >= CPU_SETSIZE) {
        throw std::runtime_error(std::string{name} + " has invalid CPU id");
    }
    return cpu;
}
} //namespace util

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

} // namespace lle_bench
