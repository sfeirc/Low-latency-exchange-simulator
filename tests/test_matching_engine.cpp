#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "jane/matching/matching_engine.hpp"

using namespace jane;
using namespace jane::matching;

namespace {

using Engine = MatchingEngine<400, 128>;  // prices [1000, 1400)

Order make_order(std::uint64_t id, Side side, OrderType type, TimeInForce tif, std::int64_t price,
                  std::int64_t qty, std::uint32_t client = 1, std::uint64_t seq = 0) {
    Order o{};
    o.id = OrderId{id};
    o.client = ClientId{client};
    o.symbol = SymbolId{1};
    o.side = side;
    o.type = type;
    o.tif = tif;
    o.price = Price{price};
    o.quantity = Quantity{qty};
    o.remaining = Quantity{qty};
    o.sequence = Sequence{seq};
    o.timestamp = Nanos{0};
    return o;
}

Order limit(std::uint64_t id, Side side, std::int64_t price, std::int64_t qty,
            std::uint32_t client = 1) {
    return make_order(id, side, OrderType::Limit, TimeInForce::Day, price, qty, client);
}

// Fresh out_fills per call, mirroring the common "I only care about this
// one call's fills" test usage; tests exercising cross-call accumulation
// (none currently need to) would share a vector across calls instead.
NewOrderResult submit(Engine& engine, const Order& order, std::vector<Fill>& fills) {
    fills.clear();
    return engine.submit(order, fills);
}

}  // namespace

// --- non-crossing: rests on the book -------------------------------------

TEST_CASE("Matching: a non-crossing limit order simply rests", "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    auto result = submit(engine, limit(1, Side::Buy, 1050, 100), fills);
    REQUIRE(result.reject_reason == RejectReason::None);
    REQUIRE(fills.empty());
    REQUIRE(result.rested);
    REQUIRE(result.remaining_after.value() == 100);
    REQUIRE(engine.book().best_bid() == Price{1050});
}

// --- basic crossing --------------------------------------------------------

TEST_CASE("Matching: an aggressive limit order fills at the resting price (price improvement)",
          "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(submit(engine, limit(1, Side::Sell, 1050, 100), fills).reject_reason ==
            RejectReason::None);

    // Buyer is willing to pay up to 1080, but the resting ask is only 1050.
    auto result = submit(engine, limit(2, Side::Buy, 1080, 100), fills);
    REQUIRE(result.reject_reason == RejectReason::None);
    REQUIRE(fills.size() == 1);
    REQUIRE(fills[0].price == Price{1050});  // resting price, not the aggressor's limit
    REQUIRE(fills[0].quantity.value() == 100);
    REQUIRE(fills[0].resting_order_id == OrderId{1});
    REQUIRE(fills[0].aggressor_order_id == OrderId{2});
    REQUIRE(fills[0].resting_fully_filled);
    REQUIRE(result.remaining_after.value() == 0);
    REQUIRE_FALSE(result.rested);

    REQUIRE_FALSE(engine.book().best_ask().has_value());  // resting side fully consumed
    REQUIRE(engine.book().find(OrderId{1}) == nullptr);
}

TEST_CASE("Matching: resting order partially filled keeps its FIFO position", "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(submit(engine, limit(1, Side::Sell, 1050, 100), fills).reject_reason ==
            RejectReason::None);

    auto result = submit(engine, limit(2, Side::Buy, 1050, 40), fills);
    REQUIRE(fills.size() == 1);
    REQUIRE(fills[0].quantity.value() == 40);
    REQUIRE_FALSE(fills[0].resting_fully_filled);
    (void)result;

    const auto* node = engine.book().find(OrderId{1});
    REQUIRE(node != nullptr);
    REQUIRE(node->order.remaining.value() == 60);
}

TEST_CASE("Matching: aggressor partially filled, remainder rests (Day)", "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(submit(engine, limit(1, Side::Sell, 1050, 30), fills).reject_reason ==
            RejectReason::None);

    auto result = submit(engine, limit(2, Side::Buy, 1050, 100), fills);
    REQUIRE(fills.size() == 1);
    REQUIRE(fills[0].quantity.value() == 30);
    REQUIRE(result.remaining_after.value() == 70);
    REQUIRE(result.rested);

    REQUIRE(engine.book().best_bid() == Price{1050});
    const auto* node = engine.book().find(OrderId{2});
    REQUIRE(node != nullptr);
    REQUIRE(node->order.remaining.value() == 70);
}

// --- FIFO / price priority across multiple resting orders and levels -----

