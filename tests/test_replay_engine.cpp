#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "jane/marketdata/sinks.hpp"
#include "jane/protocol/codec.hpp"
#include "jane/replay/replay_engine.hpp"

using namespace jane;
using namespace jane::replay;
using namespace jane::marketdata;
using namespace jane::protocol;

namespace {

constexpr std::size_t kNumLevels = 400;
constexpr std::size_t kMaxOrders = 128;
constexpr std::size_t kMaxAccounts = 64;
using Replay = ReplayEngine<kNumLevels, kMaxOrders, kMaxAccounts, InMemorySink>;

risk::Limits generous_limits() {
    return risk::Limits{.max_order_size = Quantity{1'000'000},
                        .max_position = Quantity{1'000'000},
                        .max_loss_per_client = PnL{-1'000'000'000}};
}

NewOrderMessage new_order(std::uint64_t id, Side side, std::int64_t price, std::int64_t qty,
                           std::uint32_t client = 1) {
    return NewOrderMessage{.order_id = id,
                            .price = price,
                            .quantity = qty,
                            .client_id = client,
                            .symbol_id = 1,
                            .side = side,
                            .order_type = OrderType::Limit,
                            .time_in_force = TimeInForce::Day};
}

// Counts framed messages of each MessageType in a byte buffer by walking
// it with peek_header — a lightweight way to assert "an ExecutionReport
// and two BookDeltas were published" without fully decoding each one.
struct MessageCounts {
    int new_order = 0, cancel = 0, replace = 0, exec_report = 0, trade = 0, delta = 0, snapshot = 0;
};

MessageCounts count_messages(std::span<const std::byte> bytes) {
    MessageCounts counts;
    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        const auto header = peek_header(bytes.subspan(cursor));
        REQUIRE(header.has_value());
        const std::size_t total = sizeof(MessageHeader) + header->length;
        switch (header->type) {
            case MessageType::NewOrder: ++counts.new_order; break;
            case MessageType::CancelOrder: ++counts.cancel; break;
            case MessageType::ReplaceOrder: ++counts.replace; break;
            case MessageType::ExecutionReport: ++counts.exec_report; break;
            case MessageType::Trade: ++counts.trade; break;
            case MessageType::BookDelta: ++counts.delta; break;
            case MessageType::BookSnapshot: ++counts.snapshot; break;
        }
        cursor += total;
    }
    return counts;
}

}  // namespace

TEST_CASE("Replay: a non-crossing new order publishes one level update and one report",
          "[replay]") {
    InMemorySink sink;
    matching::MatchingEngine<kNumLevels, kMaxOrders> engine(SymbolId{1}, Price{1000});
    risk::RiskEngine<kMaxAccounts> risk(generous_limits());
    MarketDataPublisher<InMemorySink> feed(sink);
    Replay replay(engine, risk, feed);

    replay.process_new_order(new_order(1, Side::Buy, 1050, 100), Nanos{0});

    const auto counts = count_messages(sink.data());
    REQUIRE(counts.delta == 1);
    REQUIRE(counts.exec_report == 1);
    REQUIRE(counts.trade == 0);
    REQUIRE(engine.book().best_bid() == Price{1050});
}

TEST_CASE("Replay: a crossing order publishes a trade, both sides' level deltas, and reports",
          "[replay]") {
    InMemorySink sink;
    matching::MatchingEngine<kNumLevels, kMaxOrders> engine(SymbolId{1}, Price{1000});
    risk::RiskEngine<kMaxAccounts> risk(generous_limits());
    MarketDataPublisher<InMemorySink> feed(sink);
    Replay replay(engine, risk, feed);

    replay.process_new_order(new_order(1, Side::Sell, 1050, 100, /*client=*/1), Nanos{0});
    sink.clear();
    replay.process_new_order(new_order(2, Side::Buy, 1050, 100, /*client=*/2), Nanos{1});

    const auto counts = count_messages(sink.data());
    REQUIRE(counts.trade == 1);
    REQUIRE(counts.delta == 1);  // the resting ask's level, now empty
    REQUIRE(counts.exec_report == 1);

    const auto span = sink.data();
    std::size_t cursor = 0;
    const auto trade_header = peek_header(span);
    REQUIRE(trade_header->type == MessageType::Trade);
    const auto trade = decode<TradeMessage>(span);
    REQUIRE(trade.payload.price == 1050);
    REQUIRE(trade.payload.quantity == 100);
    cursor += trade.consumed;

    const auto delta = decode<BookDeltaMessage>(span.subspan(cursor));
    REQUIRE(delta.status == DecodeStatus::Ok);
    REQUIRE(delta.payload.action == DeltaAction::Delete);
    REQUIRE(delta.payload.side == Side::Sell);
}

TEST_CASE("Replay: risk rejection never reaches the book and publishes a Rejected report",
          "[replay]") {
    InMemorySink sink;
    matching::MatchingEngine<kNumLevels, kMaxOrders> engine(SymbolId{1}, Price{1000});
    risk::RiskEngine<kMaxAccounts> risk(
        risk::Limits{.max_order_size = Quantity{10},
                     .max_position = Quantity{1'000'000},
                     .max_loss_per_client = PnL{-1'000'000'000}});
    MarketDataPublisher<InMemorySink> feed(sink);
    Replay replay(engine, risk, feed);

    replay.process_new_order(new_order(1, Side::Buy, 1050, 100), Nanos{0});  // exceeds max_order_size

    REQUIRE(engine.book().order_count() == 0);
    const auto report = decode<ExecutionReportMessage>(sink.data());
    REQUIRE(report.status == DecodeStatus::Ok);
    REQUIRE(report.payload.exec_type == ExecType::Rejected);
    REQUIRE(report.payload.reject_reason == RejectReason::RiskMaxOrderSize);
}

TEST_CASE("Replay: cancel publishes a level update and a Cancelled report", "[replay]") {
    InMemorySink sink;
    matching::MatchingEngine<kNumLevels, kMaxOrders> engine(SymbolId{1}, Price{1000});
    risk::RiskEngine<kMaxAccounts> risk(generous_limits());
    MarketDataPublisher<InMemorySink> feed(sink);
    Replay replay(engine, risk, feed);

    replay.process_new_order(new_order(1, Side::Buy, 1050, 100), Nanos{0});
    sink.clear();
    replay.process_cancel(CancelOrderMessage{.order_id = 1, .client_id = 1, .symbol_id = 1});

    REQUIRE(engine.book().find(OrderId{1}) == nullptr);
    const auto counts = count_messages(sink.data());
    REQUIRE(counts.delta == 1);
    REQUIRE(counts.exec_report == 1);

    // publish_level runs before publish_report in process_cancel, so the
    // delta comes first on the wire — skip past it to reach the report.
    const auto span = sink.data();
    const auto delta = decode<BookDeltaMessage>(span);
    REQUIRE(delta.status == DecodeStatus::Ok);
    REQUIRE(delta.payload.action == DeltaAction::Delete);
    const auto report = decode<ExecutionReportMessage>(span.subspan(delta.consumed));
    REQUIRE(report.status == DecodeStatus::Ok);
    REQUIRE(report.payload.exec_type == ExecType::Cancelled);
}

TEST_CASE("Replay: cancelling an unknown order publishes a Rejected/UnknownOrder report, no crash",
          "[replay]") {
    InMemorySink sink;
    matching::MatchingEngine<kNumLevels, kMaxOrders> engine(SymbolId{1}, Price{1000});
    risk::RiskEngine<kMaxAccounts> risk(generous_limits());
    MarketDataPublisher<InMemorySink> feed(sink);
    Replay replay(engine, risk, feed);

    replay.process_cancel(CancelOrderMessage{.order_id = 999, .client_id = 1, .symbol_id = 1});

    const auto report = decode<ExecutionReportMessage>(sink.data());
    REQUIRE(report.status == DecodeStatus::Ok);
    REQUIRE(report.payload.exec_type == ExecType::Rejected);
    REQUIRE(report.payload.reject_reason == RejectReason::UnknownOrder);
}

TEST_CASE("Replay: replace publishes updates for both the old and new price levels", "[replay]") {
    InMemorySink sink;
    matching::MatchingEngine<kNumLevels, kMaxOrders> engine(SymbolId{1}, Price{1000});
    risk::RiskEngine<kMaxAccounts> risk(generous_limits());
    MarketDataPublisher<InMemorySink> feed(sink);
    Replay replay(engine, risk, feed);

    replay.process_new_order(new_order(1, Side::Buy, 1050, 100), Nanos{0});
    sink.clear();
    replay.process_replace(
        ReplaceOrderMessage{.order_id = 1, .new_price = 1060, .new_quantity = 50, .client_id = 1,
                            .symbol_id = 1},
        Nanos{1});

    const auto counts = count_messages(sink.data());
    REQUIRE(counts.delta == 2);  // old level (now empty) + new level
    REQUIRE(counts.exec_report == 1);
    REQUIRE(engine.book().level_at(Side::Buy, Price{1050})->order_count == 0);
    REQUIRE(engine.book().best_bid() == Price{1060});
}

TEST_CASE("Replay: a replace that crosses publishes a trade in addition to level updates",
          "[replay]") {
    InMemorySink sink;
    matching::MatchingEngine<kNumLevels, kMaxOrders> engine(SymbolId{1}, Price{1000});
    risk::RiskEngine<kMaxAccounts> risk(generous_limits());
    MarketDataPublisher<InMemorySink> feed(sink);
    Replay replay(engine, risk, feed);

    replay.process_new_order(new_order(1, Side::Buy, 1040, 50, /*client=*/1), Nanos{0});
    replay.process_new_order(new_order(2, Side::Sell, 1060, 30, /*client=*/2), Nanos{1});
    sink.clear();

    replay.process_replace(
        ReplaceOrderMessage{.order_id = 1, .new_price = 1070, .new_quantity = 50, .client_id = 1,
                            .symbol_id = 1},
        Nanos{2});

    const auto counts = count_messages(sink.data());
    REQUIRE(counts.trade == 1);
    REQUIRE_FALSE(engine.book().best_ask().has_value());  // consumed, never left crossed
}

TEST_CASE("Replay: risk position/PnL state updates from both sides of a trade", "[replay]") {
    InMemorySink sink;
    matching::MatchingEngine<kNumLevels, kMaxOrders> engine(SymbolId{1}, Price{1000});
    risk::RiskEngine<kMaxAccounts> risk(generous_limits());
    MarketDataPublisher<InMemorySink> feed(sink);
    Replay replay(engine, risk, feed);

    replay.process_new_order(new_order(1, Side::Sell, 1050, 100, /*client=*/1), Nanos{0});
    replay.process_new_order(new_order(2, Side::Buy, 1050, 100, /*client=*/2), Nanos{1});

    REQUIRE(risk.position(ClientId{1}, SymbolId{1}).value() == -100);  // seller
    REQUIRE(risk.position(ClientId{2}, SymbolId{1}).value() == 100);   // buyer
}

// --- the core deliverable: deterministic, byte-identical replay -----------

TEST_CASE("Replay: the identical input sequence produces byte-identical published output",
          "[replay][determinism]") {
    struct Event {
        NewOrderMessage msg;
        Nanos ts;
    };
    std::vector<Event> events;
    for (std::uint64_t i = 0; i < 200; ++i) {
        const Side side = (i % 3 == 0) ? Side::Sell : Side::Buy;
        const std::int64_t price = 1000 + static_cast<std::int64_t>((i * 37) % 80);
        const std::int64_t qty = 1 + static_cast<std::int64_t>((i * 13) % 50);
        events.push_back(
            {new_order(i + 1, side, price, qty, static_cast<std::uint32_t>(1 + i % 5)),
             Nanos{static_cast<std::int64_t>(i)}});
    }
    // A cancel and a replace too, not just new orders — determinism must
    // hold across the whole request vocabulary, not just the common case.
    const auto cancel_target = events[10].msg.order_id;
    const auto replace_target = events[20].msg.order_id;

    auto run_once = [&]() -> std::vector<std::byte> {
        InMemorySink sink;
        matching::MatchingEngine<kNumLevels, kMaxOrders> engine(SymbolId{1}, Price{1000});
        risk::RiskEngine<kMaxAccounts> risk(generous_limits());
        MarketDataPublisher<InMemorySink> feed(sink);
        Replay replay(engine, risk, feed);

        for (std::size_t i = 0; i < events.size(); ++i) {
            replay.process_new_order(events[i].msg, events[i].ts);
            if (i == 10) {
                replay.process_cancel(CancelOrderMessage{
                    .order_id = cancel_target, .client_id = 1, .symbol_id = 1});
            }
            if (i == 20) {
                replay.process_replace(
                    ReplaceOrderMessage{.order_id = replace_target, .new_price = 1015,
                                        .new_quantity = 5, .client_id = 1, .symbol_id = 1},
                    events[i].ts);
            }
        }
        return std::vector<std::byte>(sink.data().begin(), sink.data().end());
    };

    const auto run1 = run_once();
    const auto run2 = run_once();

    REQUIRE_FALSE(run1.empty());
    REQUIRE(run1.size() == run2.size());
    REQUIRE(run1 == run2);
}
