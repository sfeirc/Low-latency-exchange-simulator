#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "jane/book/price_level.hpp"
#include "jane/core/types.hpp"
#include "jane/matching/matching_engine.hpp"
#include "jane/protocol/codec.hpp"
#include "jane/protocol/messages.hpp"

// Turns matching-engine output (fills, and the driver's own notion of
// "these price levels changed") into the binary wire feed defined in
// jane/protocol: trade prints and incremental book deltas hot-path, full
// depth snapshots periodically or on subscriber connect. This class does
// not decide *when* a level changed — see MatchingEngine::submit's
// returned Fill list and `rested` flag — that bookkeeping belongs to the
// driver loop (jane::replay), which has the full before/after context;
// this class only knows how to encode and publish once told what to send.
namespace jane::marketdata {

template <typename Sink>
class MarketDataPublisher {
public:
    explicit MarketDataPublisher(Sink& sink, std::size_t snapshot_scratch_bytes = 8192)
        : sink_(sink), snapshot_scratch_(snapshot_scratch_bytes) {}

    void publish_trade(SymbolId symbol, const matching::Fill& fill) {
        const protocol::TradeMessage msg{
            .sequence = next_sequence_++,
            .match_id = fill.match_id,
            .price = fill.price.value(),
            .quantity = fill.quantity.value(),
            .symbol_id = symbol.value(),
            .aggressor_side = fill.aggressor_side,
        };
        std::array<std::byte, 64> buf{};
        const std::size_t n = protocol::encode(std::span(buf), msg);
        sink_.write(std::span(buf).first(n));
    }

    // `level` must be the level's *current* (post-update) state; action is
    // derived from whether it's now empty, not passed by the caller.
    void publish_level_update(SymbolId symbol, Side side, Price price,
                               const book::PriceLevel& level) {
        const protocol::BookDeltaMessage msg{
            .sequence = next_sequence_++,
            .price = price.value(),
            .aggregate_quantity = level.total_quantity.value(),
            .symbol_id = symbol.value(),
            .order_count = level.order_count,
            .side = side,
            .action = (level.order_count == 0) ? protocol::DeltaAction::Delete
                                                : protocol::DeltaAction::Update,
        };
        std::array<std::byte, 64> buf{};
        const std::size_t n = protocol::encode(std::span(buf), msg);
        sink_.write(std::span(buf).first(n));
    }

    // Walks the top `depth` levels of each side (best first) and
    // publishes a full BookSnapshot. Not a hot-path call (periodic, or
    // on a new subscriber connecting) — allocates freely into reusable
    // scratch members rather than trying to be allocation-free the way
    // the per-order path is.
    template <typename Book>
    void publish_snapshot(SymbolId symbol, const Book& book, std::size_t depth) {
        bid_scratch_.clear();
        ask_scratch_.clear();
        const auto collect = [depth](std::vector<protocol::PriceLevelRecord>& out) {
            return [&out, depth](Price price, const book::PriceLevel& lvl) {
                out.push_back(protocol::PriceLevelRecord{
                    .price = price.value(),
                    .aggregate_quantity = lvl.total_quantity.value(),
                    .order_count = lvl.order_count,
                });
                return out.size() < depth;
            };
        };
        book.for_each_level(Side::Buy, collect(bid_scratch_));
        book.for_each_level(Side::Sell, collect(ask_scratch_));

        std::size_t written = 0;
        for (int attempt = 0; attempt < 4; ++attempt) {
            written = protocol::encode_snapshot(std::span(snapshot_scratch_), next_sequence_,
                                                 symbol.value(), bid_scratch_, ask_scratch_);
            if (written > 0) {
                break;
            }
            snapshot_scratch_.resize(snapshot_scratch_.size() * 2);
        }
        if (written == 0) {
            return;  // depth exceeds the wire format's own level-count ceiling; nothing sane to send
        }
        ++next_sequence_;
        sink_.write(std::span(snapshot_scratch_).first(written));
    }

    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_sequence_; }

private:
    Sink& sink_;
    std::uint64_t next_sequence_ = 1;
    std::vector<std::byte> snapshot_scratch_;
    std::vector<protocol::PriceLevelRecord> bid_scratch_;
    std::vector<protocol::PriceLevelRecord> ask_scratch_;
};

}  // namespace jane::marketdata
