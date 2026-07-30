#include <catch2/catch_test_macros.hpp>

#include "jane/marketdata/sinks.hpp"
#include "jane/replay/replay_engine.hpp"
#include "jane/strategy/arbitrage.hpp"

using namespace jane;
using namespace jane::strategy;

namespace {
ArbitrageConfig basic_config() {
    return ArbitrageConfig{
        .symbol_a = SymbolId{1}, .symbol_b = SymbolId{2}, .client = ClientId{9},
        .clip_size = 10, .min_edge_ticks = 2};
}
}  // namespace

TEST_CASE("Arbitrage: no action when the two books are priced fairly (no edge)",
          "[strategy][arbitrage]") {
    ArbitrageStrategy arb(basic_config(), 1);
    auto action = arb.check(Price{999}, Price{1001}, Price{999}, Price{1001});
    REQUIRE_FALSE(action.has_value());
}

TEST_CASE("Arbitrage: detects B rich vs. A cheap and buys A / sells B", "[strategy][arbitrage]") {
    ArbitrageStrategy arb(basic_config(), 1);
    // A: 999/1001 (ask=1001).  B: 1010/1012 (bid=1010).  Edge = 1010-1001 = 9 >= 2.
    auto action = arb.check(Price{999}, Price{1001}, Price{1010}, Price{1012});
    REQUIRE(action.has_value());
    REQUIRE(action->buy_leg.symbol_id == 1);
    REQUIRE(action->buy_leg.side == Side::Buy);
    REQUIRE(action->buy_leg.price == 1001);
    REQUIRE(action->sell_leg.symbol_id == 2);
    REQUIRE(action->sell_leg.side == Side::Sell);
    REQUIRE(action->sell_leg.price == 1010);
    REQUIRE(action->edge_ticks == 9);
    REQUIRE(action->buy_leg.time_in_force == TimeInForce::IOC);
    REQUIRE(action->sell_leg.time_in_force == TimeInForce::IOC);
}

TEST_CASE("Arbitrage: detects A rich vs. B cheap and buys B / sells A", "[strategy][arbitrage]") {
    ArbitrageStrategy arb(basic_config(), 1);
    // A: 1010/1012 (bid=1010).  B: 999/1001 (ask=1001).  Edge = 1010-1001 = 9.
    auto action = arb.check(Price{1010}, Price{1012}, Price{999}, Price{1001});
    REQUIRE(action.has_value());
    REQUIRE(action->buy_leg.symbol_id == 2);
    REQUIRE(action->buy_leg.price == 1001);
    REQUIRE(action->sell_leg.symbol_id == 1);
    REQUIRE(action->sell_leg.price == 1010);
    REQUIRE(action->edge_ticks == 9);
}

TEST_CASE("Arbitrage: an edge exactly at the threshold triggers (inclusive boundary)",
          "[strategy][arbitrage]") {
    ArbitrageStrategy arb(basic_config(), 1);  // min_edge_ticks = 2
    auto action = arb.check(Price{999}, Price{1000}, Price{1002}, Price{1004});  // edge exactly 2
    REQUIRE(action.has_value());
    REQUIRE(action->edge_ticks == 2);
}

TEST_CASE("Arbitrage: an edge just below the threshold does not trigger",
          "[strategy][arbitrage]") {
    ArbitrageStrategy arb(basic_config(), 1);
    auto action = arb.check(Price{999}, Price{1000}, Price{1001}, Price{1003});  // edge = 1
    REQUIRE_FALSE(action.has_value());
}

TEST_CASE("Arbitrage: a missing side on either book never crashes and never signals an "
          "opportunity",
          "[strategy][arbitrage]") {
    ArbitrageStrategy arb(basic_config(), 1);
    REQUIRE_FALSE(arb.check(std::nullopt, std::nullopt, std::nullopt, std::nullopt).has_value());
    REQUIRE_FALSE(arb.check(Price{1000}, std::nullopt, std::nullopt, Price{1000}).has_value());
    REQUIRE_FALSE(arb.check(std::nullopt, Price{1000}, Price{1000}, std::nullopt).has_value());
}

