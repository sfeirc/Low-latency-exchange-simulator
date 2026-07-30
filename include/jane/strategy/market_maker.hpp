#pragma once

#include <optional>

#include "jane/core/types.hpp"
#include "jane/protocol/messages.hpp"

// A passive two-sided quoter: rests a bid and an ask symmetrically around
// the observed mid-price, re-quoting (via replace, not cancel+new — see
// on_tick's doc comment) whenever the mid moves, and backing off a side
// entirely once inventory gets too large on that side. Position/PnL are
// deliberately *not* tracked here — every strategy in jane::strategy
// trades through the same jane::risk::RiskEngine every other client
// does, so its position()/pnl() are already the correct, tested answer;
// duplicating that bookkeeping per-strategy would be exactly the kind of
// parallel-implementation risk this project avoids elsewhere.
namespace jane::strategy {

struct MarketMakerConfig {
    SymbolId symbol;
    ClientId client;
    std::int64_t half_spread_ticks = 5;
    std::int64_t quote_size = 10;
    std::int64_t max_inventory = 500;     // stop quoting a side once |position| would exceed this
    Price initial_reference_price{1000};  // used only before any market exists on either side
};

// What on_tick wants submitted this tick. At most one of {new_order,
// replace} is set per side — a fresh quote uses NewOrder (no resting
// order yet to replace), a moved one uses Replace (re-queues at the new
// price, same as any other replace in this codebase — see
// MatchingEngine::replace's doc comment on why that's crossing-aware).
struct MarketMakerAction {
    std::optional<protocol::NewOrderMessage> new_bid;
    std::optional<protocol::NewOrderMessage> new_ask;
    std::optional<protocol::ReplaceOrderMessage> replace_bid;
    std::optional<protocol::ReplaceOrderMessage> replace_ask;
    std::optional<protocol::CancelOrderMessage> cancel_bid;  // inventory cap reached: pull the quote
    std::optional<protocol::CancelOrderMessage> cancel_ask;
};

class MarketMaker {
public:
    MarketMaker(MarketMakerConfig config, std::uint64_t first_order_id) noexcept
        : config_(config), next_order_id_(first_order_id) {}

    // `inventory` is this strategy's current signed position (from
    // RiskEngine::position) — the caller already has it for the risk
    // check it presumably also runs, no reason for this class to ask for
    // it a second way.
    [[nodiscard]] MarketMakerAction on_tick(std::optional<Price> best_bid,
                                             std::optional<Price> best_ask,
                                             Quantity inventory) noexcept {
        const Price mid = compute_mid(best_bid, best_ask);
        const Price target_bid = mid - Price{config_.half_spread_ticks};
        const Price target_ask = mid + Price{config_.half_spread_ticks};

        MarketMakerAction action;

        const bool bid_allowed = inventory.value() < config_.max_inventory;
        if (bid_allowed) {
            if (!resting_bid_id_.has_value()) {
                action.new_bid = make_new_order(Side::Buy, target_bid);
                resting_bid_id_ = OrderId{action.new_bid->order_id};
                last_quoted_bid_ = target_bid;
            } else if (target_bid != last_quoted_bid_) {
                action.replace_bid = make_replace(*resting_bid_id_, target_bid);
                last_quoted_bid_ = target_bid;
            }
        } else if (resting_bid_id_.has_value()) {
            // Inventory capped: pull the bid rather than leave it resting
            // (a replace to an unreachable price would be a workaround;
            // withdrawing the quote is what a real maker would do). Must
            // actually cancel, not just stop tracking the id — an
            // earlier version of this function did the latter, which
            // left the order resting in the book forever with this
            // class having no way to ever cancel or replace it again.
            action.cancel_bid =
                protocol::CancelOrderMessage{.order_id = resting_bid_id_->value(),
                                              .client_id = config_.client.value(),
                                              .symbol_id = config_.symbol.value()};
            resting_bid_id_.reset();
        }

        const bool ask_allowed = inventory.value() > -config_.max_inventory;
        if (ask_allowed) {
            if (!resting_ask_id_.has_value()) {
                action.new_ask = make_new_order(Side::Sell, target_ask);
                resting_ask_id_ = OrderId{action.new_ask->order_id};
                last_quoted_ask_ = target_ask;
            } else if (target_ask != last_quoted_ask_) {
                action.replace_ask = make_replace(*resting_ask_id_, target_ask);
                last_quoted_ask_ = target_ask;
            }
        } else if (resting_ask_id_.has_value()) {
            action.cancel_ask =
                protocol::CancelOrderMessage{.order_id = resting_ask_id_->value(),
                                              .client_id = config_.client.value(),
                                              .symbol_id = config_.symbol.value()};
            resting_ask_id_.reset();
        }

        return action;
    }

    [[nodiscard]] std::optional<OrderId> resting_bid_id() const noexcept { return resting_bid_id_; }
    [[nodiscard]] std::optional<OrderId> resting_ask_id() const noexcept { return resting_ask_id_; }

    // The caller must call this when it learns (a Fill execution report,
    // or simply observing the order no longer resting) that a tracked
    // order is gone — otherwise the next on_tick() would try to replace
    // an order that no longer exists, get rejected (UnknownOrder), and
    // never fall back to placing a fresh one: this class has no way to
    // discover a fill on its own, since it's never handed execution
    // reports directly (see the class comment on why PnL/fills flow
    // through RiskEngine instead of being pushed into every strategy).
    void notify_order_gone(OrderId id) noexcept {
        if (resting_bid_id_.has_value() && *resting_bid_id_ == id) {
            resting_bid_id_.reset();
        }
        if (resting_ask_id_.has_value() && *resting_ask_id_ == id) {
            resting_ask_id_.reset();
        }
    }

private:
    [[nodiscard]] Price compute_mid(std::optional<Price> best_bid,
                                     std::optional<Price> best_ask) const noexcept {
        if (best_bid.has_value() && best_ask.has_value()) {
            return Price{(best_bid->value() + best_ask->value()) / 2};
        }
        if (best_bid.has_value()) {
            return *best_bid + Price{config_.half_spread_ticks};
        }
        if (best_ask.has_value()) {
            return *best_ask - Price{config_.half_spread_ticks};
        }
        return config_.initial_reference_price;
    }

    [[nodiscard]] protocol::NewOrderMessage make_new_order(Side side, Price price) noexcept {
        return protocol::NewOrderMessage{
            .order_id = next_order_id_++,
            .price = price.value(),
            .quantity = config_.quote_size,
            .client_id = config_.client.value(),
            .symbol_id = config_.symbol.value(),
            .side = side,
            .order_type = OrderType::Limit,
            .time_in_force = TimeInForce::Day,
        };
    }

    [[nodiscard]] protocol::ReplaceOrderMessage make_replace(OrderId id, Price new_price) noexcept {
        return protocol::ReplaceOrderMessage{
            .order_id = id.value(),
            .new_price = new_price.value(),
            .new_quantity = config_.quote_size,
            .client_id = config_.client.value(),
            .symbol_id = config_.symbol.value(),
        };
    }

    MarketMakerConfig config_;
    std::uint64_t next_order_id_;
    std::optional<OrderId> resting_bid_id_;
    std::optional<OrderId> resting_ask_id_;
    Price last_quoted_bid_{0};
    Price last_quoted_ask_{0};
};

}  // namespace jane::strategy
