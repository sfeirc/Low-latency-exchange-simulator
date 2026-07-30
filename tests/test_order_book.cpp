#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <vector>

#include "jane/book/order_book.hpp"
#include "jane/core/order.hpp"

using namespace jane;
using namespace jane::book;

namespace {

using TestBook = OrderBook<200, 64>;  // prices [1000, 1200), up to 64 resting orders

Order make_order(std::uint64_t id, Side side, std::int64_t price, std::int64_t qty,
                  std::uint64_t seq = 0) {
    Order o{};
    o.id = OrderId{id};
    o.client = ClientId{1};
    o.symbol = SymbolId{1};
    o.side = side;
    o.type = OrderType::Limit;
    o.tif = TimeInForce::Day;
    o.price = Price{price};
    o.quantity = Quantity{qty};
    o.remaining = Quantity{qty};
    o.sequence = Sequence{seq};
    o.timestamp = Nanos{0};
    return o;
}

// Setup helper: seed the book with an order expected to succeed. Asserts
// success (rather than silently discarding the [[nodiscard]] result) so a
// broken test fixture fails loudly at the setup line, not several lines
// later as a confusing assertion on book state.
void must_add(TestBook& book, std::uint64_t id, Side side, std::int64_t price, std::int64_t qty,
              std::uint64_t seq = 0) {
    REQUIRE(book.add(make_order(id, side, price, qty, seq)).has_value());
}
void must_cancel(TestBook& book, std::uint64_t id) {
    REQUIRE(book.cancel(OrderId{id}).has_value());
}

}  // namespace

// --- basic add / query -----------------------------------------------------

TEST_CASE("OrderBook: add a single bid becomes best_bid", "[order_book]") {
    TestBook book(SymbolId{1}, Price{1000});
    auto result = book.add(make_order(1, Side::Buy, 1050, 100));
    REQUIRE(result.has_value());
    REQUIRE(*result == OrderId{1});

    REQUIRE(book.best_bid() == Price{1050});
    REQUIRE_FALSE(book.best_ask().has_value());
    REQUIRE(book.order_count() == 1);

    const auto* lvl = book.level_at(Side::Buy, Price{1050});
    REQUIRE(lvl != nullptr);
    REQUIRE(lvl->order_count == 1);
    REQUIRE(lvl->total_quantity.value() == 100);
}

TEST_CASE("OrderBook: best_bid picks the highest price, best_ask the lowest", "[order_book]") {
    TestBook book(SymbolId{1}, Price{1000});
    must_add(book, 1, Side::Buy, 1010, 10);
    must_add(book, 2, Side::Buy, 1050, 10);  // higher bid
    must_add(book, 3, Side::Buy, 1030, 10);
    must_add(book, 4, Side::Sell, 1090, 10);
    must_add(book, 5, Side::Sell, 1070, 10);  // lower ask
    must_add(book, 6, Side::Sell, 1080, 10);

    REQUIRE(book.best_bid() == Price{1050});
    REQUIRE(book.best_ask() == Price{1070});
}

TEST_CASE("OrderBook: FIFO order within a price level is preserved", "[order_book]") {
    TestBook book(SymbolId{1}, Price{1000});
    must_add(book, 1, Side::Buy, 1050, 10, 1);
    must_add(book, 2, Side::Buy, 1050, 20, 2);
    must_add(book, 3, Side::Buy, 1050, 30, 3);

    const OrderNode* front = book.front_at(Side::Buy, Price{1050});
    REQUIRE(front != nullptr);
    REQUIRE(front->order.id == OrderId{1});
    REQUIRE(front->next->order.id == OrderId{2});
    REQUIRE(front->next->next->order.id == OrderId{3});
    REQUIRE(front->next->next->next == nullptr);

    const auto* lvl = book.level_at(Side::Buy, Price{1050});
    REQUIRE(lvl->total_quantity.value() == 60);
    REQUIRE(lvl->order_count == 3);
}

// --- cancel ------------------------------------------------------------

