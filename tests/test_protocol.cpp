#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <random>
#include <vector>

#include "jane/protocol/codec.hpp"
#include "jane/protocol/messages.hpp"

using namespace jane;
using namespace jane::protocol;

// --- round trips ----------------------------------------------------------

TEST_CASE("NewOrderMessage round-trips through encode/decode", "[protocol]") {
    const NewOrderMessage msg{
        .order_id = 42,
        .price = 10'050,
        .quantity = 100,
        .client_id = 7,
        .symbol_id = 1,
        .side = Side::Buy,
        .order_type = OrderType::Limit,
        .time_in_force = TimeInForce::Day,
    };
    std::array<std::byte, 128> buf{};
    const std::size_t written = encode(std::span(buf), msg);
    REQUIRE(written == sizeof(MessageHeader) + sizeof(NewOrderMessage));

    const auto result = decode<NewOrderMessage>(std::span(buf).first(written));
    REQUIRE(result.status == DecodeStatus::Ok);
    REQUIRE(result.consumed == written);
    REQUIRE(result.payload.order_id == msg.order_id);
    REQUIRE(result.payload.client_id == msg.client_id);
    REQUIRE(result.payload.price == msg.price);
    REQUIRE(result.payload.quantity == msg.quantity);
    REQUIRE(result.payload.symbol_id == msg.symbol_id);
    REQUIRE(result.payload.side == msg.side);
    REQUIRE(result.payload.order_type == msg.order_type);
    REQUIRE(result.payload.time_in_force == msg.time_in_force);
}

TEST_CASE("CancelOrderMessage round-trips", "[protocol]") {
    const CancelOrderMessage msg{.order_id = 99, .client_id = 3, .symbol_id = 2};
    std::array<std::byte, 64> buf{};
    const std::size_t written = encode(std::span(buf), msg);
    const auto result = decode<CancelOrderMessage>(std::span(buf).first(written));
    REQUIRE(result.status == DecodeStatus::Ok);
    REQUIRE(result.payload.order_id == 99);
    REQUIRE(result.payload.client_id == 3);
    REQUIRE(result.payload.symbol_id == 2);
}

TEST_CASE("ReplaceOrderMessage round-trips", "[protocol]") {
    const ReplaceOrderMessage msg{
        .order_id = 5, .new_price = 200, .new_quantity = 50, .client_id = 6, .symbol_id = 1};
    std::array<std::byte, 64> buf{};
    const std::size_t written = encode(std::span(buf), msg);
    const auto result = decode<ReplaceOrderMessage>(std::span(buf).first(written));
    REQUIRE(result.status == DecodeStatus::Ok);
    REQUIRE(result.payload.new_price == 200);
    REQUIRE(result.payload.new_quantity == 50);
}

TEST_CASE("ExecutionReportMessage round-trips", "[protocol]") {
    const ExecutionReportMessage msg{
        .order_id = 1,
        .match_id = 3,
        .price = 10'000,
        .last_quantity = 25,
        .leaves_quantity = 75,
        .client_id = 2,
        .symbol_id = 1,
        .exec_type = ExecType::PartialFill,
        .reject_reason = RejectReason::None,
    };
    std::array<std::byte, 128> buf{};
    const std::size_t written = encode(std::span(buf), msg);
    const auto result = decode<ExecutionReportMessage>(std::span(buf).first(written));
    REQUIRE(result.status == DecodeStatus::Ok);
    REQUIRE(result.payload.exec_type == ExecType::PartialFill);
    REQUIRE(result.payload.leaves_quantity == 75);
}

TEST_CASE("TradeMessage and BookDeltaMessage round-trip", "[protocol]") {
    const TradeMessage trade{.sequence = 1,
                              .match_id = 1,
                              .price = 10'000,
                              .quantity = 10,
                              .symbol_id = 1,
                              .aggressor_side = Side::Sell};
    std::array<std::byte, 64> tbuf{};
    const std::size_t twritten = encode(std::span(tbuf), trade);
    const auto tresult = decode<TradeMessage>(std::span(tbuf).first(twritten));
    REQUIRE(tresult.status == DecodeStatus::Ok);
    REQUIRE(tresult.payload.aggressor_side == Side::Sell);

    const BookDeltaMessage delta{.sequence = 1,
                                  .price = 10'000,
                                  .aggregate_quantity = 500,
                                  .symbol_id = 1,
                                  .order_count = 4,
                                  .side = Side::Buy,
                                  .action = DeltaAction::Update};
    std::array<std::byte, 64> dbuf{};
    const std::size_t dwritten = encode(std::span(dbuf), delta);
    const auto dresult = decode<BookDeltaMessage>(std::span(dbuf).first(dwritten));
    REQUIRE(dresult.status == DecodeStatus::Ok);
    REQUIRE(dresult.payload.action == DeltaAction::Update);
    REQUIRE(dresult.payload.order_count == 4);
}

