#include <catch2/catch_test_macros.hpp>

#include <random>
#include <unordered_map>
#include <vector>

#include "jane/correctness/invariants.hpp"
#include "jane/matching/matching_engine.hpp"

using namespace jane;
using namespace jane::matching;
using jane::correctness::check_book_invariants;

namespace {

using Engine = MatchingEngine<200, 4000>;  // prices [1000, 1200)

// Independent per-order ledger the test maintains itself, cross-checked
// against the engine's actual state after every single action. Any
// divergence means either the matching engine or the book has a real bug
// — this is not testing a specific scenario, it's testing that "what the
// test believes happened" and "what the engine says happened" never
// disagree, over thousands of randomly generated actions.
using Ledger = std::unordered_map<std::uint64_t, Quantity>;

void apply_fills(Ledger& ledger, const std::vector<Fill>& fills, std::size_t start,
                  std::size_t count) {
    for (std::size_t i = start; i < start + count; ++i) {
        const Fill& f = fills[i];
        auto it = ledger.find(f.resting_order_id.value());
        REQUIRE(it != ledger.end());
        it->second -= f.quantity;
        REQUIRE(it->second.value() >= 0);
        if (it->second.value() == 0) {
            ledger.erase(it);
        }
    }
}

void verify_ledger_matches_book(const Ledger& ledger, const Engine& engine) {
    for (const auto& [id, qty] : ledger) {
        const auto* node = engine.book().find(OrderId{id});
        REQUIRE(node != nullptr);
        REQUIRE(node->order.remaining.value() == qty.value());
    }
    REQUIRE(engine.book().order_count() == ledger.size());
}

}  // namespace

TEST_CASE("Correctness: random session never violates book invariants or loses/creates volume",
          "[correctness][property]") {
    std::mt19937_64 rng(0xC0FFEE'D00D);
    std::uniform_int_distribution<int> action_dist(0, 9);   // 0-5 submit, 6-7 cancel, 8-9 replace
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> type_dist(0, 4);     // 0-3 limit, 4 market
    std::uniform_int_distribution<int> tif_dist(0, 9);      // 0-6 day, 7-8 ioc, 9 fok
    std::uniform_int_distribution<std::int64_t> price_dist(1000, 1099);  // narrow: forces crossing
    std::uniform_int_distribution<std::int64_t> qty_dist(1, 50);

    Engine engine(SymbolId{1}, Price{1000});
    Ledger ledger;
    std::vector<Fill> fills;
    std::vector<OrderId> ever_live;  // candidates for cancel/replace (may already be gone)
    std::uint64_t next_id = 1;

    constexpr int kIterations = 8000;
    for (int iter = 0; iter < kIterations; ++iter) {
        const int action = action_dist(rng);

        if (action <= 5 || ever_live.empty()) {
            Order o{};
            o.id = OrderId{next_id++};
            o.client = ClientId{1};
            o.symbol = SymbolId{1};
            o.side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            o.type = (type_dist(rng) < 4) ? OrderType::Limit : OrderType::Market;
            const int tif_roll = tif_dist(rng);
            o.tif = (tif_roll <= 6) ? TimeInForce::Day : (tif_roll <= 8 ? TimeInForce::IOC : TimeInForce::FOK);
            o.price = Price{price_dist(rng)};
            o.quantity = Quantity{qty_dist(rng)};
            o.remaining = o.quantity;

            fills.clear();
            auto result = engine.submit(o, fills);
            if (result.reject_reason == RejectReason::None) {
                apply_fills(ledger, fills, 0, result.fill_count);
                if (result.rested) {
                    ledger[o.id.value()] = result.remaining_after;
                    ever_live.push_back(o.id);
                }
            }
        } else if (action <= 7) {
            const OrderId victim = ever_live[static_cast<std::size_t>(rng()) % ever_live.size()];
            auto result = engine.cancel(victim);
            if (result.accepted) {
                ledger.erase(victim.value());
            }
        } else {
            const OrderId victim = ever_live[static_cast<std::size_t>(rng()) % ever_live.size()];
            fills.clear();
            auto result = engine.replace(victim, Price{price_dist(rng)}, Quantity{qty_dist(rng)},
                                          Sequence{static_cast<std::uint64_t>(iter)}, Nanos{0}, fills);
            if (result.accepted) {
                ledger.erase(victim.value());
                apply_fills(ledger, fills, 0, result.fill_count);
                if (result.rested) {
                    ledger[victim.value()] = result.remaining_after;
                    ever_live.push_back(victim);
                }
            }
        }

        // The two things this whole test exists to check, every single
        // step, not just at the end:
        verify_ledger_matches_book(ledger, engine);
        const auto violations = check_book_invariants(engine.book());
        if (!violations.empty()) {
            INFO("iteration " << iter << " produced " << violations.size() << " violation(s)");
            for (const auto& v : violations) {
                INFO(v);
            }
            FAIL("book invariant violated");
        }
    }

    // A final sanity check that this test actually exercised something —
    // an assertion that never fires because the loop body never runs is
    // worse than no test at all.
    REQUIRE(next_id > 1000);
    REQUIRE_FALSE(ledger.empty());
}
