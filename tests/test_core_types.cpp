#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <unordered_map>

#include "jane/core/clock.hpp"
#include "jane/core/order.hpp"
#include "jane/core/types.hpp"

using namespace jane;

// --- compile-time type-safety: the whole point of the strong types -------

static_assert(!std::convertible_to<Price, Quantity>,
              "Price and Quantity must not be implicitly interchangeable");
static_assert(!std::convertible_to<OrderId, Sequence>,
              "OrderId and Sequence must not be implicitly interchangeable");
static_assert(!std::is_same_v<Price, Quantity>);
static_assert(std::is_trivially_copyable_v<Price>);
static_assert(std::is_trivially_copyable_v<OrderId>);

TEST_CASE("StrongAmount arithmetic", "[types]") {
    constexpr Price a{100};
    constexpr Price b{25};

    STATIC_REQUIRE((a + b).value() == 125);
    STATIC_REQUIRE((a - b).value() == 75);
    STATIC_REQUIRE((-a).value() == -100);
    STATIC_REQUIRE((a * 3).value() == 300);
    STATIC_REQUIRE(a > b);
    STATIC_REQUIRE(b < a);
    STATIC_REQUIRE(a == Price{100});

    Price c{10};
    c += Price{5};
    REQUIRE(c.value() == 15);
    c -= Price{3};
    REQUIRE(c.value() == 12);
}

TEST_CASE("StrongId comparison, ordering, and hashing", "[types]") {
    constexpr OrderId id1{42};
    constexpr OrderId id2{42};
    constexpr OrderId id3{7};

    STATIC_REQUIRE(id1 == id2);
    STATIC_REQUIRE(id3 < id1);

    std::unordered_map<OrderId, int> m;
    m[id1] = 1;
    m[id3] = 2;
    REQUIRE(m.at(OrderId{42}) == 1);
    REQUIRE(m.at(OrderId{7}) == 2);
}

TEST_CASE("Side::opposite flips exactly two ways", "[types]") {
    STATIC_REQUIRE(opposite(Side::Buy) == Side::Sell);
    STATIC_REQUIRE(opposite(Side::Sell) == Side::Buy);
    STATIC_REQUIRE(opposite(opposite(Side::Buy)) == Side::Buy);
}

TEST_CASE("Order fill accounting", "[types][order]") {
    Order o{};
    o.quantity = Quantity{100};
    o.remaining = Quantity{100};
    REQUIRE_FALSE(o.is_filled());
    REQUIRE(o.filled_quantity().value() == 0);

    o.remaining = Quantity{40};
    REQUIRE_FALSE(o.is_filled());
    REQUIRE(o.filled_quantity().value() == 60);

    o.remaining = Quantity{0};
    REQUIRE(o.is_filled());
    REQUIRE(o.filled_quantity().value() == 100);
}

TEST_CASE("SystemClock is monotonic and reports nanoseconds", "[types][clock]") {
    static_assert(ClockLike<SystemClock>);
    const Nanos t1 = SystemClock::now();
    const Nanos t2 = SystemClock::now();
    REQUIRE(t2 >= t1);
}
