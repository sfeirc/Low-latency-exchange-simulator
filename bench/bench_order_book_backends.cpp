#include <benchmark/benchmark.h>

#include <cstdint>
#include <random>
#include <vector>

#include "jane/book/price_index_variants.hpp"

// Isolates the one dimension that differs between plausible order book
// designs — how occupied price levels are indexed — from everything else
// (FIFO management, allocation) that's identical regardless of this
// choice and already benchmarked elsewhere. See
// include/jane/book/price_index_variants.hpp and docs/tradeoffs.md.

using namespace jane::book::variants;

namespace {
constexpr std::int64_t kRange = 1 << 16;     // representable price range
constexpr int kChurnOpsPerIteration = 5000;  // erase-best + insert-new, per timed iteration

// Fixed seed: every variant sees the exact same random price sequence, so
// differences in the result are the data structure, not the input.
std::vector<std::int64_t> make_price_sequence(std::size_t n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<std::int64_t> dist(0, kRange - 1);
    std::vector<std::int64_t> prices(n);
    for (auto& p : prices) p = dist(rng);
    return prices;
}

// BookDepth is a template parameter (not a runtime one) so a thinly
// resting instrument and a deeply resting one are two distinct, clearly
// labeled benchmarks rather than one number that hides which regime it
// was measured in.
template <typename Index, std::size_t BookDepth>
void run_churn_benchmark(benchmark::State& state) {
    const auto build_up = make_price_sequence(BookDepth, 1);
    const auto churn_inserts = make_price_sequence(
        static_cast<std::size_t>(kChurnOpsPerIteration) * 20 /* headroom across iterations */, 2);

    for (auto _ : state) {
        state.PauseTiming();
        Index index(kRange);  // constructible from a capacity hint (Ladder); Tree/Flat ignore it
        for (auto p : build_up) index.insert(p);
        std::size_t churn_cursor = 0;
        state.ResumeTiming();

        for (int i = 0; i < kChurnOpsPerIteration; ++i) {
            const auto best = index.best();
            if (best.has_value()) {
                index.erase(*best);
            }
            index.insert(churn_inserts[churn_cursor]);
            churn_cursor = (churn_cursor + 1) % churn_inserts.size();
        }
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * kChurnOpsPerIteration);
}

// LadderIndexVariant's constructor takes a capacity; Tree/Flat don't need
// one, so give them a same-signature adapter for the template above.
struct TreeIndexAdapter : TreeIndexVariant {
    explicit TreeIndexAdapter(std::int64_t /*capacity_hint*/) {}
};
struct FlatIndexAdapter : FlatIndexVariant {
    explicit FlatIndexAdapter(std::int64_t /*capacity_hint*/) {}
};

}  // namespace

#define JANE_PRICE_INDEX_BENCHMARK(DEPTH)                                     \
    BENCHMARK(run_churn_benchmark<LadderIndexVariant, DEPTH>)                 \
        ->Name("PriceIndex/ladder_bitmap/depth" #DEPTH)                       \
        ->Unit(benchmark::kMicrosecond)                                       \
        ->Iterations(200);                                                    \
    BENCHMARK(run_churn_benchmark<TreeIndexAdapter, DEPTH>)                   \
        ->Name("PriceIndex/tree_stdset/depth" #DEPTH)                         \
        ->Unit(benchmark::kMicrosecond)                                       \
        ->Iterations(200);                                                    \
    BENCHMARK(run_churn_benchmark<FlatIndexAdapter, DEPTH>)                   \
        ->Name("PriceIndex/flat_sorted_vector/depth" #DEPTH)                  \
        ->Unit(benchmark::kMicrosecond)                                       \
        ->Iterations(200)

JANE_PRICE_INDEX_BENCHMARK(20);    // a thinly-quoted instrument
JANE_PRICE_INDEX_BENCHMARK(2000);  // a deeply-quoted / wide-band instrument

#undef JANE_PRICE_INDEX_BENCHMARK

BENCHMARK_MAIN();
