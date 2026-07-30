#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <random>
#include <set>

#include "jane/book/price_index_variants.hpp"

using namespace jane::book::variants;

namespace {
constexpr std::int64_t kRange = 4096;
}

TEST_CASE("price index variants: basic insert/erase/best agree", "[book][price_index]") {
    LadderIndexVariant ladder(kRange);
    TreeIndexVariant tree;
    FlatIndexVariant flat;

    REQUIRE_FALSE(ladder.best().has_value());
    REQUIRE_FALSE(tree.best().has_value());
    REQUIRE_FALSE(flat.best().has_value());

    ladder.insert(10);
    tree.insert(10);
    flat.insert(10);
    REQUIRE(ladder.best() == 10);
    REQUIRE(tree.best() == 10);
    REQUIRE(flat.best() == 10);

    ladder.insert(50);
    tree.insert(50);
    flat.insert(50);
    REQUIRE(ladder.best() == 50);
    REQUIRE(tree.best() == 50);
    REQUIRE(flat.best() == 50);

    ladder.erase(50);
    tree.erase(50);
    flat.erase(50);
    REQUIRE(ladder.best() == 10);
    REQUIRE(tree.best() == 10);
    REQUIRE(flat.best() == 10);

    ladder.erase(10);
    tree.erase(10);
    flat.erase(10);
    REQUIRE_FALSE(ladder.best().has_value());
    REQUIRE_FALSE(tree.best().has_value());
    REQUIRE_FALSE(flat.best().has_value());
}

TEST_CASE("price index variants: duplicate insert and erase-of-absent are no-ops", "[book][price_index]") {
    LadderIndexVariant ladder(kRange);
    TreeIndexVariant tree;
    FlatIndexVariant flat;

    ladder.insert(5);
    ladder.insert(5);  // duplicate
    tree.insert(5);
    tree.insert(5);
    flat.insert(5);
    flat.insert(5);
    REQUIRE(ladder.best() == 5);
    REQUIRE(tree.best() == 5);
    REQUIRE(flat.best() == 5);

    ladder.erase(999);  // never inserted
    tree.erase(999);
    flat.erase(999);
    REQUIRE(ladder.best() == 5);
    REQUIRE(tree.best() == 5);
    REQUIRE(flat.best() == 5);
}

TEST_CASE("price index variants: all three agree over a long random operation sequence",
          "[book][price_index][fuzz]") {
    LadderIndexVariant ladder(kRange);
    TreeIndexVariant tree;
    FlatIndexVariant flat;
    std::set<std::int64_t> reference;  // brute-force oracle

    std::mt19937_64 rng(7);
    std::uniform_int_distribution<std::int64_t> price_dist(0, kRange - 1);
    std::bernoulli_distribution insert_or_erase(0.6);  // bias toward growing the book a bit

    for (int i = 0; i < 20'000; ++i) {
        const std::int64_t p = price_dist(rng);
        if (insert_or_erase(rng) || reference.empty()) {
            ladder.insert(p);
            tree.insert(p);
            flat.insert(p);
            reference.insert(p);
        } else {
            ladder.erase(p);
            tree.erase(p);
            flat.erase(p);
            reference.erase(p);
        }

        const std::optional<std::int64_t> expected =
            reference.empty() ? std::nullopt : std::optional<std::int64_t>(*reference.rbegin());
        REQUIRE(ladder.best() == expected);
        REQUIRE(tree.best() == expected);
        REQUIRE(flat.best() == expected);
    }
}
