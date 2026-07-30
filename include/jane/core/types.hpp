#pragma once

#include <cstdint>

#include "jane/core/strong_type.hpp"

namespace jane {

namespace tag {
struct PriceTag {};
struct QuantityTag {};
struct OrderIdTag {};
struct SequenceTag {};
struct SymbolIdTag {};
struct ClientIdTag {};
struct NanosTag {};
struct PnLTag {};
}  // namespace tag

// Price is an integer number of ticks, never a float: floating point
// equality/ordering on prices is a well-known source of exchange bugs
// (two "equal" prices that compare unequal after arithmetic). The mapping
// from ticks to a human price (e.g. ticks * 0.0001 USD) is a symbol-level
// concern, not something the matching core needs to know about.
using Price = StrongAmount<std::int64_t, tag::PriceTag>;

// Quantity is signed (not size_t/uint64_t) so that `remaining = total -
// filled` and similar deltas never wrap around to a huge positive number
// if a bug produces a transient negative value — it fails loudly instead
// of corrupting book state. JANE_ASSERT(qty.value() >= 0) is used at the
// boundaries where a negative quantity would be a logic error.
using Quantity = StrongAmount<std::int64_t, tag::QuantityTag>;

using OrderId = StrongId<std::uint64_t, tag::OrderIdTag>;
using Sequence = StrongId<std::uint64_t, tag::SequenceTag>;
using SymbolId = StrongId<std::uint32_t, tag::SymbolIdTag>;
using ClientId = StrongId<std::uint32_t, tag::ClientIdTag>;

// Nanoseconds, monotonic clock, never wall-clock: used for both real
// timestamps and deterministic replay (see jane/replay).
using Nanos = StrongAmount<std::int64_t, tag::NanosTag>;

// Signed money-like quantity — price * quantity, e.g. cash flow or P&L.
// Deliberately its own type rather than reusing Price or Quantity: it has
// different units than either (see jane::risk::RiskEngine) and mixing it
// with a bare Price or Quantity by accident is exactly the class of bug
// the strong-type scheme throughout core/ exists to catch at compile time.
using PnL = StrongAmount<std::int64_t, tag::PnLTag>;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

enum class OrderType : std::uint8_t {
    Limit = 0,
    Market = 1,
};

enum class TimeInForce : std::uint8_t {
    Day = 0,  // rests on the book until cancelled or session end
    IOC = 1,  // immediate-or-cancel: fill what's available now, cancel the rest
    FOK = 2,  // fill-or-kill: fill the entire quantity now or cancel all of it
};

enum class OrderStatus : std::uint8_t {
    New = 0,
    PartiallyFilled = 1,
    Filled = 2,
    Cancelled = 3,
    Rejected = 4,
    Replaced = 5,
};

// Shared vocabulary for why an order/cancel/replace did not proceed.
// Carried on both the internal execution report and the wire ExecReport
// message (jane/protocol) so a rejected client order always tells the
// caller exactly which control fired.
enum class RejectReason : std::uint8_t {
    None = 0,
    InvalidPrice,
    InvalidQuantity,
    UnknownSymbol,
    UnknownOrder,        // cancel/replace referencing a nonexistent/filled order
    DuplicateOrderId,
    FokNotFillable,      // fill-or-kill couldn't be fully satisfied immediately; nothing was filled
    BookCapacityExceeded,  // resting the order would exceed the book's fixed order capacity
    RiskMaxOrderSize,
    RiskMaxPosition,
    RiskMaxLoss,
    RiskKillSwitch,
};

}  // namespace jane
