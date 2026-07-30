#include <catch2/catch_test_macros.hpp>

#include "jane/marketdata/sinks.hpp"
#include "jane/replay/replay_engine.hpp"
#include "jane/strategy/market_maker.hpp"

using namespace jane;
using namespace jane::strategy;

namespace {
MarketMakerConfig basic_config() {
    return MarketMakerConfig{.symbol = SymbolId{1},
                              .client = ClientId{9},
                              .half_spread_ticks = 5,
                              .quote_size = 10,
                              .max_inventory = 50,
                              .initial_reference_price = Price{1000}};
}
}  // namespace

TEST_CASE("MarketMaker: first tick on an empty book quotes around the reference price",
          "[strategy][market_maker]") {
    MarketMaker mm(basic_config(), 1);
    auto action = mm.on_tick(std::nullopt, std::nullopt, Quantity{0});

    REQUIRE(action.new_bid.has_value());
    REQUIRE(action.new_ask.has_value());
    REQUIRE_FALSE(action.replace_bid.has_value());
    REQUIRE_FALSE(action.replace_ask.has_value());
    REQUIRE(action.new_bid->price == 995);   // 1000 - 5
    REQUIRE(action.new_ask->price == 1005);  // 1000 + 5
    REQUIRE(action.new_bid->side == Side::Buy);
    REQUIRE(action.new_ask->side == Side::Sell);
    REQUIRE(action.new_bid->time_in_force == TimeInForce::Day);
    REQUIRE(mm.resting_bid_id().has_value());
    REQUIRE(mm.resting_ask_id().has_value());
}

TEST_CASE("MarketMaker: quotes around the true mid once both sides of the market exist",
          "[strategy][market_maker]") {
    MarketMaker mm(basic_config(), 1);
    auto action = mm.on_tick(Price{1090}, Price{1110}, Quantity{0});  // mid = 1100
    REQUIRE(action.new_bid->price == 1095);
    REQUIRE(action.new_ask->price == 1105);
}

TEST_CASE("MarketMaker: an unchanged mid emits no replace on the second tick",
          "[strategy][market_maker]") {
    MarketMaker mm(basic_config(), 1);
    (void)mm.on_tick(Price{1090}, Price{1110}, Quantity{0});
    auto second = mm.on_tick(Price{1090}, Price{1110}, Quantity{0});

    REQUIRE_FALSE(second.new_bid.has_value());
    REQUIRE_FALSE(second.new_ask.has_value());
    REQUIRE_FALSE(second.replace_bid.has_value());
    REQUIRE_FALSE(second.replace_ask.has_value());
}

TEST_CASE("MarketMaker: a moved mid re-quotes via replace, not a fresh order",
          "[strategy][market_maker]") {
    MarketMaker mm(basic_config(), 1);
    auto first = mm.on_tick(Price{1090}, Price{1110}, Quantity{0});
    const OrderId bid_id = OrderId{first.new_bid->order_id};

    auto second = mm.on_tick(Price{1190}, Price{1210}, Quantity{0});  // mid moved to 1200
    REQUIRE_FALSE(second.new_bid.has_value());
    REQUIRE(second.replace_bid.has_value());
    REQUIRE(second.replace_bid->order_id == bid_id.value());
    REQUIRE(second.replace_bid->new_price == 1195);
    REQUIRE(second.replace_ask->new_price == 1205);
}

TEST_CASE("MarketMaker: reaching the long inventory cap pulls the bid, not the ask",
          "[strategy][market_maker]") {
    MarketMaker mm(basic_config(), 1);
    (void)mm.on_tick(Price{1090}, Price{1110}, Quantity{0});
    auto capped = mm.on_tick(Price{1090}, Price{1110}, Quantity{50});  // at max_inventory
    REQUIRE(capped.cancel_bid.has_value());
    REQUIRE_FALSE(capped.cancel_ask.has_value());
    REQUIRE_FALSE(mm.resting_bid_id().has_value());
    REQUIRE(mm.resting_ask_id().has_value());  // ask untouched
}

TEST_CASE("MarketMaker: reaching the short inventory cap pulls the ask, not the bid",
          "[strategy][market_maker]") {
    MarketMaker mm(basic_config(), 1);
    (void)mm.on_tick(Price{1090}, Price{1110}, Quantity{0});
    auto capped = mm.on_tick(Price{1090}, Price{1110}, Quantity{-50});
    REQUIRE(capped.cancel_ask.has_value());
    REQUIRE_FALSE(capped.cancel_bid.has_value());
    REQUIRE_FALSE(mm.resting_ask_id().has_value());
    REQUIRE(mm.resting_bid_id().has_value());
}

