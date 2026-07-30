#pragma once

#include <type_traits>

#include "jane/core/types.hpp"

namespace jane {

// The domain representation of an order — what the protocol layer decodes
// into, what the risk engine inspects, what strategies build. Deliberately
// has no knowledge of how the book stores it internally (no intrusive list
// pointers here; see jane::book::OrderNode for that).
struct Order {
    OrderId id{};
    ClientId client{};
    SymbolId symbol{};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    TimeInForce tif{TimeInForce::Day};
    Price price{};        // meaningful when type == Limit; ignored for Market
    Quantity quantity{};  // original requested quantity
    Quantity remaining{};  // quantity still open (== quantity until first fill)
    Sequence sequence{};  // book-assigned; the tiebreak that defines time priority
    Nanos timestamp{};    // book-assigned acceptance time

    [[nodiscard]] constexpr bool is_filled() const noexcept { return remaining.value() == 0; }

    [[nodiscard]] constexpr Quantity filled_quantity() const noexcept {
        return quantity - remaining;
    }
};

static_assert(std::is_trivially_copyable_v<Order>,
              "Order must stay trivially copyable: it is stored by value in "
              "pools/ring buffers and moved with memcpy on the hot path");

}  // namespace jane
