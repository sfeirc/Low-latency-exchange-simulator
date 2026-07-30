#include <catch2/catch_test_macros.hpp>

#include "jane/risk/risk_engine.hpp"

using namespace jane;
using namespace jane::risk;

namespace {

using Engine = RiskEngine<64>;

Order make_order(Side side, std::int64_t qty, std::uint32_t client = 1, std::uint32_t symbol = 1) {
    Order o{};
    o.id = OrderId{1};
    o.client = ClientId{client};
    o.symbol = SymbolId{symbol};
    o.side = side;
    o.type = OrderType::Limit;
    o.tif = TimeInForce::Day;
    o.price = Price{100};
    o.quantity = Quantity{qty};
    o.remaining = Quantity{qty};
    return o;
}

Limits generous_limits() {
    return Limits{.max_order_size = Quantity{1'000'000},
                  .max_position = Quantity{1'000'000},
                  .max_loss_per_client = PnL{-1'000'000'000}};
}

}  // namespace

// --- max order size --------------------------------------------------------

TEST_CASE("RiskEngine: rejects an order exceeding max_order_size", "[risk]") {
    Limits limits = generous_limits();
    limits.max_order_size = Quantity{100};
    Engine engine(limits);

    REQUIRE(engine.check_new_order(make_order(Side::Buy, 100)) == RejectReason::None);
    REQUIRE(engine.check_new_order(make_order(Side::Buy, 101)) == RejectReason::RiskMaxOrderSize);
}

// --- max position ------------------------------------------------------

TEST_CASE("RiskEngine: rejects an order that would push position beyond the limit",
          "[risk]") {
    Limits limits = generous_limits();
    limits.max_position = Quantity{100};
    Engine engine(limits);

    REQUIRE(engine.check_new_order(make_order(Side::Buy, 100)) == RejectReason::None);
    REQUIRE(engine.check_new_order(make_order(Side::Buy, 101)) == RejectReason::RiskMaxPosition);
    // Selling past -limit is symmetric.
    REQUIRE(engine.check_new_order(make_order(Side::Sell, 101)) == RejectReason::RiskMaxPosition);
}

TEST_CASE("RiskEngine: an established position tightens headroom for further orders on the "
          "same side",
          "[risk]") {
    Limits limits = generous_limits();
    limits.max_position = Quantity{100};
    Engine engine(limits);

    engine.record_fill(ClientId{1}, SymbolId{1}, Side::Buy, Price{100}, Quantity{80});
    REQUIRE(engine.position(ClientId{1}, SymbolId{1}).value() == 80);

    REQUIRE(engine.check_new_order(make_order(Side::Buy, 20)) == RejectReason::None);   // exactly to the limit
    REQUIRE(engine.check_new_order(make_order(Side::Buy, 21)) == RejectReason::RiskMaxPosition);
}

TEST_CASE("RiskEngine: a reducing trade is allowed even near the limit", "[risk]") {
    Limits limits = generous_limits();
    limits.max_position = Quantity{100};
    Engine engine(limits);

    engine.record_fill(ClientId{1}, SymbolId{1}, Side::Buy, Price{100}, Quantity{100});
    // Selling reduces the long position back toward zero — must not be
    // confused with the (rejected) case of extending it further.
    REQUIRE(engine.check_new_order(make_order(Side::Sell, 50)) == RejectReason::None);
}

TEST_CASE("RiskEngine: position and max_position are tracked per (client, symbol)",
          "[risk]") {
    Limits limits = generous_limits();
    limits.max_position = Quantity{100};
    Engine engine(limits);

    engine.record_fill(ClientId{1}, SymbolId{1}, Side::Buy, Price{100}, Quantity{100});
    // Same client, different symbol: independent exposure, order approved.
    REQUIRE(engine.check_new_order(make_order(Side::Buy, 100, /*client=*/1, /*symbol=*/2)) ==
            RejectReason::None);
    // Different client, same symbol: also independent.
    REQUIRE(engine.check_new_order(make_order(Side::Buy, 100, /*client=*/2, /*symbol=*/1)) ==
            RejectReason::None);
    REQUIRE(engine.position(ClientId{1}, SymbolId{2}).value() == 0);
}

// --- max loss ------------------------------------------------------------

TEST_CASE("RiskEngine: mark-to-market PnL reflects a round-trip profit and loss correctly",
          "[risk]") {
    Engine engine(generous_limits());

    // Buy 10 @ 100, sell 10 @ 110: 100 profit.
    engine.record_fill(ClientId{1}, SymbolId{1}, Side::Buy, Price{100}, Quantity{10});
    engine.record_fill(ClientId{1}, SymbolId{1}, Side::Sell, Price{110}, Quantity{10});
    REQUIRE(engine.position(ClientId{1}, SymbolId{1}).value() == 0);
    REQUIRE(engine.pnl(ClientId{1}, SymbolId{1}).value() == 100);

    // Buy 10 @ 100, sell 10 @ 90: 100 loss.
    engine.record_fill(ClientId{2}, SymbolId{1}, Side::Buy, Price{100}, Quantity{10});
    engine.record_fill(ClientId{2}, SymbolId{1}, Side::Sell, Price{90}, Quantity{10});
    REQUIRE(engine.pnl(ClientId{2}, SymbolId{1}).value() == -100);
}

TEST_CASE("RiskEngine: rejects further orders once realized loss breaches max_loss_per_client",
          "[risk]") {
    Limits limits = generous_limits();
    limits.max_loss_per_client = PnL{-50};
    Engine engine(limits);

    engine.record_fill(ClientId{1}, SymbolId{1}, Side::Buy, Price{100}, Quantity{10});
    REQUIRE(engine.check_new_order(make_order(Side::Buy, 1)) == RejectReason::None);  // no loss yet

    engine.record_fill(ClientId{1}, SymbolId{1}, Side::Sell, Price{90}, Quantity{10});  // -100 loss
    REQUIRE(engine.pnl(ClientId{1}, SymbolId{1}).value() == -100);
    REQUIRE(engine.check_new_order(make_order(Side::Buy, 1)) == RejectReason::RiskMaxLoss);
}

TEST_CASE("RiskEngine: a max-loss breach on one client doesn't affect another", "[risk]") {
    Limits limits = generous_limits();
    limits.max_loss_per_client = PnL{-50};
    Engine engine(limits);

    engine.record_fill(ClientId{1}, SymbolId{1}, Side::Buy, Price{100}, Quantity{10});
    engine.record_fill(ClientId{1}, SymbolId{1}, Side::Sell, Price{50}, Quantity{10});  // big loss
    REQUIRE(engine.check_new_order(make_order(Side::Buy, 1, /*client=*/1)) ==
            RejectReason::RiskMaxLoss);
    REQUIRE(engine.check_new_order(make_order(Side::Buy, 1, /*client=*/2)) == RejectReason::None);
}

// --- kill switch -------------------------------------------------------

TEST_CASE("RiskEngine: kill switch rejects everything regardless of other limits", "[risk]") {
    Engine engine(generous_limits());
    REQUIRE(engine.check_new_order(make_order(Side::Buy, 1)) == RejectReason::None);

    engine.engage_kill_switch();
    REQUIRE(engine.kill_switch_engaged());
    REQUIRE(engine.check_new_order(make_order(Side::Buy, 1)) == RejectReason::RiskKillSwitch);
    REQUIRE(engine.check_new_order(make_order(Side::Sell, 1)) == RejectReason::RiskKillSwitch);

    engine.disengage_kill_switch();
    REQUIRE_FALSE(engine.kill_switch_engaged());
    REQUIRE(engine.check_new_order(make_order(Side::Buy, 1)) == RejectReason::None);
}

TEST_CASE("RiskEngine: kill switch takes precedence over every other check", "[risk]") {
    Limits limits = generous_limits();
    limits.max_order_size = Quantity{1};  // this order would ALSO fail size, independently
    Engine engine(limits);
    engine.engage_kill_switch();

    REQUIRE(engine.check_new_order(make_order(Side::Buy, 999)) == RejectReason::RiskKillSwitch);
}

// --- record_fill accounting ------------------------------------------------

TEST_CASE("RiskEngine: record_fill updates position sign correctly for buy vs. sell",
          "[risk]") {
    Engine engine(generous_limits());
    engine.record_fill(ClientId{1}, SymbolId{1}, Side::Buy, Price{100}, Quantity{30});
    REQUIRE(engine.position(ClientId{1}, SymbolId{1}).value() == 30);
    engine.record_fill(ClientId{1}, SymbolId{1}, Side::Sell, Price{100}, Quantity{50});
    REQUIRE(engine.position(ClientId{1}, SymbolId{1}).value() == -20);
}

TEST_CASE("RiskEngine: an account with no fills yet has zero position and zero PnL",
          "[risk]") {
    Engine engine(generous_limits());
    REQUIRE(engine.position(ClientId{42}, SymbolId{1}).value() == 0);
    REQUIRE(engine.pnl(ClientId{42}, SymbolId{1}).value() == 0);
}
