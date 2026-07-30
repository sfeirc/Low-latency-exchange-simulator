#pragma once

#include <cstddef>
#include <vector>

#include "jane/core/order.hpp"
#include "jane/core/types.hpp"
#include "jane/marketdata/publisher.hpp"
#include "jane/matching/matching_engine.hpp"
#include "jane/protocol/messages.hpp"
#include "jane/risk/risk_engine.hpp"

// The driver loop every other component in this codebase was built to be
// composed by: sequences an incoming request, runs the pre-trade risk
// gate, hands approved orders to matching, and publishes the resulting
// trades/book deltas/execution reports. Nothing here is novel logic — the
// value of this file is entirely in getting the composition order right
// (validate/risk *before* matching, publish market data *after* the book
// actually changed — including on a plain cancel, which still moves a
// level's aggregate — stamp sequence/timestamp exactly once per event)
// and making that order deterministic: two runs fed the identical
// sequence of (message, timestamp) pairs produce byte-identical published
// output, which is what makes this a bug-repro tool and not just a test
// harness. See tests/test_replay_engine.cpp for the byte-identical-replay
// proof.
//
// Execution reports (private, per-client) are published through the same
// MarketDataPublisher/Sink as public market data (trades, deltas) — a
// deliberate scope simplification for this project rather than building
// a separate private-channel abstraction; see docs/architecture.md.
namespace jane::replay {

using protocol::ExecType;

// A trivial monotonic counter, not a wall clock: bug reproduction needs
// "the same input produces the same output," not "the same input takes
// the same wall-clock time to produce it." A caller wanting more
// realistic *relative* timing (for benchmarking, not correctness) can
// ignore this and supply its own timestamps instead — see
// SyntheticOrderGenerator's Poisson inter-arrival times.
class DeterministicClock {
public:
    explicit DeterministicClock(Nanos start = Nanos{0}, Nanos step = Nanos{1}) noexcept
        : current_(start), step_(step) {}

    [[nodiscard]] Nanos now() const noexcept { return current_; }
    Nanos tick() noexcept {
        const Nanos t = current_;
        current_ += step_;
        return t;
    }
    void advance(Nanos delta) noexcept { current_ += delta; }

private:
    Nanos current_;
    Nanos step_;
};

template <std::size_t NumLevels, std::size_t MaxOrders, std::size_t MaxAccounts, typename Sink>
class ReplayEngine {
public:
    using MatchingEngineT = matching::MatchingEngine<NumLevels, MaxOrders>;
    using RiskEngineT = risk::RiskEngine<MaxAccounts>;
    using PublisherT = marketdata::MarketDataPublisher<Sink>;

    ReplayEngine(MatchingEngineT& matching, RiskEngineT& risk, PublisherT& feed) noexcept
        : matching_(matching), risk_(risk), feed_(feed) {}

    // `timestamp` is supplied by the caller (log replay: an incrementing
    // DeterministicClock; synthetic generation: Poisson inter-arrivals) —
    // this function never reads a clock itself, which is exactly what
    // makes it reproducible.
    void process_new_order(const protocol::NewOrderMessage& wire, Nanos timestamp) {
        Order order{};
        order.id = OrderId{wire.order_id};
        order.client = ClientId{wire.client_id};
        order.symbol = SymbolId{wire.symbol_id};
        order.side = wire.side;
        order.type = wire.order_type;
        order.tif = wire.time_in_force;
        order.price = Price{wire.price};
        order.quantity = Quantity{wire.quantity};
        order.remaining = order.quantity;
        order.sequence = next_sequence_;
        order.timestamp = timestamp;
        next_sequence_ = Sequence{next_sequence_.value() + 1};

        const RejectReason risk_reason = risk_.check_new_order(order);
        if (risk_reason != RejectReason::None) {
            publish_report(order.id, order.client, order.symbol, {}, order.quantity,
                            ExecType::Rejected, risk_reason);
            return;
        }

        fill_scratch_.clear();
        const matching::NewOrderResult result = matching_.submit(order, fill_scratch_);
        apply_fills(order.symbol, fill_scratch_);
        if (result.rested) {
            publish_level(order.symbol, order.side, order.price);
        }

        ExecType exec_type;
        if (result.reject_reason != RejectReason::None) {
            exec_type = ExecType::Rejected;
        } else if (result.remaining_after.value() == 0) {
            exec_type = ExecType::Fill;
        } else if (!fill_scratch_.empty()) {
            exec_type = ExecType::PartialFill;
        } else if (result.rested) {
            exec_type = ExecType::New;
        } else {
            exec_type = ExecType::Cancelled;  // IOC/FOK/Market remainder dropped without a fill
        }
        publish_report(order.id, order.client, order.symbol, fill_scratch_, result.remaining_after,
                        exec_type, result.reject_reason);
    }