TEST_CASE("OrderBook: cancel removes the order and updates aggregates", "[order_book]") {
    TestBook book(SymbolId{1}, Price{1000});
    must_add(book, 1, Side::Buy, 1050, 10);
    must_add(book, 2, Side::Buy, 1050, 20);

    auto result = book.cancel(OrderId{1});
    REQUIRE(result.has_value());
    REQUIRE(book.find(OrderId{1}) == nullptr);
    REQUIRE(book.order_count() == 1);

    const auto* lvl = book.level_at(Side::Buy, Price{1050});
    REQUIRE(lvl->order_count == 1);
    REQUIRE(lvl->total_quantity.value() == 20);
    REQUIRE(lvl->head->order.id == OrderId{2});
}

TEST_CASE("OrderBook: cancel of an unknown id fails with NotFound", "[order_book]") {
    TestBook book(SymbolId{1}, Price{1000});
    auto result = book.cancel(OrderId{999});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == CancelError::NotFound);
}

TEST_CASE("OrderBook: cancelling the sole order at the best price relocates best_bid",
          "[order_book]") {
    TestBook book(SymbolId{1}, Price{1000});
    must_add(book, 1, Side::Buy, 1030, 10);
    must_add(book, 2, Side::Buy, 1050, 10);  // best

    REQUIRE(book.best_bid() == Price{1050});
    must_cancel(book, 2);
    REQUIRE(book.best_bid() == Price{1030});
    must_cancel(book, 1);
    REQUIRE_FALSE(book.best_bid().has_value());
}

// --- reduce_quantity (fills) ---------------------------------------------

TEST_CASE("OrderBook: partial fill shrinks remaining but keeps FIFO position", "[order_book]") {
    TestBook book(SymbolId{1}, Price{1000});
    must_add(book, 1, Side::Buy, 1050, 100, 1);
    must_add(book, 2, Side::Buy, 1050, 50, 2);

    auto result = book.reduce_quantity(OrderId{1}, Quantity{40});
    REQUIRE(result.has_value());

    const auto* node1 = book.find(OrderId{1});
    REQUIRE(node1 != nullptr);
    REQUIRE(node1->order.remaining.value() == 60);

    const auto* lvl = book.level_at(Side::Buy, Price{1050});
    REQUIRE(lvl->total_quantity.value() == 110);  // 60 + 50
    REQUIRE(lvl->head->order.id == OrderId{1});   // still first in line
}

TEST_CASE("OrderBook: fill equal to remaining fully removes the order", "[order_book]") {
    TestBook book(SymbolId{1}, Price{1000});
    must_add(book, 1, Side::Buy, 1050, 100);

    auto result = book.reduce_quantity(OrderId{1}, Quantity{100});
    REQUIRE(result.has_value());
    REQUIRE(book.find(OrderId{1}) == nullptr);
    REQUIRE(book.order_count() == 0);
    REQUIRE_FALSE(book.best_bid().has_value());
}

TEST_CASE("OrderBook: reduce_quantity rejects invalid fill amounts", "[order_book]") {
    TestBook book(SymbolId{1}, Price{1000});
    must_add(book, 1, Side::Buy, 1050, 100);

    REQUIRE(book.reduce_quantity(OrderId{1}, Quantity{0}).error() == ReduceError::InvalidFillAmount);
    REQUIRE(book.reduce_quantity(OrderId{1}, Quantity{-5}).error() == ReduceError::InvalidFillAmount);
    REQUIRE(book.reduce_quantity(OrderId{1}, Quantity{101}).error() ==
            ReduceError::InvalidFillAmount);
    REQUIRE(book.reduce_quantity(OrderId{999}, Quantity{1}).error() == ReduceError::NotFound);
}

// --- replace -------------------------------------------------------------

