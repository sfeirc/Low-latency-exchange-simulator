#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory_resource>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include "jane/memory/pmr_pool.hpp"
#include "jane/memory/slab_pool.hpp"

namespace {
struct Widget {
    int value = -1;
    char pad[16]{};
};
static_assert(std::is_trivially_destructible_v<Widget>);
}  // namespace

// --- SlabPool -----------------------------------------------------------

TEST_CASE("SlabPool: allocate returns default-constructed, distinct objects", "[memory][slab]") {
    jane::SlabPool<Widget, 4> pool;
    Widget* a = pool.allocate();
    Widget* b = pool.allocate();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a != b);
    REQUIRE(a->value == -1);
    REQUIRE(b->value == -1);
}

TEST_CASE("SlabPool: exhausts at capacity and returns nullptr, not UB", "[memory][slab]") {
    jane::SlabPool<Widget, 3> pool;
    REQUIRE(pool.allocate() != nullptr);
    REQUIRE(pool.allocate() != nullptr);
    REQUIRE(pool.allocate() != nullptr);
    REQUIRE(pool.allocate() == nullptr);  // exhausted
    REQUIRE(pool.live_count() == 3);
}

TEST_CASE("SlabPool: deallocate frees a slot for reuse", "[memory][slab]") {
    jane::SlabPool<Widget, 2> pool;
    Widget* a = pool.allocate();
    Widget* b = pool.allocate();
    REQUIRE(pool.allocate() == nullptr);

    pool.deallocate(a);
    REQUIRE(pool.live_count() == 1);
    Widget* c = pool.allocate();
    REQUIRE(c != nullptr);
    REQUIRE(c->value == -1);  // reused slot re-default-constructed, not stale garbage
    (void)b;
}

TEST_CASE("SlabPool: writes to one object never alias another", "[memory][slab]") {
    constexpr std::size_t kN = 256;
    jane::SlabPool<Widget, kN> pool;
    std::vector<Widget*> handles;
    handles.reserve(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        Widget* w = pool.allocate();
        REQUIRE(w != nullptr);
        w->value = static_cast<int>(i);
        handles.push_back(w);
    }
    for (std::size_t i = 0; i < kN; ++i) {
        REQUIRE(handles[i]->value == static_cast<int>(i));
    }

    std::unordered_set<Widget*> unique_ptrs(handles.begin(), handles.end());
    REQUIRE(unique_ptrs.size() == kN);
}

TEST_CASE("SlabPool: full allocate/deallocate/reallocate cycles never exceed capacity", "[memory][slab]") {
    constexpr std::size_t kCapacity = 8;
    jane::SlabPool<Widget, kCapacity> pool;
    std::vector<Widget*> live;

    for (int round = 0; round < 1000; ++round) {
        if (live.size() < kCapacity && (round % 3 != 0)) {
            Widget* w = pool.allocate();
            REQUIRE(w != nullptr);
            live.push_back(w);
        } else if (!live.empty()) {
            pool.deallocate(live.back());
            live.pop_back();
        }
        REQUIRE(pool.live_count() == live.size());
    }
}

// --- PmrPool --------------------------------------------------------------

TEST_CASE("PmrPool: allocate returns default-constructed, distinct objects", "[memory][pmr]") {
    jane::PmrPool<Widget> pool(4);
    Widget* a = pool.allocate();
    Widget* b = pool.allocate();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a != b);
    REQUIRE(a->value == -1);
    pool.deallocate(a);
    pool.deallocate(b);
}

TEST_CASE("PmrPool: exhaustion throws bad_alloc rather than falling back to the heap",
          "[memory][pmr]") {
    jane::PmrPool<Widget> pool(2);
    Widget* a = pool.allocate();
    Widget* b = pool.allocate();
    REQUIRE_THROWS_AS(pool.allocate(), std::bad_alloc);
    pool.deallocate(a);
    pool.deallocate(b);
}

TEST_CASE("PmrPool: writes to one object never alias another", "[memory][pmr]") {
    constexpr std::size_t kN = 256;
    jane::PmrPool<Widget> pool(kN);
    std::vector<Widget*> handles;
    handles.reserve(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        Widget* w = pool.allocate();
        REQUIRE(w != nullptr);
        w->value = static_cast<int>(i);
        handles.push_back(w);
    }
    for (std::size_t i = 0; i < kN; ++i) {
        REQUIRE(handles[i]->value == static_cast<int>(i));
    }
    for (Widget* w : handles) {
        pool.deallocate(w);
    }
}
