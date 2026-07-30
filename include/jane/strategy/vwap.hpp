#pragma once

#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <vector>

#include "jane/core/types.hpp"
#include "jane/protocol/messages.hpp"

// Slices one parent order into N child orders sized against a stylized
// U-shaped volume curve (heavier at the start and end of the execution
// window, lighter in the middle — the well-known real-world intraday
// volume pattern, reproduced here as a smooth synthetic curve rather
// than sourced from real historical volume, which is out of scope for a
// synthetic exchange with no real market data to draw one from). Each
// child is submitted IOC at a limit price, protecting against paying
// through a level that's moved unfavorably rather than resting and
// signaling intent.
namespace jane::strategy {

struct VwapConfig {
    SymbolId symbol;
    ClientId client;
    Side side;
    std::int64_t total_quantity;
    int num_slices = 10;  // must be >= 1
    Price limit_price{0};  // never trade worse than this
};

class VwapExecutor {
public:
    VwapExecutor(VwapConfig config, std::uint64_t first_order_id)
        : config_(config), next_order_id_(first_order_id), slice_sizes_(build_slice_sizes(config)) {}

    [[nodiscard]] bool done() const noexcept {
        return static_cast<std::size_t>(slices_sent_) >= slice_sizes_.size();
    }

    // nullopt once done() — every slice's quantities sum to exactly
    // config.total_quantity (the last slice absorbs the rounding
    // remainder from the curve-weighted split; see build_slice_sizes).
    [[nodiscard]] std::optional<protocol::NewOrderMessage> next_slice() noexcept {
        if (done()) {
            return std::nullopt;
        }
        const std::int64_t qty = slice_sizes_[static_cast<std::size_t>(slices_sent_)];
        ++slices_sent_;
        return protocol::NewOrderMessage{
            .order_id = next_order_id_++,
            .price = config_.limit_price.value(),
            .quantity = qty,
            .client_id = config_.client.value(),
            .symbol_id = config_.symbol.value(),
            .side = config_.side,
            .order_type = OrderType::Limit,
            .time_in_force = TimeInForce::IOC,
        };
    }

    [[nodiscard]] int slices_sent() const noexcept { return slices_sent_; }
    [[nodiscard]] const std::vector<std::int64_t>& slice_sizes() const noexcept { return slice_sizes_; }

private:
    // Weight(t) = 1 + cos(2*pi*t) for t in [0,1): peaks at t=0 and t=1
    // (start/end of the window), minimum at t=0.5 (midday lull) — a
    // smooth, deterministic stand-in for a real U-shaped volume curve.
    // Normalized so the weights sum to exactly 1, then scaled to
    // integer quantities; the last slice takes whatever's left over so
    // the total always reconciles exactly regardless of rounding.
    [[nodiscard]] static std::vector<std::int64_t> build_slice_sizes(const VwapConfig& config) {
        std::vector<double> weights(static_cast<std::size_t>(config.num_slices));
        double sum = 0.0;
        for (int i = 0; i < config.num_slices; ++i) {
            const double t = config.num_slices > 1
                                  ? static_cast<double>(i) / static_cast<double>(config.num_slices - 1)
                                  : 0.0;
            const double w = 1.0 + std::cos(2.0 * std::numbers::pi * t);
            weights[static_cast<std::size_t>(i)] = w;
            sum += w;
        }

        std::vector<std::int64_t> sizes(static_cast<std::size_t>(config.num_slices));
        std::int64_t allocated = 0;
        for (int i = 0; i < config.num_slices - 1; ++i) {
            const auto qty = static_cast<std::int64_t>(std::llround(
                static_cast<double>(config.total_quantity) * (weights[static_cast<std::size_t>(i)] / sum)));
            sizes[static_cast<std::size_t>(i)] = qty;
            allocated += qty;
        }
        sizes[static_cast<std::size_t>(config.num_slices - 1)] = config.total_quantity - allocated;
        return sizes;
    }

    VwapConfig config_;
    std::uint64_t next_order_id_;
    std::vector<std::int64_t> slice_sizes_;
    int slices_sent_ = 0;
};

}  // namespace jane::strategy
