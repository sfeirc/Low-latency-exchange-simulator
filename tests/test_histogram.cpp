#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "jane/metrics/counters.hpp"
#include "jane/metrics/histogram.hpp"

using jane::metrics::LatencyHistogram;

namespace {
// Exact percentile from a fully materialized, sorted sample set — the
// oracle the histogram's bucketed estimate is checked against. Uses the
// same ceil-based rank convention as LatencyHistogram::percentile so the
// two are actually comparable.
std::int64_t exact_percentile(std::vector<std::int64_t> sorted_samples, double p) {
    std::sort(sorted_samples.begin(), sorted_samples.end());
    if (p <= 0.0) return sorted_samples.front();
    if (p >= 100.0) return sorted_samples.back();
    const auto rank = static_cast<std::size_t>(
        std::ceil(p / 100.0 * static_cast<double>(sorted_samples.size())));
    return sorted_samples[std::min(rank, sorted_samples.size()) - 1];
}
}  // namespace

TEST_CASE("LatencyHistogram: empty histogram reports zeros, not garbage or UB", "[histogram]") {
    LatencyHistogram h;
    REQUIRE(h.count() == 0);
    REQUIRE(h.min() == 0);
    REQUIRE(h.max() == 0);
    REQUIRE(h.mean() == 0.0);
    REQUIRE(h.percentile(50) == 0);
    REQUIRE(h.percentile(99.9) == 0);
}

TEST_CASE("LatencyHistogram: a single recorded value is every percentile", "[histogram]") {
    LatencyHistogram h;
    h.record(1234);
    REQUIRE(h.count() == 1);
    REQUIRE(h.min() == 1234);
    REQUIRE(h.max() == 1234);
    REQUIRE(h.percentile(0) == 1234);
    REQUIRE(h.percentile(50) == 1234);
    REQUIRE(h.percentile(99.9) == 1234);
    REQUIRE(h.percentile(100) == 1234);
}

TEST_CASE("LatencyHistogram: exact precision within the linear region (< 8192 ns)",
          "[histogram]") {
    LatencyHistogram h;
    for (std::int64_t v = 1; v <= 1000; ++v) {
        h.record(v);
    }
    REQUIRE(h.count() == 1000);
    REQUIRE(h.min() == 1);
    REQUIRE(h.max() == 1000);
    // Every value in this region maps to its own bucket, so these are
    // exact, not approximate.
    REQUIRE(h.percentile(50) == 500);
    REQUIRE(h.percentile(90) == 900);
    REQUIRE(h.percentile(99) == 990);
    REQUIRE(h.percentile(99.9) == 999);
    REQUIRE(h.mean() > 500.0);
    REQUIRE(h.mean() < 501.0);
}

TEST_CASE("LatencyHistogram: percentile is monotonically non-decreasing in p", "[histogram]") {
    LatencyHistogram h;
    std::mt19937_64 rng(11);
    std::uniform_int_distribution<std::int64_t> dist(1, 5'000'000);
    for (int i = 0; i < 10'000; ++i) {
        h.record(dist(rng));
    }
    std::int64_t previous = 0;
    for (double p : {0.0, 1.0, 10.0, 25.0, 50.0, 75.0, 90.0, 99.0, 99.9, 99.99, 100.0}) {
        const std::int64_t v = h.percentile(p);
        REQUIRE(v >= previous);
        previous = v;
    }
}

TEST_CASE("LatencyHistogram: bucketed percentiles track the exact distribution within "
          "the log region's bounded relative error",
          "[histogram]") {
    LatencyHistogram h;
    std::vector<std::int64_t> samples;
    std::mt19937_64 rng(2026);
    // Spans several octaves above the linear region (8192..~2M ns) so
    // this genuinely exercises the log-bucketed path, not the exact one.
    std::uniform_int_distribution<std::int64_t> dist(10'000, 2'000'000);
    for (int i = 0; i < 50'000; ++i) {
        const std::int64_t v = dist(rng);
        h.record(v);
        samples.push_back(v);
    }

    for (double p : {50.0, 90.0, 99.0, 99.9}) {
        const std::int64_t approx = h.percentile(p);
        const std::int64_t exact = exact_percentile(samples, p);
        const double relative_error =
            std::abs(static_cast<double>(approx - exact)) / static_cast<double>(exact);
        INFO("p" << p << ": approx=" << approx << " exact=" << exact
                 << " relative_error=" << relative_error);
        REQUIRE(relative_error < 0.05);  // bucket width is ~1.6% of value; 5% leaves slack
    }
}

TEST_CASE("LatencyHistogram: values at and around the linear/log boundary don't misbehave",
          "[histogram]") {
    LatencyHistogram h;
    h.record(LatencyHistogram::kLinearRangeNs - 1);  // last linear-region value
    h.record(LatencyHistogram::kLinearRangeNs);      // first log-region value
    h.record(LatencyHistogram::kLinearRangeNs + 1);
    REQUIRE(h.count() == 3);
    REQUIRE(h.min() == LatencyHistogram::kLinearRangeNs - 1);
    REQUIRE(h.max() == LatencyHistogram::kLinearRangeNs + 1);
}

TEST_CASE("LatencyHistogram: a huge outlier is recorded (exact min/max) without crashing or "
          "corrupting other buckets",
          "[histogram]") {
    LatencyHistogram h;
    for (std::int64_t v = 1; v <= 100; ++v) {
        h.record(v);
    }
    h.record(1'000'000'000'000LL);  // ~16.6 minutes in ns — far beyond realistic latencies
    REQUIRE(h.count() == 101);
    REQUIRE(h.max() == 1'000'000'000'000LL);
    REQUIRE(h.min() == 1);
    // The bulk of the distribution (the 100 small values) must still
    // dominate the low percentiles — one outlier shouldn't drag p50 up.
    REQUIRE(h.percentile(50) <= 100);
}

TEST_CASE("LatencyHistogram: negative input is clamped to zero, not UB", "[histogram]") {
    LatencyHistogram h;
    h.record(-500);
    REQUIRE(h.count() == 1);
    REQUIRE(h.min() == 0);
    REQUIRE(h.max() == 0);
}

TEST_CASE("ThroughputCounter: ops_per_second matches count/elapsed", "[metrics]") {
    jane::metrics::ThroughputCounter counter(jane::Nanos{0});
    for (std::uint64_t i = 1; i <= 1000; ++i) {
        counter.record(jane::Nanos{static_cast<std::int64_t>(i) * 1'000'000});  // 1ms apart
    }
    REQUIRE(counter.count() == 1000);
    // 1000 ops over 1000ms (1 second) = 1000 ops/sec.
    REQUIRE(counter.ops_per_second() > 990.0);
    REQUIRE(counter.ops_per_second() < 1010.0);
}

TEST_CASE("ThroughputCounter: zero elapsed time reports zero rather than dividing by zero",
          "[metrics]") {
    jane::metrics::ThroughputCounter counter(jane::Nanos{100});
    counter.record(jane::Nanos{100}, 5);  // same timestamp as start
    REQUIRE(counter.count() == 5);
    REQUIRE(counter.ops_per_second() == 0.0);
}
