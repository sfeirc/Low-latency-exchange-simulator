#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <random>
#include <vector>

#include "jane/book/bitmap.hpp"

using jane::book::Bitmap;

namespace {
// Reference implementation: obviously correct, O(n), used to cross-check
// the bit-scan version's output over many random trials.
std::optional<std::size_t> brute_highest_at_or_below(const std::vector<bool>& bits, std::size_t i) {
    if (bits.empty()) return std::nullopt;
    if (i >= bits.size()) i = bits.size() - 1;
    for (std::size_t k = i + 1; k-- > 0;) {
        if (bits[k]) return k;
    }
    return std::nullopt;
}
std::optional<std::size_t> brute_lowest_at_or_above(const std::vector<bool>& bits, std::size_t i) {
    for (std::size_t k = i; k < bits.size(); ++k) {
        if (bits[k]) return k;
    }
    return std::nullopt;
}
}  // namespace

TEST_CASE("Bitmap: set/clear/test basics", "[bitmap]") {
    Bitmap bm(128);
    REQUIRE_FALSE(bm.test(0));
    bm.set(0);
    REQUIRE(bm.test(0));
    bm.clear(0);
    REQUIRE_FALSE(bm.test(0));

    bm.set(63);
    bm.set(64);
    bm.set(127);
    REQUIRE(bm.test(63));
    REQUIRE(bm.test(64));
    REQUIRE(bm.test(127));
    REQUIRE_FALSE(bm.test(65));
}

TEST_CASE("Bitmap: find_* return nullopt when nothing is set", "[bitmap]") {
    Bitmap bm(200);
    REQUIRE_FALSE(bm.find_highest_at_or_below(199).has_value());
    REQUIRE_FALSE(bm.find_lowest_at_or_above(0).has_value());
}

TEST_CASE("Bitmap: find_highest_at_or_below basic placements", "[bitmap]") {
    Bitmap bm(200);
    bm.set(50);
    REQUIRE(bm.find_highest_at_or_below(50) == 50);   // exactly at query
    REQUIRE(bm.find_highest_at_or_below(199) == 50);  // query above
    REQUIRE_FALSE(bm.find_highest_at_or_below(49).has_value());  // query below: nothing there

    bm.set(150);
    REQUIRE(bm.find_highest_at_or_below(199) == 150);  // picks the higher of two candidates
    REQUIRE(bm.find_highest_at_or_below(149) == 50);   // excludes the one above query
}

TEST_CASE("Bitmap: find_lowest_at_or_above basic placements", "[bitmap]") {
    Bitmap bm(200);
    bm.set(50);
    REQUIRE(bm.find_lowest_at_or_above(50) == 50);
    REQUIRE(bm.find_lowest_at_or_above(0) == 50);
    REQUIRE_FALSE(bm.find_lowest_at_or_above(51).has_value());

    bm.set(150);
    REQUIRE(bm.find_lowest_at_or_above(0) == 50);    // picks the lower of two candidates
    REQUIRE(bm.find_lowest_at_or_above(51) == 150);  // excludes the one below query
}

TEST_CASE("Bitmap: word-boundary positions (63/64, 127/128)", "[bitmap]") {
    Bitmap bm(256);
    bm.set(63);
    REQUIRE(bm.find_highest_at_or_below(63) == 63);
    REQUIRE(bm.find_lowest_at_or_above(63) == 63);
    REQUIRE(bm.find_lowest_at_or_above(0) == 63);
    REQUIRE_FALSE(bm.find_lowest_at_or_above(64).has_value());

    bm.clear(63);
    bm.set(64);
    REQUIRE(bm.find_highest_at_or_below(64) == 64);
    REQUIRE_FALSE(bm.find_highest_at_or_below(63).has_value());
    REQUIRE(bm.find_lowest_at_or_above(64) == 64);
}

TEST_CASE("Bitmap: out-of-range query index is handled, not UB", "[bitmap]") {
    Bitmap bm(100);
    bm.set(10);
    bm.set(90);
    REQUIRE(bm.find_highest_at_or_below(100000) == 90);  // clamps to last valid bit
    REQUIRE_FALSE(bm.find_lowest_at_or_above(100000).has_value());
    REQUIRE_FALSE(bm.find_lowest_at_or_above(100).has_value());  // exactly one past the end
}

TEST_CASE("Bitmap: last bit of a non-multiple-of-64 size is reachable", "[bitmap]") {
    Bitmap bm(70);  // spans two words, second word only has 6 valid bits
    bm.set(69);
    REQUIRE(bm.find_highest_at_or_below(69) == 69);
    REQUIRE(bm.find_lowest_at_or_above(65) == 69);
}

TEST_CASE("Bitmap: matches a brute-force reference over random bitmaps and queries", "[bitmap][fuzz]") {
    std::mt19937_64 rng(42);

    for (int trial = 0; trial < 500; ++trial) {
        std::uniform_int_distribution<std::size_t> size_dist(1, 500);
        const std::size_t n = size_dist(rng);
        Bitmap bm(n);
        std::vector<bool> reference(n, false);

        std::bernoulli_distribution density(0.05);  // sparse-ish, more interesting than 50/50
        for (std::size_t i = 0; i < n; ++i) {
            if (density(rng)) {
                bm.set(i);
                reference[i] = true;
            }
        }

        std::uniform_int_distribution<std::size_t> query_dist(0, n - 1);
        for (int q = 0; q < 50; ++q) {
            const std::size_t idx = query_dist(rng);
            REQUIRE(bm.find_highest_at_or_below(idx) == brute_highest_at_or_below(reference, idx));
            REQUIRE(bm.find_lowest_at_or_above(idx) == brute_lowest_at_or_above(reference, idx));
        }
    }
}
