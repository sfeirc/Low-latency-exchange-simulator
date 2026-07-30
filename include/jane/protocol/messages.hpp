#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "jane/core/types.hpp"

// The wire format every component downstream of order entry actually
// speaks: client -> exchange (NewOrder/Cancel/Replace), exchange -> client
// (ExecutionReport), and exchange -> market data (Trade, BookDelta,
// BookSnapshot*). Modeled loosely on NASDAQ ITCH/OUCH: small fixed-size
// binary messages, no text, no schema negotiation.
//
// Field order within every struct is deliberately largest-alignment-first
// (all 8-byte fields, then 4-byte, then 1-byte) so natural alignment alone
// produces a compact layout with no compiler-inserted padding — verified
// by the static_assert(sizeof(...) == N) after each struct, not assumed.
// This project never uses #pragma pack: packing can force misaligned
// member access, and it's unnecessary when the layout is simply chosen to
// not need padding in the first place.
namespace jane::protocol {

enum class MessageType : std::uint8_t {
    NewOrder = 1,
    CancelOrder = 2,
    ReplaceOrder = 3,
    ExecutionReport = 4,
    Trade = 5,
    BookDelta = 6,
    BookSnapshot = 7,
};

// Precedes every message on the wire. Deliberately compact (4 bytes) —
// see jane/protocol/codec.hpp for why that means decode is a bounds-
// checked memcpy rather than an aligned reinterpret_cast view: a 4-byte
// header does not leave 8-byte-aligned payloads for a buffer that only
// itself starts 8-aligned, and this project would rather have one obviously
// -correct decode path than a compact header *and* a viable zero-copy view.
struct MessageHeader {
    std::uint16_t length;  // payload length in bytes, NOT including this header
    MessageType type;
    std::uint8_t reserved{0};
};
static_assert(sizeof(MessageHeader) == 4);
static_assert(std::is_trivially_copyable_v<MessageHeader>);

// --- order entry: client -> exchange ------------------------------------

// client_id/symbol_id are uint32_t here (not uint64_t) specifically to
// match jane::ClientId/SymbolId's underlying type (see core/types.hpp) —
// an earlier version of this file used uint64_t for both, which meant
// every wire-to-domain conversion was a silent narrowing conversion the
// compiler warned about the first time replay_engine.hpp actually
// performed one. Fixed at the source (the wire format) rather than by
// casting at every conversion site.
struct NewOrderMessage {
    std::uint64_t order_id;
    std::int64_t price;  // ticks; ignored by the matching engine if order_type == Market
    std::int64_t quantity;
    std::uint32_t client_id;
    std::uint32_t symbol_id;
    Side side;
    OrderType order_type;
    TimeInForce time_in_force;
    std::uint8_t reserved[5]{};
};
static_assert(sizeof(NewOrderMessage) == 40);
static_assert(std::is_trivially_copyable_v<NewOrderMessage>);

struct CancelOrderMessage {
    std::uint64_t order_id;
    std::uint32_t client_id;
    std::uint32_t symbol_id;
};
static_assert(sizeof(CancelOrderMessage) == 16);
static_assert(std::is_trivially_copyable_v<CancelOrderMessage>);

struct ReplaceOrderMessage {
    std::uint64_t order_id;  // existing resting order being replaced
    std::int64_t new_price;
    std::int64_t new_quantity;
    std::uint32_t client_id;
    std::uint32_t symbol_id;
};
static_assert(sizeof(ReplaceOrderMessage) == 32);
static_assert(std::is_trivially_copyable_v<ReplaceOrderMessage>);

// --- order entry: exchange -> client -------------------------------------

enum class ExecType : std::uint8_t {
    New = 0,
    PartialFill = 1,
    Fill = 2,
    Cancelled = 3,
    Replaced = 4,
    Rejected = 5,
};

struct ExecutionReportMessage {
    std::uint64_t order_id;
    std::uint64_t match_id;         // 0 unless this event is a fill
    std::int64_t price;             // fill price; 0 unless this event is a fill
    std::int64_t last_quantity;     // quantity filled by this event; 0 unless a fill
    std::int64_t leaves_quantity;   // remaining open quantity after this event
    std::uint32_t client_id;
    std::uint32_t symbol_id;
    ExecType exec_type;
    RejectReason reject_reason;     // meaningful only when exec_type == Rejected
    std::uint8_t reserved[6]{};
};
static_assert(sizeof(ExecutionReportMessage) == 56);
static_assert(std::is_trivially_copyable_v<ExecutionReportMessage>);

// --- market data: exchange -> everyone ------------------------------------

struct TradeMessage {
    std::uint64_t sequence;  // feed sequence number — gap detection for subscribers
    std::uint64_t match_id;
    std::int64_t price;
    std::int64_t quantity;
    std::uint32_t symbol_id;
    Side aggressor_side;  // side of the order that crossed the spread
    std::uint8_t reserved[3]{};
};
static_assert(sizeof(TradeMessage) == 40);
static_assert(std::is_trivially_copyable_v<TradeMessage>);

// Add/Update collapse to one action deliberately: a depth feed at the
// price-level granularity (not per-order) has no meaningful difference
// between "this price is now occupied for the first time" and "this
// price's aggregate changed" — a subscriber applies both the same way
// (upsert the level to this aggregate/count). Real MBP-style feeds work
// the same way; only Delete needs to be distinct (remove the level).
enum class DeltaAction : std::uint8_t { Update = 0, Delete = 1 };

struct BookDeltaMessage {
    std::uint64_t sequence;  // feed sequence number — gap detection for subscribers
    std::int64_t price;
    std::int64_t aggregate_quantity;  // new total resting quantity at this level; 0 if Delete
    std::uint32_t symbol_id;
    std::uint32_t order_count;
    Side side;
    DeltaAction action;
    std::uint8_t reserved[6]{};
};
static_assert(sizeof(BookDeltaMessage) == 40);
static_assert(std::is_trivially_copyable_v<BookDeltaMessage>);

// A full snapshot is inherently variable-length (top-of-book through
// depth-N), so unlike every message above it isn't a single fixed struct:
// it's this header followed by `bid_level_count + ask_level_count`
// PriceLevelRecords (bids first, then asks). See jane/protocol/codec.hpp
// for encode_snapshot/decode_snapshot, which handle the framing.
struct BookSnapshotHeader {
    std::uint64_t sequence;
    std::uint32_t symbol_id;
    std::uint16_t bid_level_count;
    std::uint16_t ask_level_count;
};
static_assert(sizeof(BookSnapshotHeader) == 16);
static_assert(std::is_trivially_copyable_v<BookSnapshotHeader>);

struct PriceLevelRecord {
    std::int64_t price;
    std::int64_t aggregate_quantity;
    std::uint32_t order_count;
    std::uint8_t reserved[4]{};
};
static_assert(sizeof(PriceLevelRecord) == 24);
static_assert(std::is_trivially_copyable_v<PriceLevelRecord>);

// Compile-time Payload type -> wire MessageType tag, so codec.hpp's
// encode<T>/decode<T> can be single generic functions instead of one
// hand-written pair per message type.
template <typename Payload>
struct MessageTraits;

#define JANE_DEFINE_MESSAGE_TRAITS(PayloadType, WireType)     \
    template <>                                               \
    struct MessageTraits<PayloadType> {                       \
        static constexpr MessageType type = WireType;         \
    }

JANE_DEFINE_MESSAGE_TRAITS(NewOrderMessage, MessageType::NewOrder);
JANE_DEFINE_MESSAGE_TRAITS(CancelOrderMessage, MessageType::CancelOrder);
JANE_DEFINE_MESSAGE_TRAITS(ReplaceOrderMessage, MessageType::ReplaceOrder);
JANE_DEFINE_MESSAGE_TRAITS(ExecutionReportMessage, MessageType::ExecutionReport);
JANE_DEFINE_MESSAGE_TRAITS(TradeMessage, MessageType::Trade);
JANE_DEFINE_MESSAGE_TRAITS(BookDeltaMessage, MessageType::BookDelta);

#undef JANE_DEFINE_MESSAGE_TRAITS

}  // namespace jane::protocol
