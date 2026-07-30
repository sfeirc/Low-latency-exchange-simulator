#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

#include "jane/book/order_book.hpp"
#include "jane/core/order.hpp"
#include "jane/core/types.hpp"

// Decides whether an incoming order crosses the book and, if so, executes
// trades against resting liquidity — the layer jane::book::OrderBook
// deliberately does not implement (see the note on OrderBook::replace()).
// Everything here assumes single-threaded, sequenced input: exactly one
// thread ever calls submit/cancel/replace on a given MatchingEngine,
// which is what makes several "cannot happen" arguments below actually
// true rather than hopeful.
namespace jane::matching {

// One matched quantity between an incoming (aggressor) order and a
// resting (passive) order. Trade price is always the resting order's
// price — the aggressor gets price improvement whenever their limit was
// better than what they actually paid, never price degradation.
struct Fill {
    OrderId resting_order_id;
    ClientId resting_client_id;
    OrderId aggressor_order_id;
    ClientId aggressor_client_id;
    SymbolId symbol;
    Price price;
    Quantity quantity;
    Side aggressor_side;
    bool resting_fully_filled;
};

struct NewOrderResult {
    RejectReason reject_reason = RejectReason::None;  // None => accepted (see fill_count/rested below)
    std::size_t fill_count = 0;   // how many entries THIS call appended to the caller's out_fills
    bool rested = false;          // a remainder was added to the book
    Quantity remaining_after{0};  // 0 if fully filled; also 0 if fully rejected
};

struct CancelResult {
    bool accepted = false;
};

struct ReplaceResult {
    bool accepted = false;
    RejectReason reject_reason = RejectReason::None;
    std::size_t fill_count = 0;  // the replaced order may immediately (partially) match at its new terms
    bool rested = false;
    Quantity remaining_after{0};
};

template <std::size_t NumLevels, std::size_t MaxOrders>
class MatchingEngine {
public:
    using Book = book::OrderBook<NumLevels, MaxOrders>;

    MatchingEngine(SymbolId symbol, Price base_price) noexcept : book_(symbol, base_price) {}
    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    [[nodiscard]] const Book& book() const noexcept { return book_; }

    // `order` must be freshly formed (order.remaining == order.quantity)
    // and already carry its sequencer-assigned sequence/timestamp — same
    // determinism contract as OrderBook::add(). Fills are *appended* to
    // `out_fills` (never cleared first) so a caller processing a batch of
    // sequenced events can pass the same reused vector across many calls
    // — clear() between calls is the caller's choice, not this function's,
    // which is what keeps this whole path allocation-free once out_fills
    // has grown to its steady-state capacity (the same "let the caller
    // reuse a buffer" pattern SPSCRingBuffer's caller uses for message
    // storage, applied to a variable-length result instead of a fixed one).
    [[nodiscard]] NewOrderResult submit(Order order, std::vector<Fill>& out_fills) noexcept {
        NewOrderResult result;
        // Set up front so every early-return path (reject before any
        // matching is attempted) reports "nothing filled, all of it still
        // outstanding" instead of the struct's default-constructed 0 —
        // caught by a FOK-rejection test expecting exactly this.
        result.remaining_after = order.quantity;

        if (order.symbol != book_.symbol()) {
            result.reject_reason = RejectReason::UnknownSymbol;
            return result;
        }
        if (order.quantity.value() <= 0) {
            result.reject_reason = RejectReason::InvalidQuantity;
            return result;
        }
        order.remaining = order.quantity;

        if (order.tif == TimeInForce::FOK) {
            const bool is_market = (order.type == OrderType::Market);
            const Quantity available =
                available_liquidity(order.side, is_market, order.price, order.quantity);
            if (available.value() < order.quantity.value()) {
                result.reject_reason = RejectReason::FokNotFillable;
                return result;
            }
        }

        const std::size_t before = out_fills.size();
        match_against_book(order, out_fills);
        result.fill_count = out_fills.size() - before;

        if (order.remaining.value() > 0 && order.type == OrderType::Limit &&
            order.tif == TimeInForce::Day) {
            auto add_result = book_.add(order);
            if (add_result.has_value()) {
                result.rested = true;
            } else {
                // Only reachable if the book's order pool is exhausted —
                // price range and duplicate-id were never in question for
                // a brand new order the caller just sequenced. The fills
                // already executed above stand regardless: an aggressor
                // that partially filled and then found the book full for
                // its remainder is a capacity problem, not a reason to
                // (impossibly) unwind trades that already happened.
                result.reject_reason = RejectReason::BookCapacityExceeded;
            }
        }
        // Market orders, and any non-Day remainder, are simply dropped —
        // never rest, matching this project's documented order-type
        // semantics (see jane::Order / core/types.hpp).

        result.remaining_after = order.remaining;
        return result;
    }

    [[nodiscard]] CancelResult cancel(OrderId id) noexcept {
        return CancelResult{.accepted = book_.cancel(id).has_value()};
    }

