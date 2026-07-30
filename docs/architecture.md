# Architecture

This document is the fuller version of the diagram in the README: every
stage, what thread runs it, what crosses a queue versus a plain function
call, and — as directly as [`docs/correctness.md`](correctness.md) and
[`docs/tradeoffs.md`](tradeoffs.md) do — which simplifications are
deliberate scope decisions rather than oversights.

## The pipeline

```
 client 0 ─┐
 client 1 ─┼─> SPSCRingBuffer[N]  ─┐
   ...     │   (one per client)   │
 client N ─┘                      │
                                   v
                        FanInSequencer<N>
                     (round-robin drain, 1 thread)
                                   │
                          assigns Sequence + Nanos
                                   │
                                   v
                          RiskEngine::check_new_order
                             (pre-trade gate)
                          reject ──────────────┐
                                   │            │
                                 accept         │
                                   v            │
                       MatchingEngine::submit   │
                        (book + FIFO matching)  │
                                   │            │
                    Fill[] ────────┤            │
                       │           │            │
                       v           v            v
              RiskEngine::   MarketDataPublisher::  ExecutionReportMessage
              record_fill    publish_trade /            (per request, to
              (both sides)   publish_level_update        the requesting
                                   │                      client)
                                   v
                              Sink::write
                        (InMemorySink / FileSink /
                         a socket, in a real deployment)
```

Everything from `FanInSequencer`'s drain onward — risk, matching, publish
— runs on **one thread**. That is the load-bearing design decision this
whole document exists to explain.

## Why one thread past the sequencer

`MatchingEngine`, `OrderBook`, and `RiskEngine` contain no locks, no
atomics, and no `std::mutex` anywhere in their implementation. That is not
an oversight — it is only sound because exactly one thread ever calls
`submit`/`cancel`/`replace`/`check_new_order`/`record_fill` on a given
instance, a contract stated directly in `matching_engine.hpp`'s header
comment ("everything here assumes single-threaded, sequenced input").
Concurrency in this codebase is deliberately pushed to the *edges* —
ingestion (many client threads) and, in a fuller deployment, egress (many
subscriber connections) — and kept out of the parts of the system where
correctness is hardest to reason about under interleaving. The
alternative (a lock or a lock-free structure *inside* the book) would
mean every one of the invariants in `docs/correctness.md` — strict FIFO,
volume conservation, no crossed trades — would need to be proven under
concurrent mutation instead of proven once, structurally, against
sequential input. This project takes the trade deliberately: push hard
concurrency problems into two small, independently-verified primitives
(`SPSCRingBuffer`, `FanInSequencer`/`MPSCRingBuffer`) instead of smearing
lock-free reasoning across the entire matching core.

`FanInSequencer<T, NumProducers, QueueCapacity>`
([`include/jane/concurrency/fan_in_sequencer.hpp`](../include/jane/concurrency/fan_in_sequencer.hpp))
is what makes that boundary real: each client/gateway thread owns
exclusive write access to its own `SPSCRingBuffer`, so from the queue's
point of view there is genuinely only one producer — reusing an
already-proven, simpler primitive (single-producer/single-consumer) N
times rather than inventing a new N-producer algorithm. The *only* new
concurrency concern `FanInSequencer` introduces is which order the one
sequencer thread drains the N queues in, and that's answered
deliberately: plain round-robin, not an attempt to reconstruct "true"
cross-client arrival order from client-supplied timestamps. A
client-supplied send time isn't trustworthy (clock skew, or a client that
lies) and waiting to see if an earlier-timestamped message is "about to
arrive" from a slower client means either unbounded latency or an
arbitrary wait policy. Real exchanges sequence by arrival at the matching
engine for exactly this reason — "sequence number" and "wall-clock send
order" are related but not the same thing anywhere. See
[`jane::MPSCRingBuffer`](../include/jane/concurrency/mpsc_ring_buffer.hpp)
for the alternative this project also built and benchmarked (a single
shared buffer via Vyukov's bounded MPMC algorithm rather than N queues
fanned in) — `docs/tradeoffs.md` has the measured throughput/latency
comparison between the two ingestion strategies.

## Component walkthrough

Following `include/jane/`, in pipeline order:

**`core/`** — `Price`, `Quantity`, `OrderId`, `Sequence`, `SymbolId`,
`ClientId`, `Nanos`, `PnL`: eight strong types over plain integers
(`StrongAmount`/`StrongId` in `core/strong_type.hpp`), each its own C++
type via a tag struct, so e.g. a `Quantity` can never be passed where a
`Price` is expected without a compile error — a real bug class in
exchange code (mixing up price and quantity arguments) turned into a
type error instead of a runtime one. `Price`/`Quantity`/`PnL` are signed
integers, not floats or `size_t`: integer ticks avoid floating-point
equality/ordering bugs on prices, and signed avoids silent wraparound if
a logic error ever produces a transient negative value.

