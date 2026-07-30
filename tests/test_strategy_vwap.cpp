#include <catch2/catch_test_macros.hpp>

#include <numeric>

#include "jane/correctness/invariants.hpp"
#include "jane/marketdata/sinks.hpp"
#include "jane/replay/replay_engine.hpp"
#include "jane/strategy/vwap.hpp"

using namespace jane;
using namespace jane::strategy;

TEST_CASE("VwapExecutor: slice sizes always sum exactly to the parent quantity",
          "[strategy][vwap]") {
    for (std::int64_t total : {1LL, 7LL, 100LL, 1001LL, 99999LL}) {
        for (int slices : {1, 2, 3, 10, 37}) {
            VwapExecutor vwap(VwapConfig{.symbol = SymbolId{1},
                                          .client = ClientId{1},
                                          .side = Side::Buy,
                                          .total_quantity = total,
                                          .num_slices = slices,
                                          .limit_price = Price{1000}},
                               1);
            const auto sum = std::accumulate(vwap.slice_sizes().begin(), vwap.slice_sizes().end(),
                                              std::int64_t{0});
            REQUIRE(sum == total);
            REQUIRE(static_cast<int>(vwap.slice_sizes().size()) == slices);
        }
    }
}

TEST_CASE("VwapExecutor: the curve is heavier at the start/end than in the middle",
          "[strategy][vwap]") {
    VwapExecutor vwap(VwapConfig{.symbol = SymbolId{1},
                                  .client = ClientId{1},
                                  .side = Side::Buy,
                                  .total_quantity = 10000,
                                  .num_slices = 11,
                                  .limit_price = Price{1000}},
                       1);
    const auto& sizes = vwap.slice_sizes();
    const std::int64_t middle = sizes[5];
    REQUIRE(sizes.front() > middle);
    REQUIRE(sizes.back() > middle);
}

TEST_CASE("VwapExecutor: a single slice takes the entire parent quantity", "[strategy][vwap]") {
    VwapExecutor vwap(VwapConfig{.symbol = SymbolId{1},
                                  .client = ClientId{1},
                                  .side = Side::Sell,
                                  .total_quantity = 250,
                                  .num_slices = 1,
                                  .limit_price = Price{500}},
                       1);
    REQUIRE(vwap.slice_sizes().size() == 1);
    REQUIRE(vwap.slice_sizes()[0] == 250);
}

TEST_CASE("VwapExecutor: next_slice() is exhausted after exactly num_slices calls",
          "[strategy][vwap]") {
    VwapExecutor vwap(VwapConfig{.symbol = SymbolId{1},
                                  .client = ClientId{1},
                                  .side = Side::Buy,
                                  .total_quantity = 100,
                                  .num_slices = 4,
                                  .limit_price = Price{1000}},
                       1);
    int count = 0;
    while (!vwap.done()) {
        auto slice = vwap.next_slice();
        REQUIRE(slice.has_value());
        REQUIRE(slice->side == Side::Buy);
        REQUIRE(slice->symbol_id == 1);
        REQUIRE(slice->time_in_force == TimeInForce::IOC);
        REQUIRE(slice->price == 1000);
        ++count;
    }
    REQUIRE(count == 4);
    REQUIRE_FALSE(vwap.next_slice().has_value());
    REQUIRE(vwap.slices_sent() == 4);
}

TEST_CASE("VwapExecutor: order ids increment monotonically from the configured start",
          "[strategy][vwap]") {
    VwapExecutor vwap(VwapConfig{.symbol = SymbolId{1},
                                  .client = ClientId{1},
                                  .side = Side::Buy,
                                  .total_quantity = 100,
                                  .num_slices = 5,
                                  .limit_price = Price{1000}},
                       500);
    std::uint64_t expected = 500;
    while (!vwap.done()) {
        auto slice = vwap.next_slice();
        REQUIRE(slice->order_id == expected);
        ++expected;
    }
}

TEST_CASE("VwapExecutor: driven through a real pipeline against resting liquidity fills "
          "some or all slices without violating invariants",
          "[strategy][vwap][integration]") {
    marketdata::InMemorySink sink;
    matching::MatchingEngine<400, 128> engine(SymbolId{1}, Price{800});
    risk::RiskEngine<64> risk(risk::Limits{.max_order_size = Quantity{1000},
                                            .max_position = Quantity{1000},
                                            .max_loss_per_client = PnL{-1'000'000}});
    marketdata::MarketDataPublisher<marketdata::InMemorySink> feed(sink);
    replay::ReplayEngine<400, 128, 64, marketdata::InMemorySink> pipeline(engine, risk, feed);
    replay::DeterministicClock clock;

    // Resting liquidity for the VWAP buyer to consume.
    Order resting{};
    resting.id = OrderId{1};
    resting.client = ClientId{2};
    resting.symbol = SymbolId{1};
    resting.side = Side::Sell;
    resting.type = OrderType::Limit;
    resting.tif = TimeInForce::Day;
    resting.price = Price{1000};
    resting.quantity = Quantity{500};
    resting.remaining = resting.quantity;
    std::vector<matching::Fill> fills;
    REQUIRE(engine.submit(resting, fills).reject_reason == RejectReason::None);

    VwapExecutor vwap(VwapConfig{.symbol = SymbolId{1},
                                  .client = ClientId{7},
                                  .side = Side::Buy,
                                  .total_quantity = 200,
                                  .num_slices = 8,
                                  .limit_price = Price{1000}},
                       100);

    while (!vwap.done()) {
        auto slice = vwap.next_slice();
        pipeline.process_new_order(*slice, clock.tick());
        const auto violations = jane::correctness::check_book_invariants(engine.book());
        REQUIRE(violations.empty());
    }

    REQUIRE(risk.position(ClientId{7}, SymbolId{1}).value() == 200);  // fully filled: ample liquidity
}
