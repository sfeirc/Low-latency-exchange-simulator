#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "jane/memory/pmr_pool.hpp"
#include "jane/memory/slab_pool.hpp"

namespace {

// Roughly the shape/size of jane::Order — the actual pooled type.
struct OrderLike {
    std::uint64_t id = 0;
    std::uint64_t client = 0;
    std::int64_t price = 0;
    std::int64_t qty = 0;
    std::int64_t remaining = 0;
    std::uint64_t sequence = 0;
    std::int64_t timestamp = 0;
    std::uint8_t side = 0;
    std::uint8_t type = 0;
};
static_assert(std::is_trivially_destructible_v<OrderLike>);

constexpr std::size_t kPoolCapacity = 1u << 16;

}  // namespace

// --- immediate alloc/dealloc of the same slot ----------------------------
// Isolates raw per-call overhead. Because the very next allocate() always
// gets back the slot just freed, this is the *best case* for any
// allocator with a LIFO-ish free list — see the churn benchmark below for
// a less favorable access pattern.

static void bench_slab_pool_immediate_cycle(benchmark::State& state) {
    jane::SlabPool<OrderLike, kPoolCapacity> pool;
    for (auto _ : state) {
        OrderLike* p = pool.allocate();
        benchmark::DoNotOptimize(p);
        pool.deallocate(p);
    }
}
BENCHMARK(bench_slab_pool_immediate_cycle);

static void bench_pmr_pool_immediate_cycle(benchmark::State& state) {
    jane::PmrPool<OrderLike> pool(kPoolCapacity);
    for (auto _ : state) {
        OrderLike* p = pool.allocate();
        benchmark::DoNotOptimize(p);
        pool.deallocate(p);
    }
}
BENCHMARK(bench_pmr_pool_immediate_cycle);

static void bench_global_new_delete_immediate_cycle(benchmark::State& state) {
    for (auto _ : state) {
        auto* p = new OrderLike();
        benchmark::DoNotOptimize(p);
        delete p;
    }
}
BENCHMARK(bench_global_new_delete_immediate_cycle);

// --- churn: allocate a batch, free every other one, refill, free all ----
// A less LIFO-friendly pattern than the immediate cycle above — closer to
// an order book where cancels/fills free slots in an order unrelated to
// allocation order.

template <typename AllocateFn, typename DeallocateFn>
void run_churn(benchmark::State& state, std::size_t batch, AllocateFn allocate,
               DeallocateFn deallocate) {
    std::vector<OrderLike*> live;
    live.reserve(batch);
    for (auto _ : state) {
        for (std::size_t i = 0; i < batch; ++i) {
            live.push_back(allocate());
        }
        for (std::size_t i = 0; i < batch; i += 2) {
            deallocate(live[i]);
        }
        for (std::size_t i = 0; i < batch; i += 2) {
            live[i] = allocate();
        }
        for (OrderLike* p : live) {
            deallocate(p);
        }
        live.clear();
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                             static_cast<std::int64_t>(batch));
}

constexpr std::size_t kChurnBatch = 4096;

static void bench_slab_pool_churn(benchmark::State& state) {
    jane::SlabPool<OrderLike, kChurnBatch> pool;
    run_churn(
        state, kChurnBatch, [&] { return pool.allocate(); },
        [&](OrderLike* p) { pool.deallocate(p); });
}
BENCHMARK(bench_slab_pool_churn)->Unit(benchmark::kMicrosecond);

static void bench_pmr_pool_churn(benchmark::State& state) {
    jane::PmrPool<OrderLike> pool(kChurnBatch);
    run_churn(
        state, kChurnBatch, [&] { return pool.allocate(); },
        [&](OrderLike* p) { pool.deallocate(p); });
}
BENCHMARK(bench_pmr_pool_churn)->Unit(benchmark::kMicrosecond);

static void bench_global_new_delete_churn(benchmark::State& state) {
    run_churn(
        state, kChurnBatch, [] { return new OrderLike(); }, [](OrderLike* p) { delete p; });
}
BENCHMARK(bench_global_new_delete_churn)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
