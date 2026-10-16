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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <random>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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

// Queue latency on this hardware depends on where queue objects and buffers fall inside a
// 4 KB page (shared cache lines, 128-byte prefetch pairs, 4K aliasing). ASLR moves whole
// pages only, so every benchmark allocation goes through place_bytes(): each block starts at
// a random multiple of its required alignment (at least 64 bytes) within a page, drawn from
// LLE_LAYOUT_SEED (0 or unset: no shift). Each queue's own alignment contract is honoured.
namespace layout {

inline constexpr std::size_t page_bytes = 4096;
inline constexpr std::size_t line_bytes = 64;
// two lines before and after each block, so no neighbouring data shares a 128-byte pair.
inline constexpr std::size_t guard_bytes = 128;

struct State {
    std::uint64_t seed{0};
    std::mt19937_64 rng{};
};

inline State& state() {
    static State instance;
    return instance;
}

// call once in main() before any queue is constructed.
inline std::uint64_t init_from_env() {
    auto& current = state();
    current.seed = 0;
    const char* value = std::getenv("LLE_LAYOUT_SEED");
    if (value != nullptr && *value != '\0') {
        const std::string_view text{value};
        const auto* end = text.data() + text.size();
        const auto [ptr, error] = std::from_chars(text.data(), end, current.seed);
        if (error != std::errc{} || ptr != end) {
            throw std::runtime_error("LLE_LAYOUT_SEED must be an unsigned integer");
        }
    }
    current.rng.seed(current.seed);
    return current.seed;
}

inline std::size_t next_offset(std::size_t step) {
    auto& current = state();
    if (current.seed == 0) {
        return 0;
    }
    return static_cast<std::size_t>(current.rng() % (page_bytes / step)) * step;
}

// storage for `bytes` at a seeded offset within a page that keeps `alignment` (at least one
// cache line); logs the placement.
inline void* place_bytes(std::size_t bytes, std::size_t alignment, const char* what) {
    if (alignment > guard_bytes || (alignment & (alignment - 1)) != 0) {
        throw std::runtime_error("layout: alignment must be a power of two up to 128 bytes");
    }
    const std::size_t offset = next_offset(alignment < line_bytes ? line_bytes : alignment);
    auto* raw = static_cast<std::byte*>(operator new(
        guard_bytes + page_bytes + bytes + guard_bytes, std::align_val_t{page_bytes}
    ));
    auto* block = raw + guard_bytes + offset;
    // the guard in front of the block remembers the allocation for release_bytes().
    std::memcpy(block - sizeof(raw), &raw, sizeof(raw));
    std::fprintf(
        stderr, "layout %s bytes=%zu page_offset=%zu\n", what, bytes,
        static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(block) % page_bytes)
    );
    return block;
}

inline void release_bytes(void* block) noexcept {
    if (block == nullptr) {
        return;
    }
    std::byte* raw = nullptr;
    std::memcpy(&raw, static_cast<std::byte*>(block) - sizeof(raw), sizeof(raw));
    operator delete(raw, std::align_val_t{page_bytes});
}

// standard allocator for the baselines' slot arrays, so their buffers move like LLE's.
template <class T>
struct PlacedAllocator {
    using value_type = T;

    PlacedAllocator() noexcept = default;
    template <class U>
    PlacedAllocator(const PlacedAllocator<U>&) noexcept {}

    T* allocate(std::size_t count) {
        return static_cast<T*>(place_bytes(count * sizeof(T), alignof(T), "buffer"));
    }
    void deallocate(T* pointer, std::size_t) noexcept { release_bytes(pointer); }

    template <class U>
    bool operator==(const PlacedAllocator<U>&) const noexcept {
        return true;
    }
};

template <class T>
struct PlacedDeleter {
    void operator()(T* pointer) const noexcept {
        pointer->~T();
        release_bytes(pointer);
    }
};

template <class T>
using Placed = std::unique_ptr<T, PlacedDeleter<T>>;

// a queue object in its own guarded block at a seeded offset; neither thread's stack or
// other data can share a cache line with it.
template <class T, class... Args>
Placed<T> make_placed(Args&&... args) {
    void* block = place_bytes(sizeof(T), alignof(T), "object");
    try {
        return Placed<T>(new (block) T(std::forward<Args>(args)...));
    } catch (...) {
        release_bytes(block);
        throw;
    }
}

} // namespace layout

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
        void operator()(std::byte* ptr) const { layout::release_bytes(ptr); }
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
        storage_.reset(static_cast<std::byte*>(
            layout::place_bytes(*size, lle::shm::ring_memory_alignment, "buffer")
        ));
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
    rigtorp::SPSCQueue<Record<N>, layout::PlacedAllocator<Record<N>>> queue_;
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
    boost::lockfree::
        spsc_queue<Record<N>, boost::lockfree::allocator<layout::PlacedAllocator<Record<N>>>>
            queue_;
};

} // namespace lle_bench
