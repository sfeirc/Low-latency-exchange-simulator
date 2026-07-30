// A live terminal view of the order book, driven by real synthetic order
// flow through the real matching/risk/market-data pipeline — not a mockup.
// Doubles as the data source for the recorded-session animation artifact
// (--record-json): every rendered frame is a real book state produced by
// jane::matching::MatchingEngine, not synthesized for the visualization.
//
// Usage:
//   bookview [--seed=N] [--orders=N] [--depth=N] [--interval=N]
//            [--sleep-ms=N] [--record-json=PATH] [--quiet]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "jane/marketdata/sinks.hpp"
#include "jane/matching/matching_engine.hpp"
#include "jane/replay/replay_engine.hpp"
#include "jane/replay/synthetic_generator.hpp"

using namespace jane;

namespace {

struct Args {
    std::uint64_t seed = 42;
    int orders = 5000;
    int depth = 8;
    int interval = 5;      // render every N processed orders
    int sleep_ms = 40;     // delay between renders, for watchability
    std::string record_json_path;
    bool quiet = false;
};

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value_after = [&](const std::string& prefix) -> std::string {
            return arg.substr(prefix.size());
        };
        if (arg.starts_with("--seed=")) {
            args.seed = std::stoull(value_after("--seed="));
        } else if (arg.starts_with("--orders=")) {
            args.orders = std::stoi(value_after("--orders="));
        } else if (arg.starts_with("--depth=")) {
            args.depth = std::stoi(value_after("--depth="));
        } else if (arg.starts_with("--interval=")) {
            args.interval = std::stoi(value_after("--interval="));
        } else if (arg.starts_with("--sleep-ms=")) {
            args.sleep_ms = std::stoi(value_after("--sleep-ms="));
        } else if (arg.starts_with("--record-json=")) {
            args.record_json_path = value_after("--record-json=");
        } else if (arg == "--quiet") {
            args.quiet = true;
        } else {
            std::fprintf(stderr, "unrecognized argument: %s\n", arg.c_str());
            std::exit(2);
        }
    }
    return args;
}

// ANSI color helpers — bids green, asks red, matching every retail and
// professional trading terminal's convention.
constexpr const char* kReset = "\033[0m";
constexpr const char* kGreen = "\033[32m";
constexpr const char* kRed = "\033[31m";
constexpr const char* kBold = "\033[1m";
constexpr const char* kDim = "\033[2m";
constexpr const char* kClearAndHome = "\033[2J\033[H";

struct LevelView {
    std::int64_t price;
    std::int64_t qty;
    std::uint32_t count;
};

struct TradeView {
    std::int64_t price;
    std::int64_t qty;
};

// Collects the top `depth` levels of each side directly from the live
// book — the same data structure the matching engine itself uses, walked
// through the same for_each_level primitive jane::marketdata uses for
// real snapshots (see include/jane/book/order_book.hpp).
template <typename Book>
void collect_levels(const Book& book, Side side, int depth, std::vector<LevelView>& out) {
    out.clear();
    book.for_each_level(side, [&](Price price, const book::PriceLevel& lvl) {
        out.push_back(LevelView{price.value(), lvl.total_quantity.value(), lvl.order_count});
        return static_cast<int>(out.size()) < depth;
    });
}

void render_frame(int order_index, int total_orders, const std::vector<LevelView>& bids,
                   const std::vector<LevelView>& asks, const TradeView* last_trade,
                   std::int64_t max_qty_seen) {
    std::string out;
    out += kClearAndHome;
    out += kBold;
    out += "JANE — live order book (synthetic session)\n";
    out += kReset;
    out += kDim;
    out += "order " + std::to_string(order_index) + " / " + std::to_string(total_orders) + "\n\n";
    out += kReset;

    const std::int64_t bar_scale = std::max<std::int64_t>(1, max_qty_seen);
    constexpr int kBarWidth = 24;

    auto bar = [&](std::int64_t qty) {
        const int filled = static_cast<int>((qty * kBarWidth) / bar_scale);
        return std::string(static_cast<std::size_t>(std::max(0, std::min(kBarWidth, filled))), '#');
    };

    char line[256];

    // Asks: worst (furthest from spread) to best, so the best ask sits
    // just above the spread line — the standard order-book ladder layout.
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
        std::snprintf(line, sizeof(line), "  %8lld  %-24s  qty=%-6lld  n=%u",
                      static_cast<long long>(it->price), bar(it->qty).c_str(),
                      static_cast<long long>(it->qty), it->count);
        out += kRed;
        out += line;
        out += kReset;
        out += "\n";
    }

    out += kDim;
    out += "  --------------------------------------------------------------\n";
    if (last_trade != nullptr) {
        std::snprintf(line, sizeof(line), "  last trade: %lld @ qty %lld",
                      static_cast<long long>(last_trade->price), static_cast<long long>(last_trade->qty));
        out += line;
        out += "\n";
    }
    out += "  --------------------------------------------------------------\n";
    out += kReset;

    for (const auto& lvl : bids) {
        std::snprintf(line, sizeof(line), "  %8lld  %-24s  qty=%-6lld  n=%u",
                      static_cast<long long>(lvl.price), bar(lvl.qty).c_str(),
                      static_cast<long long>(lvl.qty), lvl.count);
        out += kGreen;
        out += line;
        out += kReset;
        out += "\n";
    }

    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);
}

