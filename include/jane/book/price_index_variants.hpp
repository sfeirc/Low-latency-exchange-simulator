#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <vector>

#include "jane/book/bitmap.hpp"

// Three structures answering the exact same question — "which integer
// prices are currently occupied, and what's the highest (best) one?" —
// isolating the *one* dimension that genuinely differs between possible
// order book designs: how price levels are indexed. FIFO-per-level
// management (PriceLevel/OrderNode) and allocation (SlabPool) are shared
// by every design and already benchmarked in bench_order_book.cpp /
// bench_memory_pool.cpp, so mixing them into this comparison would muddy
// which variable actually explains a performance difference.
//
// None of these are used by jane::book::OrderBook — that's
// LadderIndexVariant's job in production, wearing the name LadderSide (it
// carries the real occupied-price bitmap logic inline rather than through
// this shared comparison harness). These three exist purely to feed
// bench/bench_order_book_backends.cpp and docs/tradeoffs.md with real
// numbers instead of an assumption about which is fastest.
namespace jane::book::variants {

// Mirrors the bitmap/bit-scan design OrderBook's LadderSide actually uses.
class LadderIndexVariant {
public:
    explicit LadderIndexVariant(std::size_t num_prices) : occupied_(num_prices) {}

    void insert(std::int64_t p) {
        const auto idx = static_cast<std::size_t>(p);
        occupied_.set(idx);
        // GCC -Wmaybe-uninitialized false-positives on the `*best_` read
        // below, but only once insert() gets inlined into a heavily
        // templated call site (bench_order_book_backends.cpp's churn
        // benchmark, instantiated per BookDepth) under -O3 (RelWithDebInfo
        // in CI, see .github/workflows/ci.yml — Debug builds never hit
        // this). `best_` is a std::optional<std::size_t>: has_value() is
        // checked first and short-circuits before `*best_` is ever
        // evaluated, and it is never left disengaged-but-read — it has a
        // `= std::nullopt` default member initializer below and is only
        // ever otherwise written via `best_ = idx` / `best_ = std::nullopt`
        // in erase(). Tried both that NSDMI and an explicit
        // mem-initializer-list write (`best_(std::nullopt)` in the
        // constructor) as real fixes; neither changes the warning, which
        // confirms it's GCC's optimizer losing track of the engaged-flag
        // through the inlined std::optional machinery, not a real gap —
        // same false-positive class already hit (and pragma-suppressed)
        // for -Wnull-dereference in strong_type.hpp. Suppressed narrowly
        // at this exact read rather than project-wide, where
        // -Wmaybe-uninitialized is a genuinely useful check.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
        if (!best_.has_value() || idx > *best_) {
            best_ = idx;
        }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    }
    void erase(std::int64_t p) {
        const auto idx = static_cast<std::size_t>(p);
        occupied_.clear(idx);
        if (best_.has_value() && *best_ == idx) {
            best_ = (idx == 0) ? std::nullopt : occupied_.find_highest_at_or_below(idx - 1);
        }
    }
    [[nodiscard]] std::optional<std::int64_t> best() const noexcept {
        return best_.has_value() ? std::optional<std::int64_t>(static_cast<std::int64_t>(*best_))
                                  : std::nullopt;
    }

private:
    Bitmap occupied_;
    std::optional<std::size_t> best_ = std::nullopt;
};

// std::set<Price>: a red-black tree, the standard "ordered associative
// container" baseline. rbegin() gives the max.
class TreeIndexVariant {
public:
    void insert(std::int64_t p) { set_.insert(p); }
    void erase(std::int64_t p) { set_.erase(p); }
    [[nodiscard]] std::optional<std::int64_t> best() const noexcept {
        return set_.empty() ? std::nullopt : std::optional<std::int64_t>(*set_.rbegin());
    }

private:
    std::set<std::int64_t> set_;
};

// A sorted std::vector, boost::flat_map-style: O(log n) lookup via binary
// search but O(n) insert/erase (element shift) — the classic "trade insert
// cost for cache-friendly, contiguous storage" structure. best() is
// always O(1) (just the back of the vector).
class FlatIndexVariant {
public:
    void insert(std::int64_t p) {
        auto it = std::lower_bound(v_.begin(), v_.end(), p);
        if (it == v_.end() || *it != p) {
            v_.insert(it, p);
        }
    }
    void erase(std::int64_t p) {
        auto it = std::lower_bound(v_.begin(), v_.end(), p);
        if (it != v_.end() && *it == p) {
            v_.erase(it);
        }
    }
    [[nodiscard]] std::optional<std::int64_t> best() const noexcept {
        return v_.empty() ? std::nullopt : std::optional<std::int64_t>(v_.back());
    }

private:
    std::vector<std::int64_t> v_;  // kept sorted ascending
};

}  // namespace jane::book::variants
