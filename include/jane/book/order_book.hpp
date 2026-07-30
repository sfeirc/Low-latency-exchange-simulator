#pragma once

#include <ankerl/unordered_dense.h>

#include <array>
#include <cstddef>
#include <expected>
#include <optional>

#include "jane/book/bitmap.hpp"
#include "jane/book/order_node.hpp"
#include "jane/book/price_level.hpp"
#include "jane/core/order.hpp"
#include "jane/core/types.hpp"
#include "jane/memory/slab_pool.hpp"

namespace jane::book {

enum class AddError { PriceOutOfRange, DuplicateOrderId, PoolExhausted, InvalidQuantity, MarketOrderCannotRest };
enum class CancelError { NotFound };
enum class ReduceError { NotFound, InvalidFillAmount };
enum class ReplaceError { NotFound, PriceOutOfRange, InvalidQuantity, PoolExhausted };

// One side (bids or asks) of the price ladder: NumLevels price slots plus
// a Bitmap tracking which are occupied, giving O(NumLevels/64)-worst-case
// (O(1) amortized in practice — see docs/tradeoffs.md) relocation of the
// best price when it's vacated, instead of an O(NumLevels) linear scan.
//
// IsBid controls the one thing that differs between the two sides: which
// direction is "better". Bids and asks are otherwise identical code, so
// this is a compile-time bool rather than two hand-duplicated classes.
template <std::size_t NumLevels, bool IsBid>
class LadderSide {
public:
    LadderSide() : occupied_(NumLevels) {}

    [[nodiscard]] PriceLevel& level(std::size_t index) noexcept { return levels_[index]; }
    [[nodiscard]] const PriceLevel& level(std::size_t index) const noexcept { return levels_[index]; }

    // Call after pushing the first order into a level (i.e. the level was
    // empty immediately before this push).
    void note_nonempty(std::size_t index) noexcept {
        occupied_.set(index);
        if (!best_index_.has_value() || is_better(index, *best_index_)) {
            best_index_ = index;
        }
    }

    // Call after removing an order from a level that may now be empty.
    void note_maybe_empty(std::size_t index) noexcept {
        if (!levels_[index].empty()) {
            return;
        }
        occupied_.clear(index);
        if (best_index_.has_value() && *best_index_ == index) {
            if constexpr (IsBid) {
                best_index_ =
                    (index == 0) ? std::nullopt : occupied_.find_highest_at_or_below(index - 1);
            } else {
                best_index_ = occupied_.find_lowest_at_or_above(index + 1);
            }
        }
    }

    [[nodiscard]] std::optional<std::size_t> best_index() const noexcept { return best_index_; }

    // Read-only: the next-best occupied index strictly worse than `index`,
    // if any. Does not require `index` itself to be the current best or
    // even occupied — used to walk the whole side in priority order (see
    // OrderBook::for_each_level), not just to relocate after a vacate.
    [[nodiscard]] std::optional<std::size_t> next_after(std::size_t index) const noexcept {
        if constexpr (IsBid) {
            return (index == 0) ? std::nullopt : occupied_.find_highest_at_or_below(index - 1);
        } else {
            return occupied_.find_lowest_at_or_above(index + 1);
        }
    }

private:
    [[nodiscard]] static constexpr bool is_better(std::size_t a, std::size_t b) noexcept {
        if constexpr (IsBid) {
            return a > b;
        } else {
            return a < b;
        }
    }

    std::array<PriceLevel, NumLevels> levels_{};
    Bitmap occupied_;
    std::optional<std::size_t> best_index_;
};

// A single symbol's limit order book: price/time priority, O(1) add
// (amortized — see LadderSide), O(1) cancel/replace lookup via a flat
// hash map, O(NumLevels/64) worst-case best-price relocation.
//
// This class only stores and organizes resting orders — it does not
// decide whether an incoming order crosses the spread or generate trades;
// that's jane::matching::MatchingEngine, built on top of the primitives
// here (add/cancel/reduce_quantity/front_at). Keeping "what's resting"
// separate from "what happens when a new order arrives" is what makes the
// correctness invariants in docs/correctness.md checkable in isolation.
//
// NumLevels prices are representable, starting at base_price: an order
// priced outside [base_price, base_price + NumLevels) is rejected with
// AddError::PriceOutOfRange rather than growing the ladder — pick
// NumLevels generously for the instrument's realistic trading range. A
// real venue would pair this with exactly the kind of price-collar risk
// check jane::risk also implements, not patch over it with dynamic
// resizing.
template <std::size_t NumLevels, std::size_t MaxOrders>
class OrderBook {
public:
    OrderBook(SymbolId symbol, Price base_price) noexcept : symbol_(symbol), base_price_(base_price) {
        order_index_.reserve(MaxOrders);
    }
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // `order` must already carry its final sequence/timestamp (assigned by
    // the caller's sequencer) — the book never touches a clock, so replay
    // is deterministic purely by replaying the same input sequence.
    [[nodiscard]] std::expected<OrderId, AddError> add(const Order& order) noexcept {
        if (order.type != OrderType::Limit) {
            return std::unexpected(AddError::MarketOrderCannotRest);
        }
        if (order.remaining.value() <= 0) {
            return std::unexpected(AddError::InvalidQuantity);
        }
        const auto index_opt = index_of(order.price);
        if (!index_opt) {
            return std::unexpected(AddError::PriceOutOfRange);
        }
        if (order_index_.contains(order.id)) {
            return std::unexpected(AddError::DuplicateOrderId);
        }
        OrderNode* node = pool_.allocate();
        if (node == nullptr) {
            return std::unexpected(AddError::PoolExhausted);
        }

        const std::size_t index = *index_opt;
        node->order = order;
        node->level_index = index;

        PriceLevel& lvl = level_for(order.side, index);
        const bool was_empty = lvl.empty();
        level_push_back(lvl, node);
        if (was_empty) {
            note_nonempty_for(order.side, index);
        }

        order_index_.emplace(order.id, node);
        ++order_count_;
        return order.id;
    }