TEST_CASE("Arbitrage: order ids increment across successive opportunities",
          "[strategy][arbitrage]") {
    ArbitrageStrategy arb(basic_config(), 100);
    auto first = arb.check(Price{999}, Price{1001}, Price{1010}, Price{1012});
    auto second = arb.check(Price{999}, Price{1001}, Price{1010}, Price{1012});
    REQUIRE(first->buy_leg.order_id == 100);
    REQUIRE(first->sell_leg.order_id == 101);
    REQUIRE(second->buy_leg.order_id == 102);
    REQUIRE(second->sell_leg.order_id == 103);
}

// --- integration: a real dislocation across two real books -----------------

TEST_CASE("Arbitrage: both legs execute through a real pipeline and risk shows a profit",
          "[strategy][arbitrage][integration]") {
    marketdata::InMemorySink sink;
    matching::MatchingEngine<400, 128> book_a(SymbolId{1}, Price{800});
    matching::MatchingEngine<400, 128> book_b(SymbolId{2}, Price{800});
    risk::RiskEngine<64> risk(risk::Limits{.max_order_size = Quantity{1000},
                                            .max_position = Quantity{1000},
                                            .max_loss_per_client = PnL{-1'000'000}});
    marketdata::MarketDataPublisher<marketdata::InMemorySink> feed(sink);
    replay::ReplayEngine<400, 128, 64, marketdata::InMemorySink> pipeline_a(book_a, risk, feed);
    replay::ReplayEngine<400, 128, 64, marketdata::InMemorySink> pipeline_b(book_b, risk, feed);
    replay::DeterministicClock clock;

    // A is cheap (resting ask at 1000), B is rich (resting bid at 1010).
    // Sized to exactly clip_size (10, from basic_config()) so the arb
    // trade below consumes each fully — a larger size would leave
    // leftover resting quantity from client 2/3 that the later closing
    // orders would trip over (a first version of this test used 50 and
    // failed exactly that way: the "close" order crossed the *leftover*
    // resting quantity instead of resting for close_a_sell to hit).
    Order cheap_ask{};
    cheap_ask.id = OrderId{1};
    cheap_ask.client = ClientId{2};
    cheap_ask.symbol = SymbolId{1};
    cheap_ask.side = Side::Sell;
    cheap_ask.type = OrderType::Limit;
    cheap_ask.tif = TimeInForce::Day;
    cheap_ask.price = Price{1000};
    cheap_ask.quantity = Quantity{10};
    cheap_ask.remaining = cheap_ask.quantity;
    std::vector<matching::Fill> setup_fills;
    REQUIRE(book_a.submit(cheap_ask, setup_fills).reject_reason == RejectReason::None);

    Order rich_bid{};
    rich_bid.id = OrderId{2};
    rich_bid.client = ClientId{3};
    rich_bid.symbol = SymbolId{2};
    rich_bid.side = Side::Buy;
    rich_bid.type = OrderType::Limit;
    rich_bid.tif = TimeInForce::Day;
    rich_bid.price = Price{1010};
    rich_bid.quantity = Quantity{10};
    rich_bid.remaining = rich_bid.quantity;
    REQUIRE(book_b.submit(rich_bid, setup_fills).reject_reason == RejectReason::None);

    ArbitrageStrategy arb(basic_config(), 1000);
    auto action = arb.check(book_a.book().best_bid(), book_a.book().best_ask(),
                             book_b.book().best_bid(), book_b.book().best_ask());
    REQUIRE(action.has_value());
    REQUIRE(action->edge_ticks == 10);

    pipeline_a.process_new_order(action->buy_leg, clock.tick());
    pipeline_b.process_new_order(action->sell_leg, clock.tick());

    REQUIRE(risk.position(ClientId{9}, SymbolId{1}).value() == 10);   // bought on A
    REQUIRE(risk.position(ClientId{9}, SymbolId{2}).value() == -10);  // sold on B

    // The 100-tick edge (bought at 1000, sold at 1010) is real and
    // locked in — but RiskEngine marks each symbol independently at its
    // *own* last trade price (see risk_engine.hpp's ClientExposure
    // comment), with no notion that A and B are the same underlying.
    // Immediately after opening, each leg's own mark-to-market PnL is
    // exactly 0 (marked at the price it was just transacted at) even
    // though the combined position is a hedged, profitable one — this
    // is a real, documented limitation of the simplified risk model,
    // not a bug in arbitrage detection.
    REQUIRE(risk.pnl(ClientId{9}, SymbolId{1}).value() == 0);
    REQUIRE(risk.pnl(ClientId{9}, SymbolId{2}).value() == 0);

    // The edge only shows up in PnL once both legs are actually closed —
    // at *any* common price, since the position is fully hedged: closing
    // A's long and B's short at the same reference price P makes the P
    // terms cancel between the two legs, leaving exactly the captured
    // edge regardless of what P is. Demonstrated at P=1005, the midpoint,
    // but any P would reconcile to the same total.
    Order close_a{};
    close_a.id = OrderId{3};
    close_a.client = ClientId{2};
    close_a.symbol = SymbolId{1};
    close_a.side = Side::Buy;  // a counterparty to sell the arbitrageur's long back to
    close_a.type = OrderType::Limit;
    close_a.tif = TimeInForce::Day;
    close_a.price = Price{1005};
    close_a.quantity = Quantity{10};
    close_a.remaining = close_a.quantity;
    REQUIRE(book_a.submit(close_a, setup_fills).reject_reason == RejectReason::None);
    Order close_a_sell{};
    close_a_sell.id = OrderId{4};
    close_a_sell.client = ClientId{9};
    close_a_sell.symbol = SymbolId{1};
    close_a_sell.side = Side::Sell;
    close_a_sell.type = OrderType::Limit;
    close_a_sell.tif = TimeInForce::IOC;
    close_a_sell.price = Price{1005};
    close_a_sell.quantity = Quantity{10};
    close_a_sell.remaining = close_a_sell.quantity;
    std::vector<matching::Fill> close_fills_a;
    auto close_a_result = book_a.submit(close_a_sell, close_fills_a);
    for (auto& f : close_fills_a) {
        risk.record_fill(f.resting_client_id, SymbolId{1}, opposite(f.aggressor_side), f.price, f.quantity);
        risk.record_fill(f.aggressor_client_id, SymbolId{1}, f.aggressor_side, f.price, f.quantity);
    }
    REQUIRE(close_a_result.reject_reason == RejectReason::None);

    Order close_b{};
    close_b.id = OrderId{5};
    close_b.client = ClientId{3};
    close_b.symbol = SymbolId{2};
    close_b.side = Side::Sell;  // a counterparty to buy the arbitrageur's short back from
    close_b.type = OrderType::Limit;
    close_b.tif = TimeInForce::Day;
    close_b.price = Price{1005};
    close_b.quantity = Quantity{10};
    close_b.remaining = close_b.quantity;
    std::vector<matching::Fill> setup_fills_b;
    REQUIRE(book_b.submit(close_b, setup_fills_b).reject_reason == RejectReason::None);
    Order close_b_buy{};
    close_b_buy.id = OrderId{6};
    close_b_buy.client = ClientId{9};
    close_b_buy.symbol = SymbolId{2};
    close_b_buy.side = Side::Buy;
    close_b_buy.type = OrderType::Limit;
    close_b_buy.tif = TimeInForce::IOC;
    close_b_buy.price = Price{1005};
    close_b_buy.quantity = Quantity{10};
    close_b_buy.remaining = close_b_buy.quantity;
    std::vector<matching::Fill> close_fills_b;
    auto close_b_result = book_b.submit(close_b_buy, close_fills_b);
    for (auto& f : close_fills_b) {
        risk.record_fill(f.resting_client_id, SymbolId{2}, opposite(f.aggressor_side), f.price, f.quantity);
        risk.record_fill(f.aggressor_client_id, SymbolId{2}, f.aggressor_side, f.price, f.quantity);
    }
    REQUIRE(close_b_result.reject_reason == RejectReason::None);

    REQUIRE(risk.position(ClientId{9}, SymbolId{1}).value() == 0);
    REQUIRE(risk.position(ClientId{9}, SymbolId{2}).value() == 0);
    const std::int64_t realized_pnl =
        risk.pnl(ClientId{9}, SymbolId{1}).value() + risk.pnl(ClientId{9}, SymbolId{2}).value();
    REQUIRE(realized_pnl == 100);  // the edge, fully realized, independent of the P=1005 close price
}