    // Cancels the existing order and re-submits its new terms through the
    // exact same crossing-aware path submit() uses — a replace that makes
    // an order marketable must be able to execute immediately, the same
    // as a brand new order would. (Only Day-Limit orders can ever be
    // found here in the first place: IOC/FOK never rest, so they can
    // never be the target of a later replace — which is why this doesn't
    // need to special-case TIF the way submit() does.) Fills are appended
    // to `out_fills`, same contract as submit().
    [[nodiscard]] ReplaceResult replace(OrderId id, Price new_price, Quantity new_quantity,
                                         Sequence new_sequence, Nanos new_timestamp,
                                         std::vector<Fill>& out_fills) noexcept {
        ReplaceResult result;
        const book::OrderNode* existing = book_.find(id);
        if (existing == nullptr) {
            result.reject_reason = RejectReason::UnknownOrder;
            return result;
        }
        if (new_quantity.value() <= 0) {
            result.reject_reason = RejectReason::InvalidQuantity;
            return result;
        }
        if (!book_.index_of(new_price).has_value()) {
            result.reject_reason = RejectReason::InvalidPrice;
            return result;
        }

        Order updated = existing->order;
        updated.price = new_price;
        updated.quantity = new_quantity;
        updated.remaining = new_quantity;
        updated.sequence = new_sequence;
        updated.timestamp = new_timestamp;

        // By construction this cannot fail: `id` was just found resting.
        [[maybe_unused]] auto cancel_result = book_.cancel(id);

        NewOrderResult submitted = submit(updated, out_fills);
        result.accepted = (submitted.reject_reason == RejectReason::None);
        result.reject_reason = submitted.reject_reason;
        result.fill_count = submitted.fill_count;
        result.rested = submitted.rested;
        result.remaining_after = submitted.remaining_after;
        return result;
    }

private:
    // Sums resting quantity available to `aggressor_side` at acceptable
    // prices, stopping as soon as it's confirmed to reach `cap` (no need
    // to keep counting once "enough" is proven) or the book side runs
    // out. Does not mutate the book — used for the FOK pre-check, which
    // must know the full order is fillable *before* executing any part of
    // it.
    [[nodiscard]] Quantity available_liquidity(Side aggressor_side, bool is_market,
                                                Price limit_price, Quantity cap) const noexcept {
        const Side opposite_side = opposite(aggressor_side);
        Quantity total{0};
        book_.for_each_level(opposite_side, [&](Price level_price, const book::PriceLevel& lvl) {
            if (!is_market) {
                const bool crosses = (aggressor_side == Side::Buy) ? (limit_price >= level_price)
                                                                    : (limit_price <= level_price);
                if (!crosses) {
                    return false;
                }
            }
            total += lvl.total_quantity;
            return total.value() < cap.value();
        });
        return total;
    }

    // Walks the opposite side best-price-first, consuming resting FIFO
    // orders until `aggressor` is filled, the book stops crossing, or
    // liquidity runs out. Mutates the book (via reduce_quantity) as it
    // goes; `aggressor.remaining` reflects what's left when this returns.
    // Appends to `out_fills` rather than returning a fresh vector — see
    // submit()'s doc comment for why.
    void match_against_book(Order& aggressor, std::vector<Fill>& out_fills) noexcept {
        const Side opposite_side = opposite(aggressor.side);

        while (aggressor.remaining.value() > 0) {
            const std::optional<Price> best =
                (opposite_side == Side::Buy) ? book_.best_bid() : book_.best_ask();
            if (!best.has_value()) {
                break;
            }
            if (aggressor.type == OrderType::Limit) {
                const bool crosses = (aggressor.side == Side::Buy) ? (aggressor.price >= *best)
                                                                    : (aggressor.price <= *best);
                if (!crosses) {
                    break;
                }
            }

            const book::OrderNode* resting = book_.front_at(opposite_side, *best);
            // best.has_value() guarantees a non-empty level, so this is
            // never null — but never dereferenced again after
            // reduce_quantity() below, which may free it.

            const Quantity fill_qty = std::min(aggressor.remaining, resting->order.remaining);
            const bool resting_fully_filled = (fill_qty.value() == resting->order.remaining.value());

            out_fills.push_back(Fill{
                .resting_order_id = resting->order.id,
                .resting_client_id = resting->order.client,
                .aggressor_order_id = aggressor.id,
                .aggressor_client_id = aggressor.client,
                .symbol = aggressor.symbol,
                .price = *best,
                .quantity = fill_qty,
                .aggressor_side = aggressor.side,
                .resting_fully_filled = resting_fully_filled,
            });

            const OrderId resting_id = resting->order.id;  // copy before any mutation below
            [[maybe_unused]] auto reduce_result = book_.reduce_quantity(resting_id, fill_qty);
            // Cannot fail: resting_id was just read from a live node in
            // this book, and fill_qty <= resting->order.remaining by
            // construction (std::min above).

            aggressor.remaining -= fill_qty;
        }
    }

    Book book_;
};

}  // namespace jane::matching