    [[nodiscard]] std::expected<void, CancelError> cancel(OrderId id) noexcept {
        auto it = order_index_.find(id);
        if (it == order_index_.end()) {
            return std::unexpected(CancelError::NotFound);
        }
        OrderNode* node = it->second;
        unlink_and_free(node);
        order_index_.erase(it);
        --order_count_;
        return {};
    }

    // Records a fill against a resting order: shrinks `remaining` without
    // moving it within its level's FIFO (a partial fill keeps whatever
    // time priority is left — see docs/correctness.md), and fully removes
    // it once remaining reaches zero.
    [[nodiscard]] std::expected<void, ReduceError> reduce_quantity(OrderId id,
                                                                     Quantity fill_amount) noexcept {
        auto it = order_index_.find(id);
        if (it == order_index_.end()) {
            return std::unexpected(ReduceError::NotFound);
        }
        OrderNode* node = it->second;
        if (fill_amount.value() <= 0 || fill_amount.value() > node->order.remaining.value()) {
            return std::unexpected(ReduceError::InvalidFillAmount);
        }

        const std::size_t index = node->level_index;
        const Side node_side = node->order.side;
        PriceLevel& lvl = level_for(node_side, index);

        node->order.remaining -= fill_amount;
        if (node->order.remaining.value() == 0) {
            level_unlink(lvl, node);  // subtracts the now-zero remaining; no double count with below
            note_maybe_empty_for(node_side, index);
            order_index_.erase(it);
            pool_.deallocate(node);
            --order_count_;
        } else {
            lvl.total_quantity -= fill_amount;
        }
        return {};
    }

    // Same OrderId, new price/quantity; always re-queued at the back of
    // its (possibly new) level — a replace loses time priority, matching
    // standard exchange semantics (see docs/correctness.md). new_sequence
    // / new_timestamp are supplied by the caller for the same determinism
    // reason as add().
    //
    // Deliberately does NOT check whether the new price crosses the
    // opposite side — this is a pure book-storage operation with no
    // notion of matching, same as add(). A replace whose new price is
    // marketable must go through jane::matching::MatchingEngine::replace()
    // instead, which cancels here and re-submits through the same
    // crossing-aware path a brand new order takes; calling this directly
    // with a marketable price would rest a crossed book, which is exactly
    // the invariant docs/correctness.md checks for.
    [[nodiscard]] std::expected<void, ReplaceError> replace(OrderId id, Price new_price,
                                                              Quantity new_quantity,
                                                              Sequence new_sequence,
                                                              Nanos new_timestamp) noexcept {
        auto it = order_index_.find(id);
        if (it == order_index_.end()) {
            return std::unexpected(ReplaceError::NotFound);
        }
        if (new_quantity.value() <= 0) {
            return std::unexpected(ReplaceError::InvalidQuantity);
        }
        if (!index_of(new_price).has_value()) {
            return std::unexpected(ReplaceError::PriceOutOfRange);
        }

        OrderNode* node = it->second;
        Order updated = node->order;
        updated.price = new_price;
        updated.quantity = new_quantity;
        updated.remaining = new_quantity;
        updated.sequence = new_sequence;
        updated.timestamp = new_timestamp;

        unlink_and_free(node);
        order_index_.erase(it);
        --order_count_;

        // By construction this cannot fail: price/quantity were validated
        // above, `id` was just erased so it can't collide, and `updated`
        // inherits `type == Limit` from the node we just removed (only
        // Limit orders are ever accepted by add() in the first place).
        // Kept as a returned error rather than an assert anyway — an
        // exchange silently losing an order because of a reasoning
        // mistake here would be a far worse failure mode than a defensive
        // branch that never triggers.
        auto result = add(updated);
        if (!result.has_value()) {
            return std::unexpected(ReplaceError::PoolExhausted);
        }
        return {};
    }

