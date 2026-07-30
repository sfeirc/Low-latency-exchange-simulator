#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "jane/marketdata/publisher.hpp"
#include "jane/marketdata/sinks.hpp"
#include "jane/matching/matching_engine.hpp"

using namespace jane;
using namespace jane::marketdata;
using namespace jane::matching;
using namespace jane::protocol;

namespace {

using Engine = MatchingEngine<400, 128>;

Order limit(std::uint64_t id, Side side, std::int64_t price, std::int64_t qty) {
    Order o{};
    o.id = OrderId{id};
    o.client = ClientId{1};
    o.symbol = SymbolId{7};
    o.side = side;
    o.type = OrderType::Limit;
    o.tif = TimeInForce::Day;
    o.price = Price{price};
    o.quantity = Quantity{qty};
    o.remaining = Quantity{qty};
    return o;
}

}  // namespace

TEST_CASE("MarketDataPublisher: publish_trade encodes a decodable TradeMessage", "[marketdata]") {
    InMemorySink sink;
    MarketDataPublisher<InMemorySink> pub(sink);

    const Fill fill{.match_id = 42,
                     .resting_order_id = OrderId{1},
                     .resting_client_id = ClientId{1},
                     .aggressor_order_id = OrderId{2},
                     .aggressor_client_id = ClientId{2},
                     .symbol = SymbolId{7},
                     .price = Price{1050},
                     .quantity = Quantity{25},
                     .aggressor_side = Side::Buy,
                     .resting_fully_filled = true};
    pub.publish_trade(SymbolId{7}, fill);

    const auto result = decode<TradeMessage>(sink.data());
    REQUIRE(result.status == DecodeStatus::Ok);
    REQUIRE(result.payload.match_id == 42);
    REQUIRE(result.payload.price == 1050);
    REQUIRE(result.payload.quantity == 25);
    REQUIRE(result.payload.symbol_id == 7);
    REQUIRE(result.payload.aggressor_side == Side::Buy);
    // No client identity in the public trade print, by design (see
    // TradeMessage's comment) — nothing here to assert against, which is
    // itself the point: the struct doesn't have client fields to check.
}

TEST_CASE("MarketDataPublisher: publish_level_update reflects Update vs. Delete correctly",
          "[marketdata]") {
    InMemorySink sink;
    MarketDataPublisher<InMemorySink> pub(sink);

    book::PriceLevel occupied;
    occupied.total_quantity = Quantity{150};
    occupied.order_count = 3;
    pub.publish_level_update(SymbolId{7}, Side::Buy, Price{1000}, occupied);

    book::PriceLevel empty;
    pub.publish_level_update(SymbolId{7}, Side::Sell, Price{1010}, empty);

    auto span = sink.data();
    const auto first = decode<BookDeltaMessage>(span);
    REQUIRE(first.status == DecodeStatus::Ok);
    REQUIRE(first.payload.action == DeltaAction::Update);
    REQUIRE(first.payload.aggregate_quantity == 150);
    REQUIRE(first.payload.order_count == 3);
    REQUIRE(first.payload.side == Side::Buy);

    const auto second = decode<BookDeltaMessage>(span.subspan(first.consumed));
    REQUIRE(second.status == DecodeStatus::Ok);
    REQUIRE(second.payload.action == DeltaAction::Delete);
    REQUIRE(second.payload.aggregate_quantity == 0);
    REQUIRE(second.payload.side == Side::Sell);
}

TEST_CASE("MarketDataPublisher: sequence numbers are strictly monotonic across message types",
          "[marketdata]") {
    InMemorySink sink;
    MarketDataPublisher<InMemorySink> pub(sink);
    REQUIRE(pub.next_sequence() == 1);

    const Fill fill{.match_id = 1,
                     .resting_order_id = OrderId{1},
                     .resting_client_id = ClientId{1},
                     .aggressor_order_id = OrderId{2},
                     .aggressor_client_id = ClientId{2},
                     .symbol = SymbolId{7},
                     .price = Price{100},
                     .quantity = Quantity{1},
                     .aggressor_side = Side::Buy,
                     .resting_fully_filled = true};
    pub.publish_trade(SymbolId{7}, fill);
    REQUIRE(pub.next_sequence() == 2);

    book::PriceLevel lvl;
    lvl.order_count = 1;
    lvl.total_quantity = Quantity{10};
    pub.publish_level_update(SymbolId{7}, Side::Buy, Price{100}, lvl);
    REQUIRE(pub.next_sequence() == 3);

    auto span = sink.data();
    const auto t = decode<TradeMessage>(span);
    REQUIRE(t.payload.sequence == 1);
    const auto d = decode<BookDeltaMessage>(span.subspan(t.consumed));
    REQUIRE(d.payload.sequence == 2);
}