**`concurrency/`** — `SPSCRingBuffer` (cached-index single-producer/
single-consumer ring), `MPSCRingBuffer` (Vyukov-style bounded MPMC used
as MPSC), `FanInSequencer` (N SPSC queues fanned into one consumer,
described above). The only place `memory_order_acquire`/`release`
reasoning lives in this codebase; verified under ThreadSanitizer, not
just reasoned about (`docs/tradeoffs.md`, `docs/correctness.md` §6).

**`memory/`** — `SlabPool<T, Capacity>` (index-based freelist, fixed
capacity, no allocation after construction) and `PmrPool<T>`
(`std::pmr`-backed, with its own `live_count_`/`capacity_` tracking to
make a real libstdc++ crash unreachable by construction — see
`docs/tradeoffs.md`). `OrderBook` owns a `SlabPool<OrderNode, MaxOrders>`
by value: every resting order lives in a pre-allocated slot, so
add/cancel/replace never call `new`/`delete` on the hot path.

**`protocol/`** — the wire format every stage past decode actually
speaks (`messages.hpp`): `NewOrderMessage`, `CancelOrderMessage`,
`ReplaceOrderMessage`, `ExecutionReportMessage`, `TradeMessage`,
`BookDeltaMessage`, and the variable-length `BookSnapshotHeader` +
`PriceLevelRecord` framing — modeled loosely on NASDAQ ITCH/OUCH, fixed
fields ordered largest-alignment-first so natural alignment alone
produces zero compiler-inserted padding (verified by a `static_assert`
on `sizeof` after every struct — this project never reaches for
`#pragma pack`, which can force misaligned access). `codec.hpp` provides
generic `encode<T>`/`decode<T>` via a compile-time `MessageTraits<T>`
tag, implemented as bounds-checked `memcpy`, not `reinterpret_cast`: a
4-byte `MessageHeader` deliberately does not leave every payload
8-byte-aligned in an arbitrary buffer, so this project chose one
obviously-correct decode path over a compact header *and* a fragile
zero-copy view.

**`book/`** — `OrderBook<NumLevels, MaxOrders>`: a direct-indexed price
ladder (`LadderSide<NumLevels, IsBid>`, one `PriceLevel` per representable
price) plus a `Bitmap` of occupied levels, giving amortized O(1)
relocation of the best price via `std::countl_zero`/`countr_zero`
bit-scan instead of an O(NumLevels) linear scan when the touch is
vacated. Each `PriceLevel` is an intrusive doubly-linked FIFO
(`level_push_back`/`level_unlink`), and a flat hash map
(`ankerl::unordered_dense::map<OrderId, OrderNode*>`) gives O(1)
cancel/replace lookup. `OrderBook` only stores and organizes resting
orders — it has no notion of whether an incoming order *crosses* the
book; that separation is what makes the invariants in
`docs/correctness.md` checkable in isolation, against a class with a much
smaller surface than the full matching engine. `book/price_index_variants.hpp`
holds two comparison-only alternative backends (flat sorted vector,
`std::map`) that exist solely to be benchmarked against the ladder — see
`docs/tradeoffs.md`'s depth-20-vs-2000 reversal.

**`matching/`** — `MatchingEngine<NumLevels, MaxOrders>`: the layer
`OrderBook` deliberately does not implement. `submit()` runs the
FOK pre-trade liquidity check (a read-only walk via `for_each_level`,
proven not to race anything else since input is single-threaded and
sequenced), then `match_against_book()` — walk the opposite side
best-price-first, consume resting FIFO orders via
`OrderBook::reduce_quantity` until the aggressor is filled, the book
stops crossing, or liquidity runs out — then rests any Day-Limit
remainder via `OrderBook::add`. Fills are *appended* to a caller-owned
`std::vector<Fill>&` rather than returned fresh, so a driver processing a
sequenced stream can reuse one buffer across the whole session (see
`docs/tradeoffs.md` for the measured, honestly-modest win). `replace()`
cancels the existing order and re-submits its new terms through this
same crossing-aware `submit()` path, rather than a raw book replace —
the one place a naive implementation could rest a crossed book (moving a
bid above the current best ask), caught by `docs/correctness.md` §3.