TEST_CASE("Matching: consumes resting orders in strict FIFO order within a level", "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(submit(engine, limit(1, Side::Sell, 1050, 10), fills).reject_reason ==
            RejectReason::None);
    REQUIRE(submit(engine, limit(2, Side::Sell, 1050, 10), fills).reject_reason ==
            RejectReason::None);
    REQUIRE(submit(engine, limit(3, Side::Sell, 1050, 10), fills).reject_reason ==
            RejectReason::None);

    auto result = submit(engine, limit(4, Side::Buy, 1050, 25), fills);
    REQUIRE(fills.size() == 3);
    REQUIRE(fills[0].resting_order_id == OrderId{1});
    REQUIRE(fills[0].quantity.value() == 10);
    REQUIRE(fills[1].resting_order_id == OrderId{2});
    REQUIRE(fills[1].quantity.value() == 10);
    REQUIRE(fills[2].resting_order_id == OrderId{3});
    REQUIRE(fills[2].quantity.value() == 5);
    REQUIRE_FALSE(fills[2].resting_fully_filled);
    REQUIRE(result.remaining_after.value() == 0);

    const auto* node3 = engine.book().find(OrderId{3});
    REQUIRE(node3->order.remaining.value() == 5);
}

TEST_CASE("Matching: consumes best price level fully before touching the next", "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(submit(engine, limit(1, Side::Sell, 1050, 10), fills).reject_reason ==
            RejectReason::None);
    REQUIRE(submit(engine, limit(2, Side::Sell, 1040, 10), fills).reject_reason ==
            RejectReason::None);  // better ask

    submit(engine, limit(3, Side::Buy, 1060, 15), fills);
    REQUIRE(fills.size() == 2);
    REQUIRE(fills[0].price == Price{1040});  // best (lowest) ask first
    REQUIRE(fills[0].quantity.value() == 10);
    REQUIRE(fills[1].price == Price{1050});
    REQUIRE(fills[1].quantity.value() == 5);
}

// --- market orders ---------------------------------------------------------

TEST_CASE("Matching: market order sweeps price levels ignoring any limit", "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(submit(engine, limit(1, Side::Sell, 1040, 10), fills).reject_reason ==
            RejectReason::None);
    REQUIRE(submit(engine, limit(2, Side::Sell, 1050, 10), fills).reject_reason ==
            RejectReason::None);

    auto result =
        submit(engine, make_order(3, Side::Buy, OrderType::Market, TimeInForce::Day, 0, 15), fills);
    REQUIRE(result.reject_reason == RejectReason::None);
    REQUIRE(fills.size() == 2);
    REQUIRE(fills[0].price == Price{1040});
    REQUIRE(fills[1].price == Price{1050});
    REQUIRE(result.remaining_after.value() == 0);
}

TEST_CASE("Matching: market order with partial liquidity fills what it can and never rests",
          "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(submit(engine, limit(1, Side::Sell, 1040, 10), fills).reject_reason ==
            RejectReason::None);

    auto result = submit(
        engine, make_order(2, Side::Buy, OrderType::Market, TimeInForce::Day, 0, 100), fills);
    REQUIRE(fills.size() == 1);
    REQUIRE(fills[0].quantity.value() == 10);
    REQUIRE(result.remaining_after.value() == 90);
    REQUIRE_FALSE(result.rested);
    REQUIRE(engine.book().find(OrderId{2}) == nullptr);
}

TEST_CASE("Matching: market order against an empty book fills nothing and isn't an error",
          "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    auto result =
        submit(engine, make_order(1, Side::Buy, OrderType::Market, TimeInForce::Day, 0, 50), fills);
    REQUIRE(result.reject_reason == RejectReason::None);
    REQUIRE(fills.empty());
    REQUIRE(result.remaining_after.value() == 50);
    REQUIRE_FALSE(result.rested);
}

// --- IOC ---------------------------------------------------------------

TEST_CASE("Matching: IOC fills what it can and drops the remainder without resting",
          "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(submit(engine, limit(1, Side::Sell, 1050, 10), fills).reject_reason ==
            RejectReason::None);

    auto result = submit(
        engine, make_order(2, Side::Buy, OrderType::Limit, TimeInForce::IOC, 1060, 100), fills);
    REQUIRE(fills.size() == 1);
    REQUIRE(fills[0].quantity.value() == 10);
    REQUIRE(result.remaining_after.value() == 90);
    REQUIRE_FALSE(result.rested);
    REQUIRE(engine.book().find(OrderId{2}) == nullptr);
}

TEST_CASE("Matching: IOC with no crossable liquidity fills nothing and doesn't rest",
          "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(submit(engine, limit(1, Side::Sell, 1080, 10), fills).reject_reason ==
            RejectReason::None);

    auto result = submit(
        engine, make_order(2, Side::Buy, OrderType::Limit, TimeInForce::IOC, 1050, 100), fills);
    REQUIRE(fills.empty());
    REQUIRE(result.remaining_after.value() == 100);
    REQUIRE_FALSE(result.rested);
}

