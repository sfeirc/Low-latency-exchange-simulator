#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

// A fixed-memory, allocation-free latency histogram covering nanoseconds
// through days in one structure, at roughly constant *relative*
// resolution (not constant absolute resolution — a linear-bucket
// histogram fine enough to resolve nanosecond ring-buffer latencies would
// need an unusable number of buckets to also cover a millisecond outlier;
// this is the well-known HDR-histogram-style trade-off, reimplemented
// here in a simplified form rather than taken as a dependency).
//
// Two regions:
//   - [0, kLinearRangeNs): exact, one bucket per nanosecond. Every
//     latency this project actually measures on the hot path (order add,
//     match, protocol decode — all sub-microsecond per bench/) falls
//     entirely inside this exact region.
//   - [kLinearRangeNs, ...): log2 "octaves" (power-of-two ranges), each
//     subdivided into kSubBucketsPerOctave equal-width linear buckets —
//     bounded relative error (~1/kSubBucketsPerOctave, ~1.6% here)
//     regardless of how large the value is. This only matters for
//     capturing tail outliers (a GC pause, a page fault, a scheduler
//     preemption) that this project's own operations don't produce in
//     normal operation but a benchmark harness should still be able to
//     report accurately if one occurs.
namespace jane::metrics {

class LatencyHistogram {
public:
    static constexpr std::int64_t kLinearRangeNs = 8192;  // 2^13, exact below this
    static constexpr int kLinearRangeBits = 13;
    static constexpr int kSubBucketsPerOctave = 64;
    static constexpr int kMaxOctaves = 40;  // headroom to ~2^53 ns (~104 days) before clamping
    static constexpr std::size_t kNumBuckets =
        static_cast<std::size_t>(kLinearRangeNs) +
        static_cast<std::size_t>(kSubBucketsPerOctave) * static_cast<std::size_t>(kMaxOctaves);

    void record(std::int64_t nanos) noexcept {
        if (nanos < 0) {
            nanos = 0;
        }
        const std::size_t idx = std::min(bucket_index(nanos), kNumBuckets - 1);
        ++counts_[idx];
        min_ = (count_ == 0) ? nanos : std::min(min_, nanos);
        max_ = std::max(max_, nanos);
        sum_ += nanos;
        ++count_;
    }

    // p in [0, 100]. Exact at the extremes (min_/max_ are tracked
    // precisely, not bucket-quantized); bucketed in between.
    [[nodiscard]] std::int64_t percentile(double p) const noexcept {
        if (count_ == 0) {
            return 0;
        }
        if (p <= 0.0) {
            return min_;
        }
        if (p >= 100.0) {
            return max_;
        }
        // The tiny epsilon subtraction guards against p/100.0*count landing
        // a few ULPs above a whole number for "round" inputs like
        // p=99.9, count=1000 (mathematically exactly 999, but 99.9 isn't
        // exactly representable in binary floating point) — without it,
        // ceil() bumps the target up by one and the reported percentile
        // is off by one bucket for exactly the round-number cases a
        // caller is most likely to sanity-check by hand.
        const auto target = static_cast<std::uint64_t>(
            std::ceil(p / 100.0 * static_cast<double>(count_) - 1e-9));
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < kNumBuckets; ++i) {
            cumulative += counts_[i];
            if (cumulative >= target) {
                return bucket_lower_bound(i);
            }
        }
        return max_;
    }

    [[nodiscard]] std::int64_t min() const noexcept { return count_ ? min_ : 0; }
    [[nodiscard]] std::int64_t max() const noexcept { return max_; }
    [[nodiscard]] double mean() const noexcept {
        return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0;
    }
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

private:
    [[nodiscard]] static std::size_t bucket_index(std::int64_t nanos) noexcept {
        if (nanos < kLinearRangeNs) {
            return static_cast<std::size_t>(nanos);
        }
        const auto v = static_cast<std::uint64_t>(nanos);
        const int msb = 63 - std::countl_zero(v);
        const auto octave_start = std::uint64_t{1} << msb;
        const auto octave_number = static_cast<std::size_t>(msb - kLinearRangeBits);
        const std::uint64_t sub_width = octave_start / static_cast<std::uint64_t>(kSubBucketsPerOctave);
        const std::uint64_t sub_index = (v - octave_start) / sub_width;
        return static_cast<std::size_t>(kLinearRangeNs) +
               octave_number * static_cast<std::size_t>(kSubBucketsPerOctave) +
               static_cast<std::size_t>(sub_index);
    }

    [[nodiscard]] static std::int64_t bucket_lower_bound(std::size_t idx) noexcept {
        if (idx < static_cast<std::size_t>(kLinearRangeNs)) {
            return static_cast<std::int64_t>(idx);
        }
        const std::size_t log_idx = idx - static_cast<std::size_t>(kLinearRangeNs);
        const std::size_t octave_number = log_idx / static_cast<std::size_t>(kSubBucketsPerOctave);
        const std::size_t sub_index = log_idx % static_cast<std::size_t>(kSubBucketsPerOctave);
        const int msb = kLinearRangeBits + static_cast<int>(octave_number);
        const auto octave_start = std::uint64_t{1} << msb;
        const std::uint64_t sub_width = octave_start / static_cast<std::uint64_t>(kSubBucketsPerOctave);
        return static_cast<std::int64_t>(octave_start + sub_index * sub_width);
    }

    std::array<std::uint64_t, kNumBuckets> counts_{};
    std::uint64_t count_ = 0;
    std::int64_t sum_ = 0;
    std::int64_t min_ = 0;
    std::int64_t max_ = 0;
};

}  // namespace jane::metrics