void write_json_array(const std::string& path, const std::vector<std::string>& frames) {
    std::ofstream out(path, std::ios::trunc);
    out << "[\n";
    for (std::size_t i = 0; i < frames.size(); ++i) {
        out << frames[i];
        if (i + 1 < frames.size()) out << ",";
        out << "\n";
    }
    out << "]\n";
}

std::string frame_to_json(int order_index, const std::vector<LevelView>& bids,
                           const std::vector<LevelView>& asks, const TradeView* last_trade) {
    auto levels_json = [](const std::vector<LevelView>& levels) {
        std::string s = "[";
        for (std::size_t i = 0; i < levels.size(); ++i) {
            if (i > 0) s += ",";
            s += "{\"price\":" + std::to_string(levels[i].price) +
                 ",\"qty\":" + std::to_string(levels[i].qty) +
                 ",\"count\":" + std::to_string(levels[i].count) + "}";
        }
        s += "]";
        return s;
    };

    std::string s = "  {\"order_index\":" + std::to_string(order_index);
    s += ",\"bids\":" + levels_json(bids);
    s += ",\"asks\":" + levels_json(asks);
    if (last_trade != nullptr) {
        s += ",\"last_trade\":{\"price\":" + std::to_string(last_trade->price) +
             ",\"qty\":" + std::to_string(last_trade->qty) + "}";
    } else {
        s += ",\"last_trade\":null";
    }
    s += "}";
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    constexpr std::size_t kNumLevels = 4096;
    constexpr std::size_t kMaxOrders = 200'000;
    constexpr std::size_t kMaxAccounts = 64;
    constexpr std::int64_t kBasePrice = 8800;

    marketdata::InMemorySink sink;
    // Heap-allocated: MatchingEngine owns its OrderBook by value, which
    // owns a SlabPool<OrderNode, kMaxOrders> by value — tens of megabytes
    // at this capacity, well past a default thread stack. Same fix as
    // bench_end_to_end.cpp's bench_end_to_end_pipeline_percentiles, same
    // root cause (see that file's comment for the first time this bit).
    auto engine_ptr = std::make_unique<matching::MatchingEngine<kNumLevels, kMaxOrders>>(
        SymbolId{1}, Price{kBasePrice});
    auto& engine = *engine_ptr;
    risk::RiskEngine<kMaxAccounts> risk(
        risk::Limits{.max_order_size = Quantity{1'000'000},
                     .max_position = Quantity{10'000'000},
                     .max_loss_per_client = PnL{-1'000'000'000'000LL}});
    marketdata::MarketDataPublisher<marketdata::InMemorySink> feed(sink, 1u << 20);
    replay::ReplayEngine<kNumLevels, kMaxOrders, kMaxAccounts, marketdata::InMemorySink> pipeline(
        engine, risk, feed);
    replay::SyntheticOrderGenerator gen(replay::SyntheticConfig{
        .seed = args.seed,
        .symbol = SymbolId{1},
        .mid_price = Price{9000},
        .price_spread_ticks = 150,
        .min_quantity = 1,
        .max_quantity = 100,
        .market_order_fraction = 0.05,
        .buy_fraction = 0.5,
    });
    replay::DeterministicClock clock;

    std::vector<LevelView> bids, asks;
    std::optional<TradeView> last_trade;
    std::int64_t max_qty_seen = 1;
    std::vector<std::string> json_frames;
    const bool recording = !args.record_json_path.empty();

    for (int i = 1; i <= args.orders; ++i) {
        const auto msg = gen.next_order(static_cast<std::uint64_t>(i),
                                         static_cast<std::uint32_t>(1 + i % 20));
        const bool was_market = (msg.order_type == OrderType::Market);
        const auto price_before_ask = engine.book().best_ask();
        const auto price_before_bid = engine.book().best_bid();

        pipeline.process_new_order(msg, clock.tick());

        // A trade happened if a market order was ever going to (a Limit
        // that doesn't cross never trades) or the touch moved — cheap
        // heuristic good enough for a visualization, not used for
        // anything else. Real trade detection (jane::matching::Fill)
        // happens inside the pipeline already.
        if (was_market || engine.book().best_ask() != price_before_ask ||
            engine.book().best_bid() != price_before_bid) {
            const auto* level = engine.book().level_at(msg.side, Price{msg.price});
            if (level != nullptr) {
                last_trade = TradeView{msg.price, msg.quantity};
            }
        }

        if (i % args.interval == 0 || i == args.orders) {
            collect_levels(engine.book(), Side::Buy, args.depth, bids);
            collect_levels(engine.book(), Side::Sell, args.depth, asks);
            for (const auto& l : bids) max_qty_seen = std::max(max_qty_seen, l.qty);
            for (const auto& l : asks) max_qty_seen = std::max(max_qty_seen, l.qty);

            if (!args.quiet) {
                render_frame(i, args.orders, bids, asks, last_trade ? &*last_trade : nullptr,
                             max_qty_seen);
                if (args.sleep_ms > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(args.sleep_ms));
                }
            }
            if (recording) {
                json_frames.push_back(frame_to_json(i, bids, asks, last_trade ? &*last_trade : nullptr));
            }
        }
    }

    if (recording) {
        write_json_array(args.record_json_path, json_frames);
        std::fprintf(stderr, "wrote %zu frames to %s\n", json_frames.size(),
                     args.record_json_path.c_str());
    }
    return 0;
}
