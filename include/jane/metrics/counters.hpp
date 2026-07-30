#pragma once

#include <cstdint>

#include "jane/core/types.hpp"

// A throughput counter needs both a count and a time basis — this pairs
// them rather than leaving the caller to compute ops/sec from two
// separately-tracked numbers by hand. Not thread-safe (plain uint64_t,
// not atomic): one counter per thread/component, same as everything else
// in this codebase avoids cross-thread sharing rather than paying for
// synchronization it doesn't need — see jane::concurrency for where
// cross-thread handoff actually happens.
namespace jane::metrics {

class ThroughputCounter {
public:
    explicit ThroughputCounter(Nanos start_time = Nanos{0}) noexcept
        : start_(start_time), last_(start_time) {}

    void record(Nanos now, std::uint64_t n = 1) noexcept {
        count_ += n;
        last_ = now;
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

    [[nodiscard]] double ops_per_second() const noexcept {
        const std::int64_t elapsed_ns = (last_ - start_).value();
        if (elapsed_ns <= 0) {
            return 0.0;
        }
        return static_cast<double>(count_) * 1e9 / static_cast<double>(elapsed_ns);
    }

    void reset(Nanos now) noexcept {
        count_ = 0;
        start_ = now;
        last_ = now;
    }

private:
    Nanos start_;
    Nanos last_;
    std::uint64_t count_ = 0;
};

}  // namespace jane::metrics
