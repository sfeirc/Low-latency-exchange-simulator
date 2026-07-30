#include <benchmark/benchmark.h>
#include <pthread.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

#include "jane/concurrency/fan_in_sequencer.hpp"
#include "jane/concurrency/mpsc_ring_buffer.hpp"

namespace {

struct Msg {
    std::uint64_t seq;
    std::uint64_t payload[3];
};
static_assert(std::is_trivially_copyable_v<Msg>);

void pin_to_core(std::thread& t, int core) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(static_cast<std::size_t>(core), &cpuset);
    ::pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
}

void pin_self_to_core(int core) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(static_cast<std::size_t>(core), &cpuset);
    ::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set_t), &cpuset);
}

// Producers pinned to physical cores 0..N-1; consumer (the benchmark's own
// thread) pinned to core 11 — free across every producer count tested
// below (max 8 producers) so it never lands on a producer's core or that
// core's SMT sibling.
constexpr int kConsumerCore = 11;
constexpr std::uint64_t kOpsPerProducer = 1u << 18;

template <std::size_t NumProducers>
void bench_mpsc_ring_buffer(benchmark::State& state) {
    pin_self_to_core(kConsumerCore);
    for (auto _ : state) {
        auto q = std::make_unique<jane::MPSCRingBuffer<Msg, 4096>>();
        std::atomic<bool> start{false};
        std::vector<std::thread> producers;
        producers.reserve(NumProducers);
        for (std::size_t p = 0; p < NumProducers; ++p) {
            producers.emplace_back([&q, &start, p] {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (std::uint64_t i = 0; i < kOpsPerProducer; ++i) {
                    const Msg m{i, {p, i, i}};
                    while (!q->try_push(m)) {
                    }
                }
            });
            pin_to_core(producers.back(), static_cast<int>(p));
        }
        start.store(true, std::memory_order_release);

        std::uint64_t received = 0;
        Msg out{};
        const std::uint64_t total = kOpsPerProducer * NumProducers;
        while (received < total) {
            if (q->try_pop(out)) {
                ++received;
            }
        }
        for (auto& t : producers) t.join();
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                             static_cast<std::int64_t>(kOpsPerProducer * NumProducers));
}

template <std::size_t NumProducers>
void bench_fan_in_sequencer(benchmark::State& state) {
    pin_self_to_core(kConsumerCore);
    for (auto _ : state) {
        auto seq = std::make_unique<jane::FanInSequencer<Msg, NumProducers, 4096>>();
        std::atomic<bool> start{false};
        std::vector<std::thread> producers;
        producers.reserve(NumProducers);
        for (std::size_t p = 0; p < NumProducers; ++p) {
            producers.emplace_back([&seq, &start, p] {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (std::uint64_t i = 0; i < kOpsPerProducer; ++i) {
                    const Msg m{i, {p, i, i}};
                    while (!seq->push_from(p, m)) {
                    }
                }
            });
            pin_to_core(producers.back(), static_cast<int>(p));
        }
        start.store(true, std::memory_order_release);

        std::uint64_t received = 0;
        Msg out{};
        std::size_t src = 0;
        const std::uint64_t total = kOpsPerProducer * NumProducers;
        while (received < total) {
            if (seq->try_drain_one(out, src)) {
                ++received;
            }
        }
        for (auto& t : producers) t.join();
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                             static_cast<std::int64_t>(kOpsPerProducer * NumProducers));
}

}  // namespace

#define JANE_MPSC_PRODUCER_BENCHMARK(N)                                     \
    BENCHMARK(bench_mpsc_ring_buffer<N>)                                    \
        ->Name("MPSC/cas_shared/producers" #N)                              \
        ->Unit(benchmark::kMillisecond)                                     \
        ->UseRealTime()                                                     \
        ->Iterations(10);                                                   \
    BENCHMARK(bench_fan_in_sequencer<N>)                                    \
        ->Name("MPSC/fanin_spsc/producers" #N)                              \
        ->Unit(benchmark::kMillisecond)                                     \
        ->UseRealTime()                                                     \
        ->Iterations(10)

JANE_MPSC_PRODUCER_BENCHMARK(2);
JANE_MPSC_PRODUCER_BENCHMARK(4);
JANE_MPSC_PRODUCER_BENCHMARK(8);

#undef JANE_MPSC_PRODUCER_BENCHMARK

BENCHMARK_MAIN();