TEST_CASE("MarketMaker: an uncapped tick after a cap resumes quoting with a fresh order",
          "[strategy][market_maker]") {
    MarketMaker mm(basic_config(), 1);
    (void)mm.on_tick(Price{1090}, Price{1110}, Quantity{0});
    (void)mm.on_tick(Price{1090}, Price{1110}, Quantity{50});  // caps and pulls the bid
    REQUIRE_FALSE(mm.resting_bid_id().has_value());

    auto resumed = mm.on_tick(Price{1090}, Price{1110}, Quantity{10});  // back under cap
    REQUIRE(resumed.new_bid.has_value());  // fresh order, not a replace of the pulled one
    REQUIRE_FALSE(resumed.replace_bid.has_value());
}

TEST_CASE("MarketMaker: notify_order_gone stops treating a filled order as still resting",
          "[strategy][market_maker]") {
    MarketMaker mm(basic_config(), 1);
    auto first = mm.on_tick(Price{1090}, Price{1110}, Quantity{0});
    const OrderId bid_id = OrderId{first.new_bid->order_id};

    mm.notify_order_gone(bid_id);
    REQUIRE_FALSE(mm.resting_bid_id().has_value());

    auto after = mm.on_tick(Price{1190}, Price{1210}, Quantity{0});
    REQUIRE(after.new_bid.has_value());  // fresh order, since the tracked one is gone
    REQUIRE_FALSE(after.replace_bid.has_value());
}

// --- integration: driven through a real pipeline ---------------------------

TEST_CASE("MarketMaker: driven through a real pipeline actually gets filled and tracked "
          "by risk",
          "[strategy][market_maker][integration]") {
    marketdata::InMemorySink sink;
    matching::MatchingEngine<400, 128> engine(SymbolId{1}, Price{800});
    risk::RiskEngine<64> risk(risk::Limits{.max_order_size = Quantity{1000},
                                            .max_position = Quantity{1000},
                                            .max_loss_per_client = PnL{-1'000'000}});
    marketdata::MarketDataPublisher<marketdata::InMemorySink> feed(sink);
    replay::ReplayEngine<400, 128, 64, marketdata::InMemorySink> pipeline(engine, risk, feed);
    replay::DeterministicClock clock;

    MarketMaker mm(MarketMakerConfig{.symbol = SymbolId{1},
                                      .client = ClientId{9},
                                      .half_spread_ticks = 5,
                                      .quote_size = 10,
                                      .max_inventory = 200,
                                      .initial_reference_price = Price{1000}},
                   1);

    auto action = mm.on_tick(engine.book().best_bid(), engine.book().best_ask(), Quantity{0});
    REQUIRE(action.new_bid.has_value());
    pipeline.process_new_order(*action.new_bid, clock.tick());
    pipeline.process_new_order(*action.new_ask, clock.tick());
    REQUIRE(engine.book().best_bid() == Price{995});
    REQUIRE(engine.book().best_ask() == Price{1005});

    // An aggressive counterparty lifts the maker's offer.
    Order taker{};
    taker.id = OrderId{9999};
    taker.client = ClientId{5};
    taker.symbol = SymbolId{1};
    taker.side = Side::Buy;
    taker.type = OrderType::Limit;
    taker.tif = TimeInForce::IOC;
    taker.price = Price{1005};
    taker.quantity = Quantity{10};
    taker.remaining = taker.quantity;
    std::vector<matching::Fill> fills;
    auto taker_result = engine.submit(taker, fills);
    for (auto& f : fills) {
        risk.record_fill(f.resting_client_id, SymbolId{1}, opposite(f.aggressor_side), f.price,
                          f.quantity);
        risk.record_fill(f.aggressor_client_id, SymbolId{1}, f.aggressor_side, f.price, f.quantity);
    }
    REQUIRE(taker_result.reject_reason == RejectReason::None);
    REQUIRE_FALSE(fills.empty());

    // The maker sold — it should now be short.
    REQUIRE(risk.position(ClientId{9}, SymbolId{1}).value() == -10);

    REQUIRE(mm.resting_ask_id().has_value());  // still tracked until told otherwise
    mm.notify_order_gone(*mm.resting_ask_id());
    REQUIRE_FALSE(mm.resting_ask_id().has_value());
}
