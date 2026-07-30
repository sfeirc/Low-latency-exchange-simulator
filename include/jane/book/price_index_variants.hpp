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
        if (!best_.has_value() || idx > *best_) {
            best_ = idx;
        }
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