    void process_cancel(const protocol::CancelOrderMessage& wire) {
        const auto* existing = matching_.book().find(OrderId{wire.order_id});
        if (existing == nullptr) {
            publish_report(OrderId{wire.order_id}, ClientId{wire.client_id}, SymbolId{wire.symbol_id},
                            {}, Quantity{0}, ExecType::Rejected, RejectReason::UnknownOrder);
            return;
        }
        const Side side = existing->order.side;
        const Price price = existing->order.price;
        const SymbolId symbol = existing->order.symbol;
        const ClientId client = existing->order.client;

        if (matching_.cancel(OrderId{wire.order_id}).accepted) {
            publish_level(symbol, side, price);
            publish_report(OrderId{wire.order_id}, client, symbol, {}, Quantity{0},
                            ExecType::Cancelled, RejectReason::None);
        }
    }

    void process_replace(const protocol::ReplaceOrderMessage& wire, Nanos timestamp) {
        const auto* existing = matching_.book().find(OrderId{wire.order_id});
        if (existing == nullptr) {
            publish_report(OrderId{wire.order_id}, ClientId{wire.client_id}, SymbolId{wire.symbol_id},
                            {}, Quantity{0}, ExecType::Rejected, RejectReason::UnknownOrder);
            return;
        }
        const Side side = existing->order.side;  // a replace never changes side
        const SymbolId symbol = existing->order.symbol;
        const ClientId client = existing->order.client;
        const Price old_price = existing->order.price;

        const Sequence seq = next_sequence_;
        next_sequence_ = Sequence{next_sequence_.value() + 1};

        fill_scratch_.clear();
        const auto result = matching_.replace(OrderId{wire.order_id}, Price{wire.new_price},
                                               Quantity{wire.new_quantity}, seq, timestamp,
                                               fill_scratch_);
        if (!result.accepted) {
            publish_report(OrderId{wire.order_id}, client, symbol, {}, Quantity{0},
                            ExecType::Rejected, result.reject_reason);
            return;
        }

        apply_fills(symbol, fill_scratch_);
        publish_level(symbol, side, old_price);  // the vacated level may now be empty
        if (result.rested) {
            publish_level(symbol, side, Price{wire.new_price});
        }

        const ExecType exec_type = (result.remaining_after.value() == 0) ? ExecType::Fill
                                    : !fill_scratch_.empty()              ? ExecType::PartialFill
                                                                          : ExecType::Replaced;
        publish_report(OrderId{wire.order_id}, client, symbol, fill_scratch_, result.remaining_after,
                        exec_type, RejectReason::None);
    }

    [[nodiscard]] Sequence next_sequence() const noexcept { return next_sequence_; }

private:
    void apply_fills(SymbolId symbol, const std::vector<matching::Fill>& fills) {
        for (const auto& f : fills) {
            feed_.publish_trade(symbol, f);
            const Side resting_side = opposite(f.aggressor_side);
            risk_.record_fill(f.resting_client_id, symbol, resting_side, f.price, f.quantity);
            risk_.record_fill(f.aggressor_client_id, symbol, f.aggressor_side, f.price, f.quantity);
            publish_level(symbol, resting_side, f.price);
        }
    }

    void publish_level(SymbolId symbol, Side side, Price price) {
        if (const auto* level = matching_.book().level_at(side, price); level != nullptr) {
            feed_.publish_level_update(symbol, side, price, *level);
        }
    }

    // One execution report per request summarizing its net effect, not
    // one per fill — see docs/architecture.md for why that's the right
    // level of detail for this project's scope. last_quantity is the
    // *sum* filled by this call; price is the last fill's price when
    // there were several (an approximation when a sweep crosses multiple
    // price levels, documented rather than silently imprecise).
    void publish_report(OrderId id, ClientId client, SymbolId symbol,
                         const std::vector<matching::Fill>& fills, Quantity leaves_quantity,
                         ExecType exec_type, RejectReason reject_reason) {
        Quantity total_filled{0};
        Price last_price{0};
        std::uint64_t last_match_id = 0;
        for (const auto& f : fills) {
            total_filled += f.quantity;
            last_price = f.price;
            last_match_id = f.match_id;
        }
        feed_.publish_execution_report(protocol::ExecutionReportMessage{
            .order_id = id.value(),
            .match_id = last_match_id,
            .price = last_price.value(),
            .last_quantity = total_filled.value(),
            .leaves_quantity = leaves_quantity.value(),
            .client_id = client.value(),
            .symbol_id = symbol.value(),
            .exec_type = exec_type,
            .reject_reason = reject_reason,
        });
    }

    MatchingEngineT& matching_;
    RiskEngineT& risk_;
    PublisherT& feed_;
    Sequence next_sequence_{1};
    std::vector<matching::Fill> fill_scratch_;
};

}  // namespace jane::replay
