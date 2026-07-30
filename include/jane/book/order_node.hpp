#pragma once

#include <type_traits>

#include "jane/core/order.hpp"

namespace jane::book {

// The book's internal, pooled representation of a resting order: the
// domain Order plus intrusive doubly-linked-list pointers threading it
// into its price level's FIFO queue. Kept separate from jane::Order so
// that type stays free of book-internal concerns (see core/order.hpp) —
// the protocol layer, risk engine, and strategies never see an OrderNode.
//
// Trivially destructible/copyable so it can live in a jane::SlabPool.
struct OrderNode {
    Order order;
    OrderNode* prev = nullptr;
    OrderNode* next = nullptr;
    std::size_t level_index = 0;  // which ladder slot this node is threaded into
};

static_assert(std::is_trivially_destructible_v<OrderNode>);
static_assert(std::is_trivially_copyable_v<OrderNode>);

}  // namespace jane::book
