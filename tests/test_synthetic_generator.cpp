#include <catch2/catch_test_macros.hpp>

#include "jane/correctness/invariants.hpp"
#include "jane/marketdata/sinks.hpp"
#include "jane/replay/replay_engine.hpp"
#include "jane/replay/synthetic_generator.hpp"

using namespace jane;
using namespace jane::replay;

TEST_CASE("SyntheticOrderGenerator: the same seed reproduces the identical order sequence",
          "[synthetic]") {
    SyntheticOrderGenerator gen_a(SyntheticConfig{.seed = 777});
    SyntheticOrderGenerator gen_b(SyntheticConfig{.seed = 777});

    for (std::uint64_t i = 0; i < 500; ++i) {
        const auto a = gen_a.next_order(i, 1);
        const auto b = gen_b.next_order(i, 1);
        REQUIRE(a.price == b.price);
        REQUIRE(a.quantity == b.quantity);
        REQUIRE(a.side == b.side);
        REQUIRE(a.order_type == b.order_type);
        REQUIRE(gen_a.next_interarrival() == gen_b.next_interarrival());
    }
}

TEST_CASE("SyntheticOrderGenerator: different seeds diverge", "[synthetic]") {
    SyntheticOrderGenerator gen_a(SyntheticConfig{.seed = 1});
    SyntheticOrderGenerator gen_b(SyntheticConfig{.seed = 2});

    bool any_difference = false;
    for (std::uint64_t i = 0; i < 100; ++i) {
        const auto a = gen_a.next_order(i, 1);
        const auto b = gen_b.next_order(i, 1);
        if (a.price != b.price || a.quantity != b.quantity || a.side != b.side) {
            any_difference = true;
            break;
        }
    }
    REQUIRE(any_difference);
}

TEST_CASE("SyntheticOrderGenerator: generated prices stay within the configured spread",
          "[synthetic]") {
    SyntheticConfig config{.seed = 5, .mid_price = Price{2000}, .price_spread_ticks = 25};
    SyntheticOrderGenerator gen(config);

    for (std::uint64_t i = 0; i < 5000; ++i) {
        const auto o = gen.next_order(i, 1);
        REQUIRE(o.price >= 2000 - 25);
        REQUIRE(o.price <= 2000 + 25);
        REQUIRE(o.quantity >= config.min_quantity);
        REQUIRE(o.quantity <= config.max_quantity);
    }
}

TEST_CASE("SyntheticOrderGenerator: side/type mix roughly matches configured fractions",
          "[synthetic]") {
    SyntheticConfig config{.seed = 9, .market_order_fraction = 0.2, .buy_fraction = 0.7};
    SyntheticOrderGenerator gen(config);

    constexpr int kN = 20'000;
    int buys = 0, markets = 0;
    for (int i = 0; i < kN; ++i) {
        const auto o = gen.next_order(static_cast<std::uint64_t>(i), 1);
        if (o.side == Side::Buy) ++buys;
        if (o.order_type == OrderType::Market) ++markets;
    }
    const double buy_ratio = static_cast<double>(buys) / kN;
    const double market_ratio = static_cast<double>(markets) / kN;
    // Loose tolerance — this checks the generator isn't wired backwards
    // or ignoring its config, not that it's a perfect RNG.
    REQUIRE(buy_ratio > 0.65);
    REQUIRE(buy_ratio < 0.75);
    REQUIRE(market_ratio > 0.15);
    REQUIRE(market_ratio < 0.25);
}

TEST_CASE("SyntheticOrderGenerator: inter-arrival times are non-negative and average near "
          "the configured rate",
          "[synthetic]") {
    SyntheticConfig config{.seed = 3, .mean_arrival_rate_per_sec = 500.0};
    SyntheticOrderGenerator gen(config);

    constexpr int kN = 20'000;
    Nanos total{0};
    for (int i = 0; i < kN; ++i) {
        const Nanos gap = gen.next_interarrival();
        REQUIRE(gap.value() >= 0);
        total += gap;
    }
    const double mean_seconds = static_cast<double>(total.value()) / 1e9 / kN;
    const double expected_mean_seconds = 1.0 / config.mean_arrival_rate_per_sec;  // 2ms
    REQUIRE(mean_seconds > expected_mean_seconds * 0.9);
    REQUIRE(mean_seconds < expected_mean_seconds * 1.1);
}

TEST_CASE("SyntheticOrderGenerator: driving a real pipeline for 3000 orders never violates "
          "book invariants",
          "[synthetic][correctness]") {
    marketdata::InMemorySink sink;
    matching::MatchingEngine<400, 4000> engine(SymbolId{1}, Price{1000});
    risk::RiskEngine<64> risk(risk::Limits{.max_order_size = Quantity{1'000'000},
                                            .max_position = Quantity{1'000'000},
                                            .max_loss_per_client = PnL{-1'000'000'000}});
    marketdata::MarketDataPublisher<marketdata::InMemorySink> feed(sink);
    ReplayEngine<400, 4000, 64, marketdata::InMemorySink> replay(engine, risk, feed);

    SyntheticConfig config{.seed = 2026, .mid_price = Price{1200}, .price_spread_ticks = 150};
    SyntheticOrderGenerator gen(config);

    DeterministicClock clock;
    for (std::uint64_t i = 0; i < 3000; ++i) {
        const auto msg = gen.next_order(i + 1, static_cast<std::uint32_t>(1 + i % 10));
        replay.process_new_order(msg, clock.tick());
        clock.advance(gen.next_interarrival());

        const auto violations = jane::correctness::check_book_invariants(engine.book());
        REQUIRE(violations.empty());
    }
    REQUIRE(engine.book().order_count() > 0);
}
