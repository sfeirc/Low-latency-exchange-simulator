#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>

#include "jane/protocol/codec.hpp"
#include "jane/protocol/messages.hpp"

// Sanity-check throughput/mean latency during development. The
// authoritative p50/p99/p99.9 latency numbers this project actually
// reports live in bench/bench_end_to_end.cpp (task: benchmark suite),
// built on jane::LatencyHistogram — a single per-call mean from Google
// Benchmark's iteration loop isn't a tail-latency distribution, and this
// codec is squarely on the per-order hot path, so it gets measured there
// with the same methodology as the order book and matching engine rather
// than a one-off percentile calculation duplicated here.

using namespace jane;
using namespace jane::protocol;

static void bench_encode_new_order(benchmark::State& state) {
    const NewOrderMessage msg{.order_id = 1,
                               .client_id = 1,
                               .price = 10'000,
                               .quantity = 100,
                               .symbol_id = 1,
                               .side = Side::Buy,
                               .order_type = OrderType::Limit,
                               .time_in_force = TimeInForce::Day};
    std::array<std::byte, 128> buf{};
    for (auto _ : state) {
        benchmark::DoNotOptimize(encode(std::span(buf), msg));
    }
}
BENCHMARK(bench_encode_new_order);

static void bench_decode_new_order(benchmark::State& state) {
    const NewOrderMessage msg{.order_id = 1,
                               .client_id = 1,
                               .price = 10'000,
                               .quantity = 100,
                               .symbol_id = 1,
                               .side = Side::Buy,
                               .order_type = OrderType::Limit,
                               .time_in_force = TimeInForce::Day};
    std::array<std::byte, 128> buf{};
    const std::size_t written = encode(std::span(buf), msg);
    const auto encoded = std::span(buf).first(written);
    for (auto _ : state) {
        benchmark::DoNotOptimize(decode<NewOrderMessage>(encoded));
    }
}
BENCHMARK(bench_decode_new_order);

static void bench_encode_decode_round_trip(benchmark::State& state) {
    const ExecutionReportMessage msg{.order_id = 1,
                                      .client_id = 1,
                                      .match_id = 1,
                                      .price = 10'000,
                                      .last_quantity = 10,
                                      .leaves_quantity = 90,
                                      .symbol_id = 1,
                                      .exec_type = ExecType::PartialFill,
                                      .reject_reason = RejectReason::None};
    std::array<std::byte, 128> buf{};
    for (auto _ : state) {
        const std::size_t written = encode(std::span(buf), msg);
        benchmark::DoNotOptimize(decode<ExecutionReportMessage>(std::span(buf).first(written)));
    }
}
BENCHMARK(bench_encode_decode_round_trip);

BENCHMARK_MAIN();
