#pragma once

#include <cstdint>
#include <random>

#include "jane/core/types.hpp"
#include "jane/protocol/messages.hpp"

// Seeded, reproducible synthetic order flow — the same seed always
// produces the same sequence of orders *and* the same inter-arrival
// times. Used for benchmarking (a realistic-shaped, not just uniform,
// input) and as a stress-test input source for the correctness checker
// (see tests/test_synthetic_generator.cpp's invariant-checking run).
namespace jane::replay {

struct SyntheticConfig {
    std::uint64_t seed = 42;
    SymbolId symbol{1};
    Price mid_price{1000};
    std::int64_t price_spread_ticks = 50;       // orders drawn from [mid-spread, mid+spread]
    std::int64_t min_quantity = 1;
    std::int64_t max_quantity = 100;
    double market_order_fraction = 0.1;         // fraction of generated orders that are Market
    double buy_fraction = 0.5;
    double mean_arrival_rate_per_sec = 1000.0;  // Poisson process lambda
};

class SyntheticOrderGenerator {
public:
    explicit SyntheticOrderGenerator(SyntheticConfig config) noexcept
        : config_(config),
          rng_(config.seed),
          buy_dist_(config.buy_fraction),
          market_dist_(config.market_order_fraction),
          interarrival_dist_(config.mean_arrival_rate_per_sec) {}

    [[nodiscard]] protocol::NewOrderMessage next_order(std::uint64_t order_id,
                                                        std::uint32_t client_id) noexcept {
        std::uniform_int_distribution<std::int64_t> price_dist(
            config_.mid_price.value() - config_.price_spread_ticks,
            config_.mid_price.value() + config_.price_spread_ticks);
        std::uniform_int_distribution<std::int64_t> qty_dist(config_.min_quantity,
                                                               config_.max_quantity);

        return protocol::NewOrderMessage{
            .order_id = order_id,
            .price = price_dist(rng_),
            .quantity = qty_dist(rng_),
            .client_id = client_id,
            .symbol_id = config_.symbol.value(),
            .side = buy_dist_(rng_) ? Side::Buy : Side::Sell,
            .order_type = market_dist_(rng_) ? OrderType::Market : OrderType::Limit,
            .time_in_force = TimeInForce::Day,
        };
    }

    // Exponentially-distributed gap to the next arrival — a Poisson
    // arrival process is exactly "inter-arrival times are exponential."
    [[nodiscard]] Nanos next_interarrival() noexcept {
        const double seconds = interarrival_dist_(rng_);
        return Nanos{static_cast<std::int64_t>(seconds * 1e9)};
    }

private:
    SyntheticConfig config_;
    std::mt19937_64 rng_;
    std::bernoulli_distribution buy_dist_;
    std::bernoulli_distribution market_dist_;
    std::exponential_distribution<double> interarrival_dist_;
};

}  // namespace jane::replay