// --- FOK -----------------------------------------------------------------

TEST_CASE("Matching: FOK fills completely across multiple levels when enough liquidity exists",
          "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(submit(engine, limit(1, Side::Sell, 1040, 10), fills).reject_reason ==
            RejectReason::None);
    REQUIRE(submit(engine, limit(2, Side::Sell, 1050, 10), fills).reject_reason ==
            RejectReason::None);

    auto result = submit(
        engine, make_order(3, Side::Buy, OrderType::Limit, TimeInForce::FOK, 1050, 20), fills);
    REQUIRE(result.reject_reason == RejectReason::None);
    REQUIRE(fills.size() == 2);
    REQUIRE(result.remaining_after.value() == 0);
    REQUIRE_FALSE(result.rested);
}

TEST_CASE("Matching: FOK with insufficient liquidity fills NOTHING (book left untouched)",
          "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(submit(engine, limit(1, Side::Sell, 1040, 10), fills).reject_reason ==
            RejectReason::None);

    auto result = submit(
        engine, make_order(2, Side::Buy, OrderType::Limit, TimeInForce::FOK, 1050, 20), fills);
    REQUIRE(result.reject_reason == RejectReason::FokNotFillable);
    REQUIRE(fills.empty());
    REQUIRE(result.remaining_after.value() == 20);

    // The resting order that was NOT enough must be completely undisturbed.
    const auto* node = engine.book().find(OrderId{1});
    REQUIRE(node != nullptr);
    REQUIRE(node->order.remaining.value() == 10);
    REQUIRE(engine.book().best_ask() == Price{1040});
}

TEST_CASE("Matching: FOK exactly at the available-liquidity boundary succeeds", "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(submit(engine, limit(1, Side::Sell, 1050, 20), fills).reject_reason ==
            RejectReason::None);

    auto result = submit(
        engine, make_order(2, Side::Buy, OrderType::Limit, TimeInForce::FOK, 1050, 20), fills);
    REQUIRE(result.reject_reason == RejectReason::None);
    REQUIRE(result.remaining_after.value() == 0);
}

TEST_CASE("Matching: FOK respects the limit price when checking available liquidity",
          "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(submit(engine, limit(1, Side::Sell, 1080, 100), fills).reject_reason ==
            RejectReason::None);

    // Plenty of quantity is resting, but not at an acceptable price.
    auto result = submit(
        engine, make_order(2, Side::Buy, OrderType::Limit, TimeInForce::FOK, 1050, 20), fills);
    REQUIRE(result.reject_reason == RejectReason::FokNotFillable);
    REQUIRE(fills.empty());
}

TEST_CASE("Matching: market FOK requires full quantity fillable at any price", "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(submit(engine, limit(1, Side::Sell, 1040, 5), fills).reject_reason ==
            RejectReason::None);

    auto insufficient = submit(
        engine, make_order(2, Side::Buy, OrderType::Market, TimeInForce::FOK, 0, 10), fills);
    REQUIRE(insufficient.reject_reason == RejectReason::FokNotFillable);
    REQUIRE(fills.empty());

    REQUIRE(submit(engine, limit(3, Side::Sell, 1050, 5), fills).reject_reason ==
            RejectReason::None);
    auto sufficient = submit(
        engine, make_order(4, Side::Buy, OrderType::Market, TimeInForce::FOK, 0, 10), fills);
    REQUIRE(sufficient.reject_reason == RejectReason::None);
    REQUIRE(fills.size() == 2);
}

// --- cancel ----------------------------------------------------------------

TEST_CASE("Matching: cancel removes a resting order", "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(submit(engine, limit(1, Side::Buy, 1050, 10), fills).reject_reason ==
            RejectReason::None);
    REQUIRE(engine.cancel(OrderId{1}).accepted);
    REQUIRE(engine.book().find(OrderId{1}) == nullptr);
}

TEST_CASE("Matching: cancel of an unknown order is not accepted", "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    REQUIRE_FALSE(engine.cancel(OrderId{999}).accepted);
}

// --- replace ---------------------------------------------------------------

TEST_CASE("Matching: a non-marketable replace just repositions, losing priority", "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(submit(engine, limit(1, Side::Buy, 1050, 100), fills).reject_reason ==
            RejectReason::None);
    REQUIRE(submit(engine, limit(2, Side::Buy, 1060, 50), fills).reject_reason ==
            RejectReason::None);

    fills.clear();
    auto result = engine.replace(OrderId{1}, Price{1060}, Quantity{75}, Sequence{1}, Nanos{0}, fills);
    REQUIRE(result.accepted);
    REQUIRE(fills.empty());
    REQUIRE(result.rested);

    const auto* lvl = engine.book().level_at(Side::Buy, Price{1060});
    REQUIRE(lvl->head->order.id == OrderId{2});  // kept priority
    REQUIRE(lvl->tail->order.id == OrderId{1});  // replaced order lost it
}

