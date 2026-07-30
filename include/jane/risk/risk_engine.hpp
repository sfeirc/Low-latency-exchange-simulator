#pragma once

#include <ankerl/unordered_dense.h>

#include <cstddef>
#include <cstdint>

#include "jane/core/order.hpp"
#include "jane/core/types.hpp"

// Pre-trade risk: the gate a driver loop (see jane::replay) calls *before*
// handing an order to jane::matching::MatchingEngine, and updates *after*
// a fill actually happens. RiskEngine has no dependency on OrderBook or
// MatchingEngine and doesn't call into either — composition is the
// caller's job, which keeps this independently testable (every test here
// calls check_new_order/record_fill directly, no book required) and keeps
// the risk logic reusable across order types (nothing about it assumes
// limit-vs-market).
namespace jane::risk {

struct Limits {
    Quantity max_order_size;
    Quantity max_position;      // per (client, symbol), net absolute position
    PnL max_loss_per_client;    // per (client, symbol); PnL must stay >= this (a negative floor)
};

// One (client, symbol) pair's running exposure: net position plus enough
// to mark it to market. PnL is tracked via cash flow rather than a
// per-lot cost basis (real accounting would use FIFO/LIFO lots per
// position) — cash_flow_ + position_ * last_mark_price gives a correct
// mark-to-market P&L for *this* simplified model, which is the right
// amount of P&L machinery for a pre-trade gate and not an accounting
// system.
//
// One real limitation worth being explicit about: `last_mark_price` only
// updates on *this account's own* fills (record_fill sets it to that
// fill's price), not on every trade the symbol sees venue-wide. A client
// who opens a position and then stops trading entirely will show a frozen
// PnL regardless of where the market moves afterward — this is a
// mark-at-my-own-last-trade model, not a live mark-to-market against the
// venue's current best price. Good enough to gate further orders from an
// account that's already trading; not a substitute for a real risk
// system's continuous marking. See docs/tradeoffs.md if this ever needs
// to grow into the real thing.
struct ClientExposure {
    Quantity position{0};
    PnL cash_flow{0};
    Price last_mark_price{0};

    [[nodiscard]] PnL mark_to_market_pnl() const noexcept {
        return cash_flow + PnL{position.value() * last_mark_price.value()};
    }
};

template <std::size_t MaxAccounts>
class RiskEngine {
public:
    explicit RiskEngine(Limits limits) noexcept : limits_(limits) { exposure_.reserve(MaxAccounts); }

    // Read-only: does not mutate any state. Call before submitting the
    // order to matching; call record_fill() after any resulting fill to
    // keep future checks accurate.
    [[nodiscard]] RejectReason check_new_order(const Order& order) const noexcept {
        if (kill_switch_engaged_) {
            return RejectReason::RiskKillSwitch;
        }
        if (order.quantity.value() > limits_.max_order_size.value()) {
            return RejectReason::RiskMaxOrderSize;
        }

        const std::uint64_t key = account_key(order.client, order.symbol);
        const auto it = exposure_.find(key);
        const Quantity current_position = (it != exposure_.end()) ? it->second.position : Quantity{0};
        const std::int64_t delta =
            (order.side == Side::Buy) ? order.quantity.value() : -order.quantity.value();
        const std::int64_t prospective = current_position.value() + delta;
        if (prospective > limits_.max_position.value() || prospective < -limits_.max_position.value()) {
            return RejectReason::RiskMaxPosition;
        }

        if (it != exposure_.end() &&
            it->second.mark_to_market_pnl().value() < limits_.max_loss_per_client.value()) {
            return RejectReason::RiskMaxLoss;
        }

        return RejectReason::None;
    }

    // Updates position, cash flow, and mark price for (client, symbol).
    // `price`/`quantity` are the fill's price and matched quantity;
    // `side` is this client's side of the trade (Buy or Sell), not
    // necessarily the aggressor's — call once per side of every fill, for
    // both the resting order's client and the aggressor's client, since a
    // fill changes both parties' exposure.
    void record_fill(ClientId client, SymbolId symbol, Side side, Price price,
                      Quantity quantity) noexcept {
        ClientExposure& exposure = exposure_[account_key(client, symbol)];
        const std::int64_t signed_qty =
            (side == Side::Buy) ? quantity.value() : -quantity.value();
        exposure.position += Quantity{signed_qty};
        // Buying pays cash out (negative cash flow); selling receives
        // cash (positive) — standard sign convention for cash-flow PnL.
        const std::int64_t proceeds = price.value() * quantity.value();
        exposure.cash_flow += PnL{(side == Side::Sell) ? proceeds : -proceeds};
        exposure.last_mark_price = price;
    }

    [[nodiscard]] Quantity position(ClientId client, SymbolId symbol) const noexcept {
        const auto it = exposure_.find(account_key(client, symbol));
        return (it != exposure_.end()) ? it->second.position : Quantity{0};
    }
    [[nodiscard]] PnL pnl(ClientId client, SymbolId symbol) const noexcept {
        const auto it = exposure_.find(account_key(client, symbol));
        return (it != exposure_.end()) ? it->second.mark_to_market_pnl() : PnL{0};
    }

    // The kill switch is a blunt, venue-wide instrument: while engaged,
    // check_new_order rejects everything regardless of any per-account
    // limit. Deliberately not auto-triggered by this class from an
    // aggregate signal — in a closed matching venue every trade has a
    // buyer and a seller, so summed cash flow (and therefore summed PnL)
    // across every account is always exactly zero; there is no
    // meaningful "aggregate loss" for RiskEngine to compute on its own.
    // A real deployment ties this to something that *is* meaningful
    // (a designated house/market-maker account's own PnL, an anomaly
    // detector, an ops button) and calls engage_kill_switch() from there.
    void engage_kill_switch() noexcept { kill_switch_engaged_ = true; }
    void disengage_kill_switch() noexcept { kill_switch_engaged_ = false; }
    [[nodiscard]] bool kill_switch_engaged() const noexcept { return kill_switch_engaged_; }

private:
    [[nodiscard]] static std::uint64_t account_key(ClientId client, SymbolId symbol) noexcept {
        return (static_cast<std::uint64_t>(client.value()) << 32) | symbol.value();
    }

    Limits limits_;
    bool kill_switch_engaged_ = false;
    ankerl::unordered_dense::map<std::uint64_t, ClientExposure> exposure_;
};

}  // namespace jane::risk