TEST_CASE("OrderBook: replace moves an order to a new price and loses priority", "[order_book]") {
    TestBook book(SymbolId{1}, Price{1000});
    must_add(book, 1, Side::Buy, 1050, 100, 1);
    must_add(book, 2, Side::Buy, 1060, 50, 2);

    auto result = book.replace(OrderId{1}, Price{1060}, Quantity{75}, Sequence{99}, Nanos{123});
    REQUIRE(result.has_value());

    REQUIRE(book.level_at(Side::Buy, Price{1050})->order_count == 0);
    const auto* lvl = book.level_at(Side::Buy, Price{1060});
    REQUIRE(lvl->order_count == 2);
    REQUIRE(lvl->head->order.id == OrderId{2});   // original occupant keeps priority
    REQUIRE(lvl->tail->order.id == OrderId{1});   // replaced order goes to the back
    REQUIRE(lvl->tail->order.remaining.value() == 75);
    REQUIRE(lvl->tail->order.sequence == Sequence{99});
}

TEST_CASE("OrderBook: replace at the same price still loses priority", "[order_book]") {
    TestBook book(SymbolId{1}, Price{1000});
    must_add(book, 1, Side::Buy, 1050, 100, 1);
    must_add(book, 2, Side::Buy, 1050, 50, 2);

    REQUIRE(book.replace(OrderId{1}, Price{1050}, Quantity{60}, Sequence{5}, Nanos{0}).has_value());

    const auto* lvl = book.level_at(Side::Buy, Price{1050});
    REQUIRE(lvl->head->order.id == OrderId{2});
    REQUIRE(lvl->tail->order.id == OrderId{1});
    REQUIRE(lvl->tail->order.remaining.value() == 60);
}

TEST_CASE("OrderBook: replace validates before mutating anything", "[order_book]") {
    TestBook book(SymbolId{1}, Price{1000});
    must_add(book, 1, Side::Buy, 1050, 100, 1);

    REQUIRE(book.replace(OrderId{999}, Price{1050}, Quantity{10}, Sequence{1}, Nanos{0}).error() ==
            ReplaceError::NotFound);
    REQUIRE(book.replace(OrderId{1}, Price{1050}, Quantity{0}, Sequence{1}, Nanos{0}).error() ==
            ReplaceError::InvalidQuantity);
    REQUIRE(book.replace(OrderId{1}, Price{99999}, Quantity{10}, Sequence{1}, Nanos{0}).error() ==
            ReplaceError::PriceOutOfRange);

    // None of the rejected replaces should have touched the original order.
    const auto* node = book.find(OrderId{1});
    REQUIRE(node != nullptr);
    REQUIRE(node->order.price == Price{1050});
    REQUIRE(node->order.remaining.value() == 100);
}

// --- rejection paths -------------------------------------------------------

TEST_CASE("OrderBook: rejects a price outside the ladder's representable range",
          "[order_book]") {
    TestBook book(SymbolId{1}, Price{1000});
    REQUIRE(book.add(make_order(1, Side::Buy, 999, 10)).error() == AddError::PriceOutOfRange);
    REQUIRE(book.add(make_order(2, Side::Buy, 1200, 10)).error() == AddError::PriceOutOfRange);
    REQUIRE(book.add(make_order(3, Side::Buy, 1000, 10)).has_value());  // inclusive lower bound
    REQUIRE(book.add(make_order(4, Side::Buy, 1199, 10)).has_value());  // inclusive upper bound
}

TEST_CASE("OrderBook: rejects a duplicate OrderId without disturbing the original",
          "[order_book]") {
    TestBook book(SymbolId{1}, Price{1000});
    must_add(book, 1, Side::Buy, 1050, 100);
    auto result = book.add(make_order(1, Side::Buy, 1060, 50));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == AddError::DuplicateOrderId);

    const auto* node = book.find(OrderId{1});
    REQUIRE(node->order.price == Price{1050});
    REQUIRE(node->order.remaining.value() == 100);
    REQUIRE(book.order_count() == 1);
}

TEST_CASE("OrderBook: rejects a Market order from resting", "[order_book]") {
    TestBook book(SymbolId{1}, Price{1000});
    Order o = make_order(1, Side::Buy, 1050, 100);
    o.type = OrderType::Market;
    REQUIRE(book.add(o).error() == AddError::MarketOrderCannotRest);
}

