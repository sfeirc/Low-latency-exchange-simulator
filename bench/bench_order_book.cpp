#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

#include "jane/book/order_book.hpp"
#include "jane/core/order.hpp"

using namespace jane;
using namespace jane::book;

namespace {

using BenchBook = OrderBook<1u << 16, 1u << 14>;  // 65536 price levels, 16384 resting orders

Order make_order(std::uint64_t id, Side side, std::int64_t price, std::int64_t qty) {
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

// Single order, same price, add then immediately cancel: isolates the
// per-call cost of the hash-map insert/erase and intrusive list
// splice/unsplice without ever touching the bitmap's relocation path
// (there's only ever one order, so its level is always both the first
// and last occupied one).
static void bench_add_cancel_same_level(benchmark::State& state) {
    BenchBook book(SymbolId{1}, Price{0});
    const Order order = make_order(1, Side::Buy, 100, 10);
    for (auto _ : state) {
        auto added = book.add(order);
        benchmark::DoNotOptimize(added);
        auto cancelled = book.cancel(OrderId{1});
        benchmark::DoNotOptimize(cancelled);
    }
}
BENCHMARK(bench_add_cancel_same_level);

// Pure read: book pre-populated, repeatedly query best_bid — should be a
// cached-field read (O(1) always, not just amortized), no bitmap scan.
static void bench_best_bid_query(benchmark::State& state) {
    BenchBook book(SymbolId{1}, Price{0});
    for (std::uint64_t i = 0; i < 1000; ++i) {
        benchmark::DoNotOptimize(book.add(make_order(i + 1, Side::Buy, static_cast<std::int64_t>(i), 10)));
    }
    for (auto _ : state) {
        benchmark::DoNotOptimize(book.best_bid());
    }
}
BENCHMARK(bench_best_bid_query);

// Churn across many distinct price levels: add N orders spread across N
// different levels, cancel them in a different order than they were
// added (front-to-back rather than LIFO), repeat. This is what actually
// exercises note_nonempty/note_maybe_empty and, whenever the current-best
// level empties, the bitmap relocation scan — bench_add_cancel_same_level
// above never touches that path at all.
constexpr std::uint64_t kChurnLevels = 2048;

static void bench_churn_across_levels(benchmark::State& state) {
    BenchBook book(SymbolId{1}, Price{0});
    std::vector<OrderId> ids;
    ids.reserve(kChurnLevels);

    for (auto _ : state) {
        for (std::uint64_t i = 0; i < kChurnLevels; ++i) {
            auto result = book.add(make_order(i + 1, Side::Buy, static_cast<std::int64_t>(i), 10));
            ids.push_back(*result);
        }
        for (const OrderId id : ids) {
            benchmark::DoNotOptimize(book.cancel(id));
        }
        ids.clear();
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                             static_cast<std::int64_t>(kChurnLevels));
}
BENCHMARK(bench_churn_across_levels)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
