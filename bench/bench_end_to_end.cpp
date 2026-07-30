#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "jane/marketdata/sinks.hpp"
#include "jane/matching/matching_engine.hpp"
#include "jane/metrics/histogram.hpp"
#include "jane/replay/replay_engine.hpp"
#include "jane/replay/synthetic_generator.hpp"

// The authoritative p50/p99/p99.9 numbers for this project: every other
// bench_*.cpp reports Google Benchmark's own aggregate mean over many
// iterations of a tight loop, which is the right tool for isolating a
// single fast operation's typical cost but doesn't produce a *tail*
// distribution. This file times each individual call with its own clock
// reading and records it into jane::metrics::LatencyHistogram, at a scale
// (hundreds of thousands of calls per run) large enough for p99.9 to mean
// something. See docs/benchmarks.md for the numbers, docs/tradeoffs.md
// for why the two methodologies can disagree by more than noise.
//
// Explicitly measured, not assumed: per-call std::chrono::steady_clock::
// now() has its own overhead, and for the fastest operations in this
// codebase (a bare order-book add/cancel, ~25ns per bench_order_book.cpp)
// that overhead is not negligible next to the thing being measured —
// bench_measurement_overhead reports it directly rather than leaving the
// reader to wonder how much of p50 elsewhere in this file is real.

using namespace jane;

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t elapsed_ns(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
}

void report_percentiles(benchmark::State& state, const metrics::LatencyHistogram& hist) {
    state.counters["p50_ns"] = static_cast<double>(hist.percentile(50));
    state.counters["p99_ns"] = static_cast<double>(hist.percentile(99));
    state.counters["p99_9_ns"] = static_cast<double>(hist.percentile(99.9));
    state.counters["max_ns"] = static_cast<double>(hist.max());
    state.counters["min_ns"] = static_cast<double>(hist.min());
    state.counters["mean_ns"] = hist.mean();
    state.counters["samples"] = static_cast<double>(hist.count());
}

Order make_limit(std::uint64_t id, Side side, std::int64_t price, std::int64_t qty) {
    Order o{};
    o.id = OrderId{id};
    o.client = ClientId{1};
    o.symbol = SymbolId{1};
    o.side = side;
    o.type = OrderType::Limit;
    o.tif = TimeInForce::Day;
    o.price = Price{price};
    o.quantity = Quantity{qty};
    o.remaining = Quantity{qty};
    return o;
}

}  // namespace

// The noise floor: nothing but two clock reads. Whatever this reports is
// baked into every other number in this file.
static void bench_measurement_overhead(benchmark::State& state) {
    metrics::LatencyHistogram hist;
    for (auto _ : state) {
        constexpr int kSamples = 100'000;
        for (int i = 0; i < kSamples; ++i) {
            const auto t0 = Clock::now();
            const auto t1 = Clock::now();
            hist.record(elapsed_ns(t0, t1));
        }
    }
    report_percentiles(state, hist);
}
BENCHMARK(bench_measurement_overhead)->Iterations(1)->Unit(benchmark::kMillisecond);

// Order book add+cancel round trip, individually timed — compare p50
// here against bench_order_book.cpp's GBench-aggregate number (25ns) for
// the literal same operation to see how much of p50 here is the
// measurement overhead quantified above rather than the operation itself.
static void bench_order_book_percentiles(benchmark::State& state) {
    book::OrderBook<1u << 16, 1u << 14> book(SymbolId{1}, Price{0});
    metrics::LatencyHistogram hist;
    const Order order = make_limit(1, Side::Buy, 100, 10);

    for (auto _ : state) {
        constexpr int kSamples = 200'000;
        for (int i = 0; i < kSamples; ++i) {
            const auto t0 = Clock::now();
            benchmark::DoNotOptimize(book.add(order));
            benchmark::DoNotOptimize(book.cancel(OrderId{1}));
            const auto t1 = Clock::now();
            hist.record(elapsed_ns(t0, t1));
        }
    }
    report_percentiles(state, hist);
}
BENCHMARK(bench_order_book_percentiles)->Iterations(1)->Unit(benchmark::kMillisecond);