// --- framing edge cases ----------------------------------------------------

TEST_CASE("encode reports 0 and writes nothing when the buffer is too small", "[protocol]") {
    const CancelOrderMessage msg{.order_id = 1, .client_id = 1, .symbol_id = 1};
    std::array<std::byte, 4> tiny{};  // smaller than header+payload
    REQUIRE(encode(std::span(tiny), msg) == 0);
}

TEST_CASE("decode reports Incomplete on a truncated buffer, at every truncation point",
          "[protocol]") {
    const NewOrderMessage msg{.order_id = 1,
                               .price = 100,
                               .quantity = 10,
                               .client_id = 1,
                               .symbol_id = 1,
                               .side = Side::Buy,
                               .order_type = OrderType::Limit,
                               .time_in_force = TimeInForce::Day};
    std::array<std::byte, 128> buf{};
    const std::size_t written = encode(std::span(buf), msg);

    for (std::size_t len = 0; len < written; ++len) {
        const auto result = decode<NewOrderMessage>(std::span(buf).first(len));
        REQUIRE(result.status == DecodeStatus::Incomplete);
    }
    REQUIRE(decode<NewOrderMessage>(std::span(buf).first(written)).status == DecodeStatus::Ok);
}

TEST_CASE("decode reports UnexpectedType when Payload doesn't match the header", "[protocol]") {
    const CancelOrderMessage msg{.order_id = 1, .client_id = 1, .symbol_id = 1};
    std::array<std::byte, 64> buf{};
    const std::size_t written = encode(std::span(buf), msg);
    const auto result = decode<ReplaceOrderMessage>(std::span(buf).first(written));
    REQUIRE(result.status == DecodeStatus::UnexpectedType);
}

TEST_CASE("decode reports Malformed when header.length disagrees with the payload size",
          "[protocol]") {
    const CancelOrderMessage msg{.order_id = 1, .client_id = 1, .symbol_id = 1};
    std::array<std::byte, 64> buf{};
    std::size_t written = encode(std::span(buf), msg);

    MessageHeader header;
    std::memcpy(&header, buf.data(), sizeof(header));
    header.length += 1;  // corrupt: claim one extra byte of payload
    std::memcpy(buf.data(), &header, sizeof(header));

    const auto result = decode<CancelOrderMessage>(std::span(buf).first(written));
    REQUIRE(result.status == DecodeStatus::Malformed);
}

TEST_CASE("peek_header + dispatch parses a stream of mixed message types in order",
          "[protocol]") {
    std::vector<std::byte> stream(512);
    std::size_t offset = 0;
    offset += encode(std::span(stream).subspan(offset),
                      NewOrderMessage{.order_id = 1,
                                      .price = 100,
                                      .quantity = 10,
                                      .client_id = 1,
                                      .symbol_id = 1,
                                      .side = Side::Buy,
                                      .order_type = OrderType::Limit,
                                      .time_in_force = TimeInForce::Day});
    offset += encode(std::span(stream).subspan(offset),
                      CancelOrderMessage{.order_id = 1, .client_id = 1, .symbol_id = 1});
    offset += encode(std::span(stream).subspan(offset),
                      TradeMessage{.sequence = 1,
                                   .match_id = 1,
                                   .price = 100,
                                   .quantity = 10,
                                   .symbol_id = 1,
                                   .aggressor_side = Side::Buy});

    std::vector<MessageType> seen;
    std::size_t cursor = 0;
    while (cursor < offset) {
        auto span = std::span(stream).subspan(cursor, offset - cursor);
        const auto header = peek_header(span);
        REQUIRE(header.has_value());
        seen.push_back(header->type);
        switch (header->type) {
            case MessageType::NewOrder:
                cursor += decode<NewOrderMessage>(span).consumed;
                break;
            case MessageType::CancelOrder:
                cursor += decode<CancelOrderMessage>(span).consumed;
                break;
            case MessageType::Trade:
                cursor += decode<TradeMessage>(span).consumed;
                break;
            default:
                FAIL("unexpected message type in stream");
        }
    }
    REQUIRE(seen == std::vector<MessageType>{MessageType::NewOrder, MessageType::CancelOrder,
                                              MessageType::Trade});
    REQUIRE(cursor == offset);
}

