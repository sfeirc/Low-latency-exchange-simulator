#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <vector>

#include "jane/book/order_node.hpp"
#include "jane/book/price_level.hpp"

using namespace jane;
using namespace jane::book;

namespace {
OrderNode make_node(std::uint64_t id, std::int64_t remaining) {
    OrderNode n{};
    n.order.id = OrderId{id};
    n.order.remaining = Quantity{remaining};
    return n;
}

// Walks head->tail, returns order ids in FIFO order — the property that
// actually matters for matching (see docs/correctness.md).
std::vector<std::uint64_t> walk_forward(const PriceLevel& level) {
    std::vector<std::uint64_t> ids;
    for (OrderNode* n = level.head; n != nullptr; n = n->next) {
        ids.push_back(n->order.id.value());
    }
    return ids;
}
std::vector<std::uint64_t> walk_backward(const PriceLevel& level) {
    std::vector<std::uint64_t> ids;
    for (OrderNode* n = level.tail; n != nullptr; n = n->prev) {
        ids.push_back(n->order.id.value());
    }
    return ids;
}
}  // namespace

TEST_CASE("PriceLevel: push_back on an empty level", "[price_level]") {
    PriceLevel level;
    OrderNode a = make_node(1, 100);
    level_push_back(level, &a);

    REQUIRE(level.head == &a);
    REQUIRE(level.tail == &a);
    REQUIRE(level.order_count == 1);
    REQUIRE(level.total_quantity.value() == 100);
    REQUIRE(a.prev == nullptr);
    REQUIRE(a.next == nullptr);
}

TEST_CASE("PriceLevel: push_back preserves FIFO order and aggregates", "[price_level]") {
    PriceLevel level;
    OrderNode a = make_node(1, 100);
    OrderNode b = make_node(2, 50);
    OrderNode c = make_node(3, 25);
    level_push_back(level, &a);
    level_push_back(level, &b);
    level_push_back(level, &c);

    REQUIRE(level.head == &a);
    REQUIRE(level.tail == &c);
    REQUIRE(level.order_count == 3);
    REQUIRE(level.total_quantity.value() == 175);
    REQUIRE(walk_forward(level) == std::vector<std::uint64_t>{1, 2, 3});
    REQUIRE(walk_backward(level) == std::vector<std::uint64_t>{3, 2, 1});
}

TEST_CASE("PriceLevel: unlink the only node empties the level", "[price_level]") {
    PriceLevel level;
    OrderNode a = make_node(1, 100);
    level_push_back(level, &a);
    level_unlink(level, &a);

    REQUIRE(level.head == nullptr);
    REQUIRE(level.tail == nullptr);
    REQUIRE(level.order_count == 0);
    REQUIRE(level.total_quantity.value() == 0);
    REQUIRE(level.empty());
    REQUIRE(a.prev == nullptr);
    REQUIRE(a.next == nullptr);
}

TEST_CASE("PriceLevel: unlink head advances head, preserves the rest", "[price_level]") {
    PriceLevel level;
    OrderNode a = make_node(1, 10), b = make_node(2, 20), c = make_node(3, 30);
    level_push_back(level, &a);
    level_push_back(level, &b);
    level_push_back(level, &c);

    level_unlink(level, &a);

    REQUIRE(level.head == &b);
    REQUIRE(level.tail == &c);
    REQUIRE(b.prev == nullptr);
    REQUIRE(level.order_count == 2);
    REQUIRE(level.total_quantity.value() == 50);
    REQUIRE(walk_forward(level) == std::vector<std::uint64_t>{2, 3});
}

TEST_CASE("PriceLevel: unlink tail retreats tail, preserves the rest", "[price_level]") {
    PriceLevel level;
    OrderNode a = make_node(1, 10), b = make_node(2, 20), c = make_node(3, 30);
    level_push_back(level, &a);
    level_push_back(level, &b);
    level_push_back(level, &c);

    level_unlink(level, &c);

    REQUIRE(level.head == &a);
    REQUIRE(level.tail == &b);
    REQUIRE(b.next == nullptr);
    REQUIRE(level.order_count == 2);
    REQUIRE(walk_forward(level) == std::vector<std::uint64_t>{1, 2});
}

TEST_CASE("PriceLevel: unlink a middle node relinks its neighbors", "[price_level]") {
    PriceLevel level;
    OrderNode a = make_node(1, 10), b = make_node(2, 20), c = make_node(3, 30), d = make_node(4, 40);
    level_push_back(level, &a);
    level_push_back(level, &b);
    level_push_back(level, &c);
    level_push_back(level, &d);

    level_unlink(level, &b);  // remove a middle node

    REQUIRE(level.head == &a);
    REQUIRE(level.tail == &d);
    REQUIRE(a.next == &c);
    REQUIRE(c.prev == &a);
    REQUIRE(level.order_count == 3);
    REQUIRE(level.total_quantity.value() == 80);
    REQUIRE(walk_forward(level) == std::vector<std::uint64_t>{1, 3, 4});
    REQUIRE(walk_backward(level) == std::vector<std::uint64_t>{4, 3, 1});

    level_unlink(level, &c);  // remove another middle node (now a<->d neighbors)
    REQUIRE(a.next == &d);
    REQUIRE(d.prev == &a);
    REQUIRE(walk_forward(level) == std::vector<std::uint64_t>{1, 4});
}

TEST_CASE("PriceLevel: interleaved push/unlink sequence matches a reference model",
          "[price_level]") {
    // Push 1..6, remove 2 and 5 (middle), push 7, remove 1 (head), remove 6 (tail).
    // Expected remaining, in FIFO order: 3, 4, 7.
    // std::deque, not vector: push_back must never invalidate the pointers
    // already linked into `level` (vector reallocation would).
    PriceLevel level;
    std::deque<OrderNode> nodes;
    for (std::uint64_t id = 1; id <= 6; ++id) {
        nodes.push_back(make_node(id, static_cast<std::int64_t>(id) * 10));
        level_push_back(level, &nodes.back());
    }
    level_unlink(level, &nodes[1]);  // id=2
    level_unlink(level, &nodes[4]);  // id=5

    nodes.push_back(make_node(7, 70));
    level_push_back(level, &nodes.back());  // id=7

    level_unlink(level, &nodes[0]);  // id=1 (head)
    level_unlink(level, &nodes[5]);  // id=6 (tail)

    REQUIRE(walk_forward(level) == std::vector<std::uint64_t>{3, 4, 7});
    REQUIRE(level.order_count == 3);
    REQUIRE(level.total_quantity.value() == 30 + 40 + 70);
}