TEST_CASE("MarketDataPublisher: snapshot reflects live book state, best-first, capped at depth",
          "[marketdata]") {
    Engine engine(SymbolId{7}, Price{1000});
    std::vector<Fill> fills;
    for (auto [id, price] : {std::pair{1, 1010}, {2, 1030}, {3, 1020}, {4, 1005}}) {
        REQUIRE(engine.submit(limit(static_cast<std::uint64_t>(id), Side::Buy, price, 10), fills)
                    .reject_reason == RejectReason::None);
    }
    for (auto [id, price] : {std::pair{5, 1080}, {6, 1060}, {7, 1070}}) {
        REQUIRE(engine.submit(limit(static_cast<std::uint64_t>(id), Side::Sell, price, 20), fills)
                    .reject_reason == RejectReason::None);
    }

    InMemorySink sink;
    MarketDataPublisher<InMemorySink> pub(sink);
    pub.publish_snapshot(SymbolId{7}, engine.book(), /*depth=*/2);

    const auto result = decode_snapshot(sink.data());
    REQUIRE(result.status == DecodeStatus::Ok);
    REQUIRE(result.header.symbol_id == 7);
    REQUIRE(result.bids.size() == 2);  // capped, even though 4 bid levels exist
    REQUIRE(result.asks.size() == 2);

    REQUIRE(result.bids[0].price == 1030);  // best bid first
    REQUIRE(result.bids[1].price == 1020);
    REQUIRE(result.asks[0].price == 1060);  // best (lowest) ask first
    REQUIRE(result.asks[1].price == 1070);
    REQUIRE(result.bids[0].aggregate_quantity == 10);
    REQUIRE(result.asks[0].aggregate_quantity == 20);
}

TEST_CASE("MarketDataPublisher: snapshot on an empty book has zero levels, not an error",
          "[marketdata]") {
    Engine engine(SymbolId{7}, Price{1000});
    InMemorySink sink;
    MarketDataPublisher<InMemorySink> pub(sink);
    pub.publish_snapshot(SymbolId{7}, engine.book(), 10);

    const auto result = decode_snapshot(sink.data());
    REQUIRE(result.status == DecodeStatus::Ok);
    REQUIRE(result.bids.empty());
    REQUIRE(result.asks.empty());
}

TEST_CASE("MarketDataPublisher: end-to-end trade publication matches actual matching-engine fills",
          "[marketdata]") {
    Engine engine(SymbolId{7}, Price{1000});
    std::vector<Fill> fills;
    REQUIRE(engine.submit(limit(1, Side::Sell, 1050, 30), fills).reject_reason ==
            RejectReason::None);

    fills.clear();
    auto result = engine.submit(limit(2, Side::Buy, 1050, 30), fills);
    REQUIRE(result.reject_reason == RejectReason::None);
    REQUIRE(fills.size() == 1);

    InMemorySink sink;
    MarketDataPublisher<InMemorySink> pub(sink);
    for (const auto& f : fills) {
        pub.publish_trade(SymbolId{7}, f);
    }

    const auto decoded = decode<TradeMessage>(sink.data());
    REQUIRE(decoded.status == DecodeStatus::Ok);
    REQUIRE(decoded.payload.price == fills[0].price.value());
    REQUIRE(decoded.payload.quantity == fills[0].quantity.value());
    REQUIRE(decoded.payload.match_id == fills[0].match_id);
}

TEST_CASE("MarketDataPublisher: FileSink round-trips the same bytes as InMemorySink", "[marketdata]") {
    const auto path = std::filesystem::temp_directory_path() / "jane_marketdata_test.bin";

    InMemorySink mem_sink;
    MarketDataPublisher<InMemorySink> mem_pub(mem_sink);
    const Fill fill{.match_id = 99,
                     .resting_order_id = OrderId{1},
                     .resting_client_id = ClientId{1},
                     .aggressor_order_id = OrderId{2},
                     .aggressor_client_id = ClientId{2},
                     .symbol = SymbolId{7},
                     .price = Price{500},
                     .quantity = Quantity{5},
                     .aggressor_side = Side::Sell,
                     .resting_fully_filled = false};
    mem_pub.publish_trade(SymbolId{7}, fill);

    {
        FileSink file_sink(path);
        MarketDataPublisher<FileSink> file_pub(file_sink);
        file_pub.publish_trade(SymbolId{7}, fill);
    }  // FileSink destructor flushes/closes

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    REQUIRE(in.good());
    const auto file_size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    std::vector<char> file_chars(file_size);
    in.read(file_chars.data(), static_cast<std::streamsize>(file_size));
    REQUIRE(in.good());

    REQUIRE(file_size == mem_sink.size());
    REQUIRE(std::memcmp(file_chars.data(), mem_sink.data().data(), file_size) == 0);

    std::filesystem::remove(path);
}