TEST_CASE("OrderBook: rejects non-positive quantity", "[order_book]") {
    TestBook book(SymbolId{1}, Price{1000});
    REQUIRE(book.add(make_order(1, Side::Buy, 1050, 0)).error() == AddError::InvalidQuantity);
    REQUIRE(book.add(make_order(2, Side::Buy, 1050, -10)).error() == AddError::InvalidQuantity);
}

TEST_CASE("OrderBook: pool exhaustion is reported, not UB", "[order_book]") {
    OrderBook<200, 2> tiny(SymbolId{1}, Price{1000});
    REQUIRE(tiny.add(make_order(1, Side::Buy, 1050, 10)).has_value());
    REQUIRE(tiny.add(make_order(2, Side::Buy, 1051, 10)).has_value());
    auto result = tiny.add(make_order(3, Side::Buy, 1052, 10));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == AddError::PoolExhausted);
}

// --- best-price relocation across many levels (exercises the bitmap) -----

TEST_CASE("OrderBook: best_bid relocates correctly through many scattered cancellations",
          "[order_book]") {
    TestBook book(SymbolId{1}, Price{1000});
    const std::vector<std::int64_t> prices = {1005, 1150, 1030, 1199, 1000, 1080, 1064, 1177};
    for (std::size_t i = 0; i < prices.size(); ++i) {
        must_add(book, i + 1, Side::Buy, prices[i], 10);
    }

    std::vector<std::int64_t> sorted_desc = prices;
    std::sort(sorted_desc.begin(), sorted_desc.end(), std::greater<>());

    for (std::size_t i = 0; i < sorted_desc.size(); ++i) {
        REQUIRE(book.best_bid() == Price{sorted_desc[i]});
        // cancel whichever order currently sits at the current best price
        const auto* front = book.front_at(Side::Buy, Price{sorted_desc[i]});
        REQUIRE(front != nullptr);
        must_cancel(book, front->order.id.value());
    }
    REQUIRE_FALSE(book.best_bid().has_value());
}

TEST_CASE("OrderBook: best_ask relocates correctly through many scattered cancellations",
          "[order_book]") {
    TestBook book(SymbolId{1}, Price{1000});
    const std::vector<std::int64_t> prices = {1005, 1150, 1030, 1199, 1000, 1080, 1064, 1177};
    for (std::size_t i = 0; i < prices.size(); ++i) {
        must_add(book, i + 1, Side::Sell, prices[i], 10);
    }

    std::vector<std::int64_t> sorted_asc = prices;
    std::sort(sorted_asc.begin(), sorted_asc.end());

    for (std::size_t i = 0; i < sorted_asc.size(); ++i) {
        REQUIRE(book.best_ask() == Price{sorted_asc[i]});
        const auto* front = book.front_at(Side::Sell, Price{sorted_asc[i]});
        REQUIRE(front != nullptr);
        must_cancel(book, front->order.id.value());
    }
    REQUIRE_FALSE(book.best_ask().has_value());
}

TEST_CASE("OrderBook: order_count tracks adds/cancels/fills/replaces precisely", "[order_book]") {
    TestBook book(SymbolId{1}, Price{1000});
    REQUIRE(book.order_count() == 0);
    must_add(book, 1, Side::Buy, 1050, 100);
    must_add(book, 2, Side::Buy, 1050, 50);
    REQUIRE(book.order_count() == 2);

    must_cancel(book, 1);
    REQUIRE(book.order_count() == 1);

    must_add(book, 3, Side::Sell, 1060, 30);
    REQUIRE(book.order_count() == 2);

    REQUIRE(book.reduce_quantity(OrderId{2}, Quantity{50}).has_value());  // fully filled -> removed
    REQUIRE(book.order_count() == 1);

    REQUIRE(book.replace(OrderId{3}, Price{1070}, Quantity{20}, Sequence{1}, Nanos{0}).has_value());
    REQUIRE(book.order_count() == 1);  // replace is cancel+add of the same logical order
}
