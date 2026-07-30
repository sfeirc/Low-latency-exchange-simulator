#pragma once

#include <cstdint>

#include "jane/book/order_node.hpp"
#include "jane/core/types.hpp"

namespace jane::book {

// The FIFO of resting orders at one exact price: an intrusive doubly-
// linked list (head = oldest = next to match) plus the running aggregates
// a matching engine or market-data publisher needs without walking the
// list (total resting quantity, order count).
struct PriceLevel {
    OrderNode* head = nullptr;
    OrderNode* tail = nullptr;
    Quantity total_quantity{0};
    std::uint32_t order_count = 0;

    [[nodiscard]] bool empty() const noexcept { return order_count == 0; }
};

// Appends `node` to the back of `level`'s FIFO (new resting orders always
// go to the back — that's what "time priority" means) and updates the
// level's aggregates. `node` must not already be linked anywhere.
inline void level_push_back(PriceLevel& level, OrderNode* node) noexcept {
    node->prev = level.tail;
    node->next = nullptr;
    if (level.tail != nullptr) {
        level.tail->next = node;
    } else {
        level.head = node;
    }
    level.tail = node;
    level.total_quantity += node->order.remaining;
    ++level.order_count;
}

// Removes `node` from wherever it sits in `level`'s FIFO (head, tail, or
// middle — a cancel can target any resting order, not just the front) and
// updates the level's aggregates. `node` must currently be linked into
// this exact level.
inline void level_unlink(PriceLevel& level, OrderNode* node) noexcept {
    if (node->prev != nullptr) {
        node->prev->next = node->next;
    } else {
        level.head = node->next;
    }
    if (node->next != nullptr) {
        node->next->prev = node->prev;
    } else {
        level.tail = node->prev;
    }
    node->prev = nullptr;
    node->next = nullptr;
    level.total_quantity -= node->order.remaining;
    --level.order_count;
}

}  // namespace jane::book
