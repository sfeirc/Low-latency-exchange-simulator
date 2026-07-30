#pragma once

#include <optional>

#include "jane/core/types.hpp"
#include "jane/protocol/messages.hpp"

// Watches two symbols this project's synthetic order flow treats as a
// correlated pair (see apps/, or a test's own generator config — nothing
// in this class or the exchange itself enforces any actual price
// relationship between them; the "arbitrage" is only real to the extent
// the two books' prices are actually kept correlated by whatever is
// generating their order flow) and, when one side's touch is priced
// enough better than the other's to clear `min_edge_ticks` after
// crossing, submits an IOC leg on each book to capture the spread.
namespace jane::strategy {

struct ArbitrageConfig {
    SymbolId symbol_a;
    SymbolId symbol_b;
    ClientId client;
    std::int64_t clip_size = 10;
    std::int64_t min_edge_ticks = 2;  // minimum profitable edge required to act
};

struct ArbitrageAction {
    protocol::NewOrderMessage buy_leg;
    protocol::NewOrderMessage sell_leg;
    std::int64_t edge_ticks;  // the observed edge that triggered this, for logging/analysis
};

class ArbitrageStrategy {
public:
    ArbitrageStrategy(ArbitrageConfig config, std::uint64_t first_order_id) noexcept
        : config_(config), next_order_id_(first_order_id) {}

    // Read-only: does not mutate any book state, just decides whether an
    // opportunity exists given the current top-of-book on both symbols.
    // Checks both directions (buy A/sell B and buy B/sell A) — real edge
    // can appear on either side depending on which symbol is temporarily
    // rich or cheap.
    [[nodiscard]] std::optional<ArbitrageAction> check(std::optional<Price> a_best_bid,
                                                         std::optional<Price> a_best_ask,
                                                         std::optional<Price> b_best_bid,
                                                         std::optional<Price> b_best_ask) noexcept {
        if (b_best_bid.has_value() && a_best_ask.has_value()) {
            const std::int64_t edge = b_best_bid->value() - a_best_ask->value();
            if (edge >= config_.min_edge_ticks) {
                return make_action(config_.symbol_a, *a_best_ask, config_.symbol_b, *b_best_bid, edge);
            }
        }
        if (a_best_bid.has_value() && b_best_ask.has_value()) {
            const std::int64_t edge = a_best_bid->value() - b_best_ask->value();
            if (edge >= config_.min_edge_ticks) {
                return make_action(config_.symbol_b, *b_best_ask, config_.symbol_a, *a_best_bid, edge);
            }
        }
        return std::nullopt;
    }

private:
    [[nodiscard]] protocol::NewOrderMessage make_leg(SymbolId symbol, Side side,
                                                       Price price) noexcept {
        return protocol::NewOrderMessage{
            .order_id = next_order_id_++,
            .price = price.value(),
            .quantity = config_.clip_size,
            .client_id = config_.client.value(),
            .symbol_id = symbol.value(),
            .side = side,
            .order_type = OrderType::Limit,
            .time_in_force = TimeInForce::IOC,
        };
    }

    // buy_symbol at buy_price, sell_symbol at sell_price — always in that
    // side order (the cheap leg is always the buy, the rich leg always
    // the sell), which is why neither side needs to be a parameter.
    [[nodiscard]] ArbitrageAction make_action(SymbolId buy_symbol, Price buy_price,
                                               SymbolId sell_symbol, Price sell_price,
                                               std::int64_t edge) noexcept {
        return ArbitrageAction{
            .buy_leg = make_leg(buy_symbol, Side::Buy, buy_price),
            .sell_leg = make_leg(sell_symbol, Side::Sell, sell_price),
            .edge_ticks = edge,
        };
    }

    ArbitrageConfig config_;
    std::uint64_t next_order_id_;
};

}  // namespace jane::strategy