    [[nodiscard]] std::optional<Price> best_bid() const noexcept { return best_price(bids_); }
    [[nodiscard]] std::optional<Price> best_ask() const noexcept { return best_price(asks_); }

    [[nodiscard]] const PriceLevel* level_at(Side side, Price price) const noexcept {
        const auto index_opt = index_of(price);
        if (!index_opt) {
            return nullptr;
        }
        return &level_for(side, *index_opt);
    }

    // Head of the FIFO at (side, price) — the next order matching would
    // consume. Returns a const pointer deliberately: mutating a resting
    // order must go through cancel()/reduce_quantity() so the ladder's
    // aggregates and occupancy bitmap stay consistent.
    [[nodiscard]] const OrderNode* front_at(Side side, Price price) const noexcept {
        const auto index_opt = index_of(price);
        if (!index_opt) {
            return nullptr;
        }
        return level_for(side, *index_opt).head;
    }

    // Visits occupied levels for `side` from best to worst, calling
    // visit(price, level) for each; stops early the first time visit
    // returns false. Read-only traversal — used by
    // jane::matching::MatchingEngine's fill-or-kill pre-check (sum
    // available quantity without mutating anything) and by
    // jane::marketdata's snapshot builder (top-N depth).
    template <typename Visitor>
    void for_each_level(Side side, Visitor&& visit) const {
        std::optional<std::size_t> idx = (side == Side::Buy) ? bids_.best_index() : asks_.best_index();
        while (idx.has_value()) {
            const PriceLevel& lvl = level_for(side, *idx);
            if (!visit(price_at(*idx), lvl)) {
                return;
            }
            idx = (side == Side::Buy) ? bids_.next_after(*idx) : asks_.next_after(*idx);
        }
    }

    [[nodiscard]] const OrderNode* find(OrderId id) const noexcept {
        auto it = order_index_.find(id);
        return it == order_index_.end() ? nullptr : it->second;
    }

    [[nodiscard]] std::size_t order_count() const noexcept { return order_count_; }
    [[nodiscard]] SymbolId symbol() const noexcept { return symbol_; }
    [[nodiscard]] Price base_price() const noexcept { return base_price_; }
    [[nodiscard]] static constexpr std::size_t num_levels() noexcept { return NumLevels; }
    [[nodiscard]] static constexpr std::size_t max_orders() noexcept { return MaxOrders; }

    [[nodiscard]] std::optional<std::size_t> index_of(Price p) const noexcept {
        const auto offset = (p - base_price_).value();
        if (offset < 0 || static_cast<std::uint64_t>(offset) >= NumLevels) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(offset);
    }
    [[nodiscard]] Price price_at(std::size_t index) const noexcept {
        return base_price_ + Price{static_cast<std::int64_t>(index)};
    }

private:
    using Bids = LadderSide<NumLevels, true>;
    using Asks = LadderSide<NumLevels, false>;

    // Bids and Asks are different C++ types (LadderSide<N,true> vs
    // LadderSide<N,false>), so a function returning "whichever side
    // matches this runtime Side" can't return a reference to either
    // templated type directly. PriceLevel itself isn't templated on
    // IsBid, so dispatching down to *that* common type works fine; the
    // few operations that must reach the LadderSide-level API
    // (note_nonempty/note_maybe_empty) get their own small dispatchers
    // instead of trying to force a shared reference type that doesn't
    // exist.
    [[nodiscard]] PriceLevel& level_for(Side s, std::size_t index) noexcept {
        return s == Side::Buy ? bids_.level(index) : asks_.level(index);
    }
    [[nodiscard]] const PriceLevel& level_for(Side s, std::size_t index) const noexcept {
        return s == Side::Buy ? bids_.level(index) : asks_.level(index);
    }
    void note_nonempty_for(Side s, std::size_t index) noexcept {
        if (s == Side::Buy) {
            bids_.note_nonempty(index);
        } else {
            asks_.note_nonempty(index);
        }
    }
    void note_maybe_empty_for(Side s, std::size_t index) noexcept {
        if (s == Side::Buy) {
            bids_.note_maybe_empty(index);
        } else {
            asks_.note_maybe_empty(index);
        }
    }

    void unlink_and_free(OrderNode* node) noexcept {
        const std::size_t index = node->level_index;
        level_unlink(level_for(node->order.side, index), node);
        note_maybe_empty_for(node->order.side, index);
        pool_.deallocate(node);
    }

    template <typename LadderSideT>
    [[nodiscard]] std::optional<Price> best_price(const LadderSideT& side) const noexcept {
        const auto idx = side.best_index();
        return idx.has_value() ? std::optional<Price>(price_at(*idx)) : std::nullopt;
    }

    SymbolId symbol_;
    Price base_price_;
    Bids bids_;
    Asks asks_;
    SlabPool<OrderNode, MaxOrders> pool_;
    ankerl::unordered_dense::map<OrderId, OrderNode*> order_index_;
    std::size_t order_count_ = 0;
};

}  // namespace jane::book