TEST_CASE("Matching: a replace that becomes marketable executes immediately instead of "
          "resting at a crossed price",
          "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(submit(engine, limit(1, Side::Buy, 1040, 50), fills).reject_reason ==
            RejectReason::None);
    REQUIRE(submit(engine, limit(2, Side::Sell, 1060, 30), fills).reject_reason ==
            RejectReason::None);

    // Move the resting bid up to 1070 — now crosses the resting ask at 1060.
    fills.clear();
    auto result = engine.replace(OrderId{1}, Price{1070}, Quantity{50}, Sequence{1}, Nanos{0}, fills);
    REQUIRE(result.accepted);
    REQUIRE(fills.size() == 1);
    REQUIRE(fills[0].price == Price{1060});  // resting (ask) price
    REQUIRE(fills[0].quantity.value() == 30);
    REQUIRE(result.remaining_after.value() == 20);
    REQUIRE(result.rested);  // the unfilled remainder still rests (Day)

    REQUIRE_FALSE(engine.book().best_ask().has_value());  // ask side fully consumed
    REQUIRE(engine.book().best_bid() == Price{1070});
    const auto* node = engine.book().find(OrderId{1});
    REQUIRE(node->order.remaining.value() == 20);

    // The book must never be left crossed, even transiently in the
    // observable end state.
    REQUIRE_FALSE(engine.book().best_ask().has_value());
}

TEST_CASE("Matching: replace validates before cancelling the original", "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(submit(engine, limit(1, Side::Buy, 1050, 100), fills).reject_reason ==
            RejectReason::None);

    REQUIRE(engine.replace(OrderId{999}, Price{1050}, Quantity{10}, Sequence{1}, Nanos{0}, fills)
                .reject_reason == RejectReason::UnknownOrder);
    REQUIRE(engine.replace(OrderId{1}, Price{1050}, Quantity{0}, Sequence{1}, Nanos{0}, fills)
                .reject_reason == RejectReason::InvalidQuantity);
    REQUIRE(engine.replace(OrderId{1}, Price{99999}, Quantity{10}, Sequence{1}, Nanos{0}, fills)
                .reject_reason == RejectReason::InvalidPrice);

    const auto* node = engine.book().find(OrderId{1});
    REQUIRE(node != nullptr);
    REQUIRE(node->order.remaining.value() == 100);
}

// --- rejects -----------------------------------------------------------

TEST_CASE("Matching: rejects an order for the wrong symbol", "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    Order o = limit(1, Side::Buy, 1050, 10);
    o.symbol = SymbolId{2};
    auto result = submit(engine, o, fills);
    REQUIRE(result.reject_reason == RejectReason::UnknownSymbol);
    REQUIRE(engine.book().order_count() == 0);
}

TEST_CASE("Matching: rejects non-positive quantity", "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(submit(engine, limit(1, Side::Buy, 1050, 0), fills).reject_reason ==
            RejectReason::InvalidQuantity);
}

// --- explicitly documented scope: no self-trade prevention ----------------

TEST_CASE("Matching: a client's aggressive order CAN trade against its own resting order "
          "(no self-trade prevention — documented scope, not an oversight)",
          "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(submit(engine, limit(1, Side::Sell, 1050, 10, /*client=*/7), fills).reject_reason ==
            RejectReason::None);
    submit(engine, limit(2, Side::Buy, 1050, 10, /*client=*/7), fills);
    REQUIRE(fills.size() == 1);
    REQUIRE(fills[0].resting_client_id == ClientId{7});
    REQUIRE(fills[0].aggressor_client_id == ClientId{7});
}

// --- caller-provided buffer contract ---------------------------------------

TEST_CASE("Matching: fills are appended, not cleared — callers can accumulate across a batch",
          "[matching]") {
    Engine engine(SymbolId{1}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(engine.submit(limit(1, Side::Sell, 1050, 10), fills).reject_reason ==
            RejectReason::None);
    REQUIRE(engine.submit(limit(2, Side::Buy, 1050, 5), fills).fill_count == 1);
    REQUIRE(fills.size() == 1);
    REQUIRE(engine.submit(limit(3, Side::Buy, 1050, 5), fills).fill_count == 1);
    REQUIRE(fills.size() == 2);  // second call's fill appended after the first's, not replacing it
}
