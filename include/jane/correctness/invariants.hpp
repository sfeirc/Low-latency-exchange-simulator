#pragma once

#include <array>
#include <string>
#include <vector>

#include "jane/core/types.hpp"

// A read-only structural audit of a live order book — safe to run after
// every operation in a test or fuzzer without perturbing anything it
// checks. Each violation string names exactly which invariant broke; see
// docs/correctness.md for what each one means and why it's load-bearing
// (a book that fails any of these can no longer be trusted to match
// orders correctly, even if it doesn't crash).
namespace jane::correctness {

template <typename Book>
[[nodiscard]] std::vector<std::string> check_book_invariants(const Book& book) {
    std::vector<std::string> violations;

    // Never crossed: the best bid must never be at or above the best ask.
    // A crossed book means there was executable liquidity matching didn't
    // execute — either a missed trade or, worse, two resting orders that
    // should never have coexisted.
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    if (bid.has_value() && ask.has_value() && bid->value() >= ask->value()) {
        violations.push_back("crossed book: best_bid=" + std::to_string(bid->value()) +
                              " >= best_ask=" + std::to_string(ask->value()));
    }

    for (const Side side : {Side::Buy, Side::Sell}) {
        book.for_each_level(side, [&](Price price, const auto& level) {
            Quantity summed{0};
            std::uint32_t counted = 0;
            using NodePtr = decltype(level.head);
            NodePtr prev = nullptr;

            for (NodePtr node = level.head; node != nullptr; node = node->next) {
                // Every resting order carries positive open quantity — a
                // fill or cancel that reaches zero must remove the node,
                // never leave a zero-or-negative husk behind.
                if (node->order.remaining.value() <= 0) {
                    violations.push_back("non-positive remaining quantity (" +
                                          std::to_string(node->order.remaining.value()) +
                                          ") resting at price " + std::to_string(price.value()));
                }
                // Every node in this side's level must actually belong to
                // this side — catches a node linked into the wrong ladder.
                if (node->order.side != side) {
                    violations.push_back("order resting on the wrong side's level at price " +
                                          std::to_string(price.value()));
                }
                // The doubly-linked FIFO must be internally consistent in
                // both directions, not just walkable forward.
                if (node->prev != prev) {
                    violations.push_back("broken FIFO back-link at price " +
                                          std::to_string(price.value()));
                }
                summed += node->order.remaining;
                ++counted;
                prev = node;
            }
            if (level.tail != prev) {
                violations.push_back("level.tail inconsistent with the FIFO walk's end at price " +
                                      std::to_string(price.value()));
            }
            // The level's cached aggregates (used by market-data snapshots
            // and risk sizing without walking the whole FIFO) must equal
            // what the FIFO itself actually contains — this is the
            // "conservation of volume" check at the single-level scope.
            if (summed.value() != level.total_quantity.value()) {
                violations.push_back(
                    "level.total_quantity=" + std::to_string(level.total_quantity.value()) +
                    " != sum of resting orders=" + std::to_string(summed.value()) + " at price " +
                    std::to_string(price.value()));
            }
            if (counted != level.order_count) {
                violations.push_back("level.order_count=" + std::to_string(level.order_count) +
                                      " != actual FIFO length=" + std::to_string(counted) +
                                      " at price " + std::to_string(price.value()));
            }
            return true;  // keep visiting every level, don't stop at the first
        });
    }

    return violations;
}

}  // namespace jane::correctness