**`risk/`** — `RiskEngine<MaxAccounts>`: a pre-trade gate
(`check_new_order`, called *before* `MatchingEngine::submit`) plus
post-fill bookkeeping (`record_fill`, called once per side of every
fill). Max order size, max net position, and max loss are all checked
per `(client, symbol)`, keyed by a packed 64-bit key
(`ClientId << 32 | SymbolId`) into a flat hash map — deliberately no
dependency on `OrderBook` or `MatchingEngine` in either direction, so
every risk test calls these two methods directly with no book required.
PnL is tracked via cash-flow accounting
(`cash_flow + position * last_mark_price`), which is exactly correct for
this model but has one real, documented limitation: `last_mark_price`
only updates on *this account's own* fills, so a client who opens a
position and stops trading shows a frozen PnL regardless of where the
market moves afterward — a mark-at-my-own-last-trade model, not a
continuous mark against the venue's current touch. The kill switch
(`engage_kill_switch()`) is deliberately not self-triggered from any
aggregate signal `RiskEngine` computes internally: in a closed venue
every trade has a buyer and a seller, so summed PnL across every account
is always exactly zero by construction — there is no meaningful
"aggregate loss" for this class to watch on its own. A real deployment
wires the kill switch to something that *is* meaningful (a house
account's own PnL, an anomaly detector, an ops button) from outside this
class.

**`marketdata/`** — `MarketDataPublisher<Sink>`: turns matching output
into wire bytes. `publish_trade`/`publish_level_update` are the hot-path
calls (one `TradeMessage` per fill, one `BookDeltaMessage` per level that
changed); `publish_snapshot` walks top-N depth of both sides into a
variable-length `BookSnapshotHeader` + `PriceLevelRecord[]` framing for a
new subscriber or periodic resync, and is allowed to allocate into
reusable scratch buffers since it isn't called per-order. `Sink` is a
duck-typed template parameter (`void write(std::span<const std::byte>)`)
rather than a virtual interface, so publishing never pays for
indirection on a hot pipeline stage; `InMemorySink` (used by tests, by
`apps/bookview`, and by anything that needs to read back exactly what
was published) and `FileSink` (for recording a session `jane::replay` can
later play back) are the two provided. A real network sink (a UDP
multicast socket, say) would satisfy the same duck-typed contract without
changing anything upstream of it.

Also published through `MarketDataPublisher`:
`publish_execution_report`. Execution reports are conceptually a private
per-client channel, not public market data — routing both through the
same `Sink` is a deliberate scope simplification for this project (a
second, separate publish path buys real-world isolation this project's
scope doesn't need to demonstrate) rather than an accident of not
noticing the distinction.

**`replay/`** — `ReplayEngine<NumLevels, MaxOrders, MaxAccounts, Sink>`:
the driver loop every component above was built to be composed by, and
the only place in this codebase that calls all of `RiskEngine`,
`MatchingEngine`, and `MarketDataPublisher` together. Its three entry
points (`process_new_order`, `process_cancel`, `process_replace`) get the
composition order right — risk check *before* matching; publish market
data *after* the book actually changed, including on a plain cancel
(which still moves a level's aggregate even though nothing traded); stamp
`Sequence`/`Nanos` exactly once per event — and, critically, make that
order deterministic. `process_new_order` publishes **one execution
report per request**, summarizing its net effect, rather than one per
individual fill inside a sweep: `last_quantity` is the sum filled by this
call, and `price` is the last fill's price when a sweep crossed several
levels — an approximation in that specific multi-level-sweep case,
documented here rather than silently imprecise. This project treats "one
report telling the client what happened to their request" as the right
grain for its scope; a venue that needs per-fill granularity on the
private channel would extend this call site, not redesign it.
`DeterministicClock` is a trivial monotonic counter, deliberately not a
wall clock — replay determinism needs "the same input produces the same
output," not "the same input takes the same wall-clock time," and a
caller wanting realistic *relative* timing for benchmarking supplies
their own timestamps instead (see `SyntheticOrderGenerator`'s Poisson
inter-arrival times, next).

`SyntheticOrderGenerator` (`replay/synthetic_generator.hpp`) produces
seeded synthetic order flow — Poisson inter-arrival times, configurable
buy/sell and market/limit fractions, a price band around a mid — for
every benchmark, strategy simulation, and the animation artifact in this
project that isn't replaying a captured session. Same seed, same output,
always: this is what makes `apps/bookview --seed=2026 ...` reproducible.

**`strategy/`** — `MarketMaker`, `VwapExecutor`, `ArbitrageStrategy`:
example clients built entirely on the public wire protocol
(`protocol::NewOrderMessage` / `CancelOrderMessage` / `ReplaceOrderMessage`
in, `ExecutionReportMessage` out) — none of them touch `OrderBook` or
`MatchingEngine` directly, which is itself a small proof that the wire
protocol is a sufficient interface to trade against this exchange. Not
built to be trading-desk-grade alpha; built to exercise realistic client
behavior against the pipeline (resting/replacing/cancelling quotes,
slicing a parent order against a volume curve, reacting to fills) end to
end.

**`metrics/`** — `LatencyHistogram` (an HDR-style linear region plus
log2-octave bucketing, giving real p50/p99/p99.9 rather than an
aggregate mean) and `ThroughputCounter`. Not in the request path at all
— used by `bench/` and by anything instrumenting the pipeline from
outside, never by `MatchingEngine`/`OrderBook` themselves, which stay
free of any timing/measurement concern.

**`correctness/`** — `check_book_invariants<Book>`: a structural checker
(walks the actual FIFO lists and recomputes aggregates independently of
the cached `PriceLevel::total_quantity`/`order_count`) used by the
property-based session test described in `docs/correctness.md` §1 — the
test that found the one real data-corruption bug this project shipped
and then fixed.

## Tracing one order end to end

A `NewOrderMessage` for a marketable limit buy, start to finish:

1. A client thread calls `FanInSequencer::push_from(my_index, msg)` —
   `try_push` onto that client's own `SPSCRingBuffer`. Non-blocking: a
   full queue means back-pressure to the client, not a stall of the
   sequencer.
2. The sequencer thread's `try_drain_one` round-robins to this producer's
   queue, pops the message, and calls
   `ReplayEngine::process_new_order(msg, clock.tick())`.
3. `process_new_order` builds an `Order` from the wire message, assigns
   `Sequence` and the caller-supplied `Nanos` timestamp exactly once, and
   calls `RiskEngine::check_new_order`. Rejected → one
   `ExecutionReportMessage{exec_type: Rejected}` published, done.
4. Accepted → `MatchingEngine::submit`. If the order crosses, this
   returns one or more `Fill`s (FIFO, price improvement — see
   `docs/correctness.md` §§2–3) and, if quantity remains and the order is
   a Day Limit, rests it (`result.rested = true`).
5. For every `Fill`: `MarketDataPublisher::publish_trade`, then
   `RiskEngine::record_fill` for *both* the resting and the aggressor
   client (a fill changes both parties' exposure), then
   `MarketDataPublisher::publish_level_update` for the resting side's
   level (its aggregate just shrank or the level emptied).
6. If the order rested, one more `publish_level_update` for its own
   side/price (the level just gained resting quantity that wasn't there
   before).
7. Exactly one `ExecutionReportMessage` is published for the request as a
   whole — `New`, `PartialFill`, `Fill`, or `Cancelled` (an IOC/FOK/Market
   remainder that was simply dropped), decided from `result` and whether
   any fills occurred.

Two runs fed the identical sequence of `(message, timestamp)` pairs
produce byte-identical published output — proven directly by
`tests/test_replay_engine.cpp`, which runs two independent pipelines over
the same input and diffs their `InMemorySink` contents byte-for-byte.
That test, not just the design description above, is what makes this a
bug-repro tool: a production incident's message log, replayed twice, is
either provably reproduced or provably not, never "probably."

## Scope boundaries

Named directly, matching this project's stance elsewhere
(`docs/correctness.md`'s "What isn't (yet) checked"):

- **No network layer.** `Sink` is satisfied by `InMemorySink`/`FileSink`
  today; the interface (`write(std::span<const std::byte>)`) is deliberately
  narrow enough that a UDP multicast or TCP sink would slot in without
  touching `MarketDataPublisher`, but no such sink is implemented here.
  Likewise, order entry in this project is driven by `apps/bookview` and
  the benchmark/test harnesses calling `ReplayEngine` directly or through
  `SyntheticOrderGenerator` — there's no listening gateway process parsing
  `NewOrderMessage`s off a real socket.
- **No durability.** Nothing is written to a WAL or replicated; a crash
  loses whatever hasn't been flushed to a `FileSink` recording. Real
  venues solve this with the exact "log the input, replay
  deterministically" property this project already has — extending it to
  crash recovery is a matter of writing every sequenced `(message,
  timestamp)` to durable storage before processing it, not a redesign.
- **No portfolio-level risk.** `RiskEngine` limits are per `(client,
  symbol)`; nothing nets exposure across symbols for a client trading a
  correlated basket. `strategy/arbitrage.hpp`'s own test is explicit that
  a hedged position across two symbols shows each leg's PnL independently
  until closed.
- **No self-trade prevention** and **single matching venue per symbol**
  (no smart-order-routing across venues) — both named already in
  `docs/correctness.md`.
- **Single-process, single matching thread per symbol.** Scaling to many
  symbols in this design means one `MatchingEngine` instance (and one
  sequencer thread) per symbol, not a shared engine — which is also why
  `RiskEngine` being independently composable per driver loop matters:
  nothing about it assumes a single global instance either.

None of these are corners cut by accident — each is a line this project
drew to keep its actual subject (the matching core, the concurrency
primitives underneath it, and proving both correct and fast) sharp,
rather than diffusing effort into a full venue's worth of adjacent
systems.
