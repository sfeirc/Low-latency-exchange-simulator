# JANE

A from-scratch electronic exchange core, in modern C++23: order entry, a
price/time-priority limit order book, FIFO matching, pre-trade risk controls,
a binary market-data feed, and the plumbing (lock-free ring buffers, a slab
allocator, a wire protocol) that makes it all run without allocating or
locking on the hot path.

Built to be *measured*, not asserted: every claim in [`docs/benchmarks.md`](docs/benchmarks.md)
is a number produced by `bench/` on a pinned core of a Ryzen 9 5900X, reproducible
via `tools/`. See [`docs/tradeoffs.md`](docs/tradeoffs.md) for why each design
decision was made, with comparative data, not opinion.

## Architecture

```
 Market-data replay / clients
              |
      Binary protocol parser
              |
      Lock-free event pipeline        (N clients -> per-client SPSC -> fan-in sequencer)
              |
       Limit Order Book engine        (price/time priority, FIFO per level)
              |
      Matching + risk checks          (pre-trade: size / position / loss / kill switch)
              |
 Trades / market-data feed / metrics  (snapshots + incremental deltas, p50/p99/p99.9)
```

See [`docs/architecture.md`](docs/architecture.md) for the full component diagram,
threading model, and message flow.

## Features

- [x] Limit order book: price/time priority, FIFO per price level
- [x] Order types: limit, market, cancel, replace
- [x] FIFO matching with partial fills
- [x] Market data: full snapshots + incremental deltas
- [x] Deterministic replay (historical log or seeded synthetic flow) for bug repro
- [x] Pre-trade risk: max order size, max position, max loss, kill switch
- [x] Strategy simulators: market maker, VWAP, arbitrage
- [x] Latency benchmarks: p50/p99/p99.9 per operation, measured not claimed
- [x] Correctness suite: volume conservation, strict FIFO, no crossed/impossible trades

Every box above is backed by tests, not just present in source — see
[`docs/correctness.md`](docs/correctness.md) for what's checked and where.

## Repository layout

```
include/jane/    public headers (the engine is largely header-only templates)
  core/          strong types: Price, Quantity, OrderId, Sequence, ClientId, SymbolId, Nanos, PnL, Order
  concurrency/   SPSC/MPSC ring buffers, multi-client fan-in sequencer
  memory/        slab allocator / pmr pools
  protocol/      binary wire messages, encode/decode
  book/          limit order book (ladder / flat-vector / std::map backends)
  matching/      matching engine
  risk/          pre-trade risk gate
  marketdata/    snapshot + incremental feed publisher
  replay/        replay engine + synthetic order-flow generator
  strategy/      market maker / VWAP / arbitrage simulators
  metrics/       latency histograms, counters
  correctness/   structural invariant checker (volume, FIFO, no crossed book)
src/             non-template implementation
apps/            bookview: live terminal order-book viewer + JSON session recorder
tests/           Catch2 unit + property/invariant tests
bench/           Google Benchmark micro + end-to-end harness
tools/           benchmark runner (shell) + results analyzer (Python)
artifacts/       benchmark results (JSON) and the recorded-session animation (HTML)
docs/            architecture, benchmarks, trade-offs, correctness write-ups
```

## Building

Dependencies: a C++23 compiler (developed against GCC 15), CMake >= 3.25, Ninja,
Catch2 3, Google Benchmark.

```sh
sudo apt-get install -y ninja-build catch2 libbenchmark-dev

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Useful CMake options (see root `CMakeLists.txt`):

| Option | Default | Purpose |
|---|---|---|
| `JANE_NATIVE_ARCH` | `ON` | `-march=native`; disable for portable binaries / CI |
| `JANE_ENABLE_LTO` | `ON` | interprocedural optimization on Release-like configs |
| `JANE_ENABLE_ASAN` | `OFF` | Address+UB sanitizers (Debug) |
| `JANE_ENABLE_TSAN` | `OFF` | ThreadSanitizer (Debug, exclusive with ASan) |
| `JANE_WERROR` | `OFF` | warnings as errors |

## Running the benchmarks

```sh
./tools/run_benchmarks.sh                              # builds Release itself, pins to an isolated core, writes artifacts/*.json
python3 tools/analyze_benchmarks.py artifacts/*.json    # human-readable summary
```

See [`docs/benchmarks.md`](docs/benchmarks.md) for the results this
actually produced, and [`docs/tradeoffs.md`](docs/tradeoffs.md) for the
comparative benchmarks (SPSC cached-vs-naive, MPSC fan-in-vs-CAS, price-index
ladder-vs-tree-vs-vector, ...) behind each design decision.

## Live viewer and order-book animation

`apps/bookview` drives the real pipeline (`ReplayEngine` over
`MatchingEngine` + `RiskEngine` + `MarketDataPublisher`) through seeded
synthetic order flow and renders the live book as an ANSI terminal ladder:

```sh
./build/apps/bookview --seed=2026 --orders=6000 --interval=20 --depth=10
```

`--record-json=PATH` captures periodic book snapshots to a JSON array
instead of (or alongside) the terminal render.
[`artifacts/book_animation.html`](artifacts/book_animation.html) is a
self-contained replay of exactly that: 300 real frames captured this way
(seed `2026`, 6000 orders), with a price ladder, a cumulative-depth chart,
a trade tape, and playback controls — open the file directly in a
browser, no server required. Every number in it is real matching-engine
output; nothing is synthesized for the visualization.

## License

MIT — see [`LICENSE`](LICENSE).
