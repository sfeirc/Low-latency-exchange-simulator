#include <benchmark/benchmark.h>

#include <vector>

#include "jane/matching/matching_engine.hpp"

using namespace jane;
using namespace jane::matching;

namespace {

using Engine = MatchingEngine<1u << 16, 1u << 14>;

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

// A resting order sits at 100; every iteration submits an aggressive buy
// that fully consumes it, then re-rests an identical sell so the next
// iteration has the same thing to cross again. Isolates one full
// cross-and-fill cycle: the actual hot path this whole project is about.
// out_fills is declared outside the timed loop and cleared (not
// reallocated) each iteration — the steady-state-zero-allocation usage
// submit()'s doc comment describes; see the fresh-vs-reused comparison
// below for what that's actually worth.
static void bench_submit_crossing_single_fill(benchmark::State& state) {
    Engine engine(SymbolId{1}, Price{0});
    std::uint64_t id = 1;
    std::vector<Fill> fills;
    fills.reserve(4);
    benchmark::DoNotOptimize(engine.submit(make_limit(id++, Side::Sell, 100, 10), fills));
    for (auto _ : state) {
        fills.clear();
        auto result = engine.submit(make_limit(id++, Side::Buy, 100, 10), fills);
        benchmark::DoNotOptimize(result);
        auto rest = engine.submit(make_limit(id++, Side::Sell, 100, 10), fills);
        benchmark::DoNotOptimize(rest);
    }
}
BENCHMARK(bench_submit_crossing_single_fill);

// Non-crossing submit: pure "add to book" cost through the matching
// engine's validation/dispatch layer, paired with a cancel so state
// doesn't accumulate across iterations.
static void bench_submit_non_crossing_then_cancel(benchmark::State& state) {
    Engine engine(SymbolId{1}, Price{0});
    std::uint64_t id = 1;
    std::vector<Fill> fills;
    for (auto _ : state) {
        auto result = engine.submit(make_limit(id, Side::Buy, 100, 10), fills);
        benchmark::DoNotOptimize(result);
        benchmark::DoNotOptimize(engine.cancel(OrderId{id}));
        ++id;
    }
}
BENCHMARK(bench_submit_non_crossing_then_cancel);

// Walks 10 resting orders at the same level in one aggressive sweep —
// exercises the FIFO-walking loop across multiple resting orders per
// call, not just the single-fill case above.
static void bench_submit_sweeps_ten_resting_orders(benchmark::State& state) {
    Engine engine(SymbolId{1}, Price{0});
    std::uint64_t id = 1;
    std::vector<Fill> fills;
    for (auto _ : state) {
        state.PauseTiming();
        fills.clear();
        for (int i = 0; i < 10; ++i) {
            benchmark::DoNotOptimize(engine.submit(make_limit(id++, Side::Sell, 100, 10), fills));
        }
        fills.clear();
        state.ResumeTiming();

        auto result = engine.submit(make_limit(id++, Side::Buy, 100, 100), fills);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(bench_submit_sweeps_ten_resting_orders);

// --- reused vs. fresh out_fills: what the caller-provided-buffer API buys --
//
// Same crossing-single-fill workload as above, but the "fresh" variant
// constructs a brand new std::vector<Fill> every call (what submit() used
// to do internally when it returned NewOrderResult::fills by value)
// instead of reusing one across iterations, to put a number on why that
// changed — see docs/tradeoffs.md.

static void bench_reused_out_fills(benchmark::State& state) {
    Engine engine(SymbolId{1}, Price{0});
    std::uint64_t id = 1;
    std::vector<Fill> fills;
    fills.reserve(4);
    benchmark::DoNotOptimize(engine.submit(make_limit(id++, Side::Sell, 100, 10), fills));
    for (auto _ : state) {
        fills.clear();
        benchmark::DoNotOptimize(engine.submit(make_limit(id++, Side::Buy, 100, 10), fills));
        benchmark::DoNotOptimize(engine.submit(make_limit(id++, Side::Sell, 100, 10), fills));
    }
}
BENCHMARK(bench_reused_out_fills);

static void bench_fresh_out_fills(benchmark::State& state) {
    Engine engine(SymbolId{1}, Price{0});
    std::uint64_t id = 1;
    std::vector<Fill> seed_fills;
    benchmark::DoNotOptimize(engine.submit(make_limit(id++, Side::Sell, 100, 10), seed_fills));
    for (auto _ : state) {
        std::vector<Fill> fresh;  // allocates on first push_back, every iteration
        benchmark::DoNotOptimize(engine.submit(make_limit(id++, Side::Buy, 100, 10), fresh));
        std::vector<Fill> fresh2;
        benchmark::DoNotOptimize(engine.submit(make_limit(id++, Side::Sell, 100, 10), fresh2));
    }
}
BENCHMARK(bench_fresh_out_fills);

// Same comparison, but each call generates 20 fills (sweeping 20 resting
// orders) instead of 1 — a fresh vector now pays several *reallocate-and-
// copy-already-collected-fills* steps as it grows (1->2->4->8->16->32),
// not just one small allocation, so the gap should widen relative to the
// single-fill case above.
constexpr int kSweepFillCount = 20;

static void bench_reused_out_fills_20_way_sweep(benchmark::State& state) {
    Engine engine(SymbolId{1}, Price{0});
    std::uint64_t id = 1;
    std::vector<Fill> fills;
    for (auto _ : state) {
        state.PauseTiming();
        fills.clear();
        for (int i = 0; i < kSweepFillCount; ++i) {
            benchmark::DoNotOptimize(engine.submit(make_limit(id++, Side::Sell, 100, 10), fills));
        }
        fills.clear();
        state.ResumeTiming();

        benchmark::DoNotOptimize(
            engine.submit(make_limit(id++, Side::Buy, 100, 10 * kSweepFillCount), fills));
    }
}
BENCHMARK(bench_reused_out_fills_20_way_sweep);

static void bench_fresh_out_fills_20_way_sweep(benchmark::State& state) {
    Engine engine(SymbolId{1}, Price{0});
    std::uint64_t id = 1;
    std::vector<Fill> setup_fills;
    for (auto _ : state) {
        state.PauseTiming();
        setup_fills.clear();
        for (int i = 0; i < kSweepFillCount; ++i) {
            benchmark::DoNotOptimize(
                engine.submit(make_limit(id++, Side::Sell, 100, 10), setup_fills));
        }
        state.ResumeTiming();

        std::vector<Fill> fresh;  // no reserve: grows/reallocates as fills accumulate
        benchmark::DoNotOptimize(
            engine.submit(make_limit(id++, Side::Buy, 100, 10 * kSweepFillCount), fresh));
    }
}
BENCHMARK(bench_fresh_out_fills_20_way_sweep);

BENCHMARK_MAIN();