// --- snapshot (variable-length) -------------------------------------------

TEST_CASE("BookSnapshot round-trips with a mix of bid/ask level counts", "[protocol][snapshot]") {
    const std::vector<PriceLevelRecord> bids = {
        {.price = 100, .aggregate_quantity = 10, .order_count = 2},
        {.price = 99, .aggregate_quantity = 20, .order_count = 3},
    };
    const std::vector<PriceLevelRecord> asks = {
        {.price = 101, .aggregate_quantity = 5, .order_count = 1},
    };

    std::vector<std::byte> buf(4096);
    const std::size_t written = encode_snapshot(std::span(buf), 12345, 7, bids, asks);
    REQUIRE(written > 0);

    const auto result = decode_snapshot(std::span(buf).first(written));
    REQUIRE(result.status == DecodeStatus::Ok);
    REQUIRE(result.header.sequence == 12345);
    REQUIRE(result.header.symbol_id == 7);
    REQUIRE(result.bids.size() == 2);
    REQUIRE(result.asks.size() == 1);
    REQUIRE(result.bids[0].price == 100);
    REQUIRE(result.bids[1].price == 99);
    REQUIRE(result.asks[0].price == 101);
    REQUIRE(result.consumed == written);
}

TEST_CASE("BookSnapshot round-trips with zero levels on one or both sides",
          "[protocol][snapshot]") {
    std::vector<std::byte> buf(256);
    SECTION("empty both sides") {
        const std::size_t written = encode_snapshot(std::span(buf), 1, 1, {}, {});
        const auto result = decode_snapshot(std::span(buf).first(written));
        REQUIRE(result.status == DecodeStatus::Ok);
        REQUIRE(result.bids.empty());
        REQUIRE(result.asks.empty());
    }
    SECTION("empty asks only") {
        const std::vector<PriceLevelRecord> bids = {
            {.price = 1, .aggregate_quantity = 1, .order_count = 1}};
        const std::size_t written = encode_snapshot(std::span(buf), 1, 1, bids, {});
        const auto result = decode_snapshot(std::span(buf).first(written));
        REQUIRE(result.status == DecodeStatus::Ok);
        REQUIRE(result.bids.size() == 1);
        REQUIRE(result.asks.empty());
    }
}

TEST_CASE("BookSnapshot handles a larger depth (64 levels per side)", "[protocol][snapshot]") {
    std::vector<PriceLevelRecord> bids;
    std::vector<PriceLevelRecord> asks;
    for (int i = 0; i < 64; ++i) {
        bids.push_back({.price = 100 - i, .aggregate_quantity = i, .order_count = 1});
        asks.push_back({.price = 101 + i, .aggregate_quantity = i, .order_count = 1});
    }
    std::vector<std::byte> buf(8192);
    const std::size_t written = encode_snapshot(std::span(buf), 1, 1, bids, asks);
    REQUIRE(written > 0);
    const auto result = decode_snapshot(std::span(buf).first(written));
    REQUIRE(result.status == DecodeStatus::Ok);
    REQUIRE(result.bids.size() == 64);
    REQUIRE(result.asks.size() == 64);
    REQUIRE(result.bids[63].price == 100 - 63);
    REQUIRE(result.asks[63].price == 101 + 63);
}

// --- fuzz: decode must never crash on arbitrary bytes ----------------------

TEST_CASE("decode<T> and decode_snapshot never misbehave on random/truncated garbage",
          "[protocol][fuzz]") {
    std::mt19937_64 rng(0xC0FFEE);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<std::size_t> len_dist(0, 200);

    for (int iter = 0; iter < 20'000; ++iter) {
        std::vector<std::byte> garbage(len_dist(rng));
        for (auto& b : garbage) {
            b = static_cast<std::byte>(byte_dist(rng));
        }
        const std::span<const std::byte> span(garbage);

        auto r1 = decode<NewOrderMessage>(span);
        if (r1.status == DecodeStatus::Ok) REQUIRE(r1.consumed <= span.size());

        auto r2 = decode<ExecutionReportMessage>(span);
        if (r2.status == DecodeStatus::Ok) REQUIRE(r2.consumed <= span.size());

        auto r3 = decode_snapshot(span);
        if (r3.status == DecodeStatus::Ok) {
            REQUIRE(r3.consumed <= span.size());
            REQUIRE(r3.bids.size() == r3.header.bid_level_count);
            REQUIRE(r3.asks.size() == r3.header.ask_level_count);
        }
    }
}
