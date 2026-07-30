#include <benchmark/benchmark.h>
#include <pthread.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <type_traits>

#include "jane/concurrency/spsc_ring_buffer.hpp"

namespace {

// Baseline for comparison only: functionally identical to
// jane::SPSCRingBuffer but without the cached-index optimization — every
// push/pop unconditionally re-reads the *other* thread's atomic index
// instead of trusting a local cache until it's proven stale. Exists to put
// a number on what the caching buys (see docs/tradeoffs.md); it is not
// part of the library's public surface.
template <typename T, std::size_t Capacity>
class NaiveSPSCRingBuffer {
    static_assert(Capacity > 1 && (Capacity & (Capacity - 1)) == 0);

public:
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= Capacity) return false;
        buffer_[static_cast<std::size_t>(head & kMask)] = item;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        if (tail == head) return false;
        out = buffer_[static_cast<std::size_t>(tail & kMask)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

private:
    static constexpr std::uint64_t kMask = Capacity - 1;
    alignas(64) std::array<T, Capacity> buffer_{};
    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::atomic<std::uint64_t> tail_{0};
};

// ~32 bytes: roughly the size of a decoded order-entry event.
struct Msg {
    std::uint64_t seq;
    std::uint64_t payload[3];
};
static_assert(std::is_trivially_copyable_v<Msg>);

constexpr std::uint64_t kOpsPerIteration = 1u << 20;

// Pin to a physical core (see `lscpu -e`: this machine's hyperthread
// siblings are CPU i and CPU i+12) so producer and consumer never land on
// sibling SMT threads, which would share L1/L2 and quietly invalidate the
// "separate cache lines" premise the whole design rests on.
void pin_to_core(std::thread& t, int core) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(static_cast<std::size_t>(core), &cpuset);
    ::pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
}

constexpr int kProducerCore = 2;
constexpr int kConsumerCore = 3;

// simulate_ns: busy-work the consumer does per message, standing in for
// "real" per-event processing (a matching-engine touch, a risk check).
// At 0 it isolates pure queue overhead; >0 shows how a consumer that
// isn't queue-bound changes the producer/consumer speed balance, which is
// exactly the variable the cached-index optimization is sensitive to.
template <typename Queue, int SimulateWork>
void run_spsc_throughput(benchmark::State& state) {
    for (auto _ : state) {
        auto q = std::make_unique<Queue>();
        std::atomic<bool> start{false};

        std::thread producer([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (std::uint64_t i = 0; i < kOpsPerIteration; ++i) {
                const Msg m{i, {i, i, i}};
                while (!q->try_push(m)) {
                }
            }
        });
        pin_to_core(producer, kProducerCore);

        start.store(true, std::memory_order_release);
        std::uint64_t received = 0;
        Msg out{};
        [[maybe_unused]] std::uint64_t sink = 0;
        while (received < kOpsPerIteration) {
            if (q->try_pop(out)) {
                ++received;
                if constexpr (SimulateWork > 0) {
                    for (int w = 0; w < SimulateWork; ++w) {
                        sink += out.seq * 2654435761u + static_cast<unsigned>(w);
                    }
                    benchmark::DoNotOptimize(sink);
                }
            }
        }
        producer.join();
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                             static_cast<std::int64_t>(kOpsPerIteration));
}

}  // namespace

// --- capacity sweep: pure queue overhead, no simulated consumer work ----

#define JANE_SPSC_CAPACITY_BENCHMARK(CAP)                                                     \
    BENCHMARK(run_spsc_throughput<jane::SPSCRingBuffer<Msg, CAP>, 0>)                          \
        ->Name("SPSC/cached_index/cap" #CAP)                                                   \
        ->Unit(benchmark::kMillisecond)                                                        \
        ->UseRealTime()                                                                        \
        ->Iterations(20);                                                                      \
    BENCHMARK(run_spsc_throughput<NaiveSPSCRingBuffer<Msg, CAP>, 0>)                           \
        ->Name("SPSC/naive_reread/cap" #CAP)                                                   \
        ->Unit(benchmark::kMillisecond)                                                        \
        ->UseRealTime()                                                                        \
        ->Iterations(20)

JANE_SPSC_CAPACITY_BENCHMARK(256);
JANE_SPSC_CAPACITY_BENCHMARK(4096);
JANE_SPSC_CAPACITY_BENCHMARK(65536);

#undef JANE_SPSC_CAPACITY_BENCHMARK

// --- fixed capacity, consumer does ~realistic per-message work ----------

BENCHMARK(run_spsc_throughput<jane::SPSCRingBuffer<Msg, 4096>, 20>)
    ->Name("SPSC/cached_index/cap4096/consumer_work")
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime()
    ->Iterations(20);

BENCHMARK(run_spsc_throughput<NaiveSPSCRingBuffer<Msg, 4096>, 20>)
    ->Name("SPSC/naive_reread/cap4096/consumer_work")
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime()
    ->Iterations(20);

// Single-threaded push-then-pop: no cross-core traffic at all, isolates
// the per-call bookkeeping cost (branch + index math + copy).
static void bench_spsc_single_threaded_roundtrip(benchmark::State& state) {
    jane::SPSCRingBuffer<Msg, 1024> q;
    const Msg in{1, {1, 2, 3}};
    Msg out{};
    for (auto _ : state) {
        benchmark::DoNotOptimize(q.try_push(in));
        benchmark::DoNotOptimize(q.try_pop(out));
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}
BENCHMARK(bench_spsc_single_threaded_roundtrip);

BENCHMARK_MAIN();