// Matching engine crossing submit (one aggressor, one resting order fully
// consumed, a fresh resting order re-added), individually timed.
static void bench_matching_engine_percentiles(benchmark::State& state) {
    matching::MatchingEngine<1u << 16, 1u << 14> engine(SymbolId{1}, Price{0});
    metrics::LatencyHistogram hist;
    std::vector<matching::Fill> fills;
    std::uint64_t id = 1;
    benchmark::DoNotOptimize(engine.submit(make_limit(id++, Side::Sell, 100, 10), fills));
    for (auto _ : state) {
        constexpr int kSamples = 200'000;
        for (int i = 0; i < kSamples; ++i) {
            const auto t0 = Clock::now();
            fills.clear();
            benchmark::DoNotOptimize(engine.submit(make_limit(id++, Side::Buy, 100, 10), fills));
            benchmark::DoNotOptimize(engine.submit(make_limit(id++, Side::Sell, 100, 10), fills));
            const auto t1 = Clock::now();
            hist.record(elapsed_ns(t0, t1));
        }
    }
    report_percentiles(state, hist);
}
BENCHMARK(bench_matching_engine_percentiles)->Iterations(1)->Unit(benchmark::kMillisecond);

// The main event: the full driver loop (sequencer clock -> pre-trade risk
// -> matching -> market data publication) under realistic synthetic
// order flow, individually timed per order at a scale (500k orders) large
// enough for p99.9 to be a meaningful statistic and not a single sample.
static void bench_end_to_end_pipeline_percentiles(benchmark::State& state) {
    constexpr std::size_t kNumLevels = 4096;
    constexpr std::size_t kMaxOrders = 300'000;
    constexpr std::size_t kMaxAccounts = 64;
    constexpr int kOrderCount = 500'000;
    constexpr std::int64_t kBasePrice = 8800;
    constexpr std::int64_t kMidPrice = 9000;
    constexpr std::int64_t kSpread = 195;  // keeps [mid-spread, mid+spread] inside [base, base+levels)

    for (auto _ : state) {
        // Heap-allocated, not a local: MatchingEngine owns its OrderBook
        // by value, which owns a SlabPool<OrderNode, kMaxOrders> by value
        // — at this capacity that's tens of megabytes. A first version of
        // this benchmark declared it as a stack local and crashed with a
        // stack overflow (SIGSEGV) the first time it actually ran, past
        // the default ~8MB thread stack; every other bench_*.cpp in this
        // project uses a small enough capacity (≤16384 orders) that this
        // never came up.
        auto engine = std::make_unique<matching::MatchingEngine<kNumLevels, kMaxOrders>>(
            SymbolId{1}, Price{kBasePrice});
        marketdata::InMemorySink sink;
        // ~300 bytes/order of published market data + exec reports,
        // generously rounded up — see InMemorySink::reserve's doc comment
        // for what happens if this is skipped.
        sink.reserve(static_cast<std::size_t>(kOrderCount) * 512);
        risk::RiskEngine<kMaxAccounts> risk(
            risk::Limits{.max_order_size = Quantity{1'000'000},
                         .max_position = Quantity{10'000'000},
                         .max_loss_per_client = PnL{-1'000'000'000'000LL}});
        marketdata::MarketDataPublisher<marketdata::InMemorySink> feed(sink, 1u << 16);
        replay::ReplayEngine<kNumLevels, kMaxOrders, kMaxAccounts, marketdata::InMemorySink> replay(
            *engine, risk, feed);
        replay::SyntheticOrderGenerator gen(replay::SyntheticConfig{
            .seed = 42,
            .symbol = SymbolId{1},
            .mid_price = Price{kMidPrice},
            .price_spread_ticks = kSpread,
            .min_quantity = 1,
            .max_quantity = 100,
            .market_order_fraction = 0.05,
            .buy_fraction = 0.5,
        });
        replay::DeterministicClock clock;
        metrics::LatencyHistogram hist;
        int capacity_rejections = 0;

        for (int i = 0; i < kOrderCount; ++i) {
            const auto msg =
                gen.next_order(static_cast<std::uint64_t>(i + 1), static_cast<std::uint32_t>(1 + i % 20));
            const Nanos ts = clock.tick();

            const auto t0 = Clock::now();
            replay.process_new_order(msg, ts);
            const auto t1 = Clock::now();
            hist.record(elapsed_ns(t0, t1));

            if (engine->book().order_count() >= kMaxOrders - 1) {
                ++capacity_rejections;  // sized generously so this should stay at 0; tracked, not assumed
            }
        }

        report_percentiles(state, hist);
        state.counters["resting_at_end"] = static_cast<double>(engine->book().order_count());
        state.counters["near_capacity_events"] = static_cast<double>(capacity_rejections);
    }
}
BENCHMARK(bench_end_to_end_pipeline_percentiles)->Iterations(1)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
