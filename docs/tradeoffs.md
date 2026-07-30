# Trade-offs

Every design decision below is backed by a number in `artifacts/` produced by
`bench/`, not by intuition. Where a benchmark contradicted the initial
hypothesis, that's reported too — that's the point of measuring.

Machine: AMD Ryzen 9 5900X (12C/24T, SMT2), 30 GiB RAM, Ubuntu, GCC 15.2,
`-O3 -march=native`, LTO on. Threads pinned to distinct *physical* cores
(see `lscpu -e`: on this chip, hyperthread siblings are `CPU i` and
`CPU i+12` — pinning two threads to `i` and `i+12` would silently share L1/L2
and invalidate any cross-core contention measurement).

## Concurrency: SPSC ring buffer — cached index vs. naive re-read

**Decision:** `jane::SPSCRingBuffer` (`include/jane/concurrency/spsc_ring_buffer.hpp`)
has each side (producer/consumer) keep a locally-cached copy of the *other*
side's index, refreshed only when the cache suggests full/empty, instead of
atomically re-reading the other side's index on every call.

**Data** (`artifacts/bench_spsc_ring_buffer.json`, producer pinned to core 2,
consumer to core 3, 32-byte messages, `Iterations=20` at 2^20 ops/iteration):

| Capacity | Cached (M items/s) | Naive re-read (M items/s) | Cached advantage |
|---:|---:|---:|---:|
| 256    | 140.1 | 104.5 | +34% |
| 4,096  | 156.1 |  94.2 | +66% |
| 65,536 | 214.2 |  94.4 | +127% |
| 4,096, consumer does ~20 mul-adds/msg | 78.3 | 71.0 | +10% |

**Reading the data:**

- The advantage *grows* with capacity, not shrinks. This is the opposite of
  "caching only helps for a big buffer because there's more slack to burn
  through" being a minor effect — it's the dominant one. At capacity 256 the
  producer hits "possibly full" often enough that even the cached version is
  frequently forced back to the real atomic; at 65,536 the producer can race
  far ahead of the consumer before its cache goes stale, so the fast path
  (touch only its own cache line) is taken almost every call. The naive
  version's throughput is essentially *flat* across all three capacities
  (~94–105 M/s) because it pays the cross-core read unconditionally every
  single call regardless of how much slack actually exists — capacity can't
  help it.
- The gap *shrinks* (to +10%) once the consumer does realistic per-message
  work (a stand-in for an order-book touch or a risk check). This makes
  sense: once the consumer, not the queue, is the bottleneck, both
  implementations spend a smaller fraction of wall-clock time on queue
  bookkeeping, so the difference between them matters less — but it never
  flips sign. The queue is never the wrong place to have made this
  optimization, only sometimes a smaller slice of the total cost.
- An earlier run without explicit core-pinning (threads left for the OS
  scheduler to place, `taskset` applied to the whole process rather than
  per-thread) showed the naive version *winning*. That result was an
  artifact of measurement, not a real effect — almost certainly threads
  landing on hyperthread siblings or migrating mid-run, which pollutes
  exactly the cross-core cache-line traffic this benchmark exists to
  measure. It's recorded here because "the first measurement was wrong, and
  here's what fixed it" is a more honest engineering log than only showing
  the clean run.

**Why not skip the cache and always re-read (simpler code, one less branch)?**
Because the two things being traded are a predictable branch (cheap,
correctly predicted almost always once the pattern is decided) against an
atomic load that may cross to another core's cache (tens of cycles even in
the best case, and a full cache-coherency round trip — hundreds of cycles —
when the line is not already shared). At any capacity large enough to let a
producer and consumer run at different instantaneous speeds even briefly
(i.e. any capacity that isn't pathologically small), the branch is
essentially free next to the cost it's avoiding.

**Why not eliminate false sharing by padding every queue slot to 64 bytes
instead of packing `Msg` tightly in the array?** Not attempted, and
deliberately: this queue is a FIFO stream, not a randomly-accessed
structure — the producer and consumer are almost never touching the *same*
slot at the *same* instant except in the narrow window right at the
head/tail boundary when the consumer has nearly caught up. Padding every
slot to a cache line would cut how many messages fit in L1/L2 by up to 8x
for a 32-byte message, which trades a rare, narrow-window contention case
for a guaranteed, permanent increase in cache pressure on the common case.
This is a hypothesis, not (yet) a measurement — a slot-padding variant is a
candidate for a follow-up benchmark if profiling ever points at that
boundary specifically.

## Concurrency: N-producer order entry — SPSC fan-in vs. CAS-based MPSC

**Decision:** the primary order-entry path (`jane::FanInSequencer`) gives
each client/gateway thread its own private `SPSCRingBuffer`, round-robin
drained by a single sequencer thread — not a single shared buffer that all
producers push into concurrently (`jane::MPSCRingBuffer`, a from-scratch
implementation of Vyukov's bounded MPMC queue specialized to one consumer).
Both are implemented and tested; only the fan-in design is used downstream.

**Data** (`artifacts/bench_mpsc.json`, producers pinned to physical cores
0..N-1, consumer pinned to core 11, 32-byte messages, 2^18 messages/producer,
`Iterations=10`):

| Producers | CAS-shared MPSC (M items/s) | SPSC fan-in (M items/s) | Fan-in advantage |
|---:|---:|---:|---:|
| 2 | 15.9 |  18.7 | +18% |
| 4 | 11.6 |  19.2 | +65% |
| 8 |  6.8 |  22.5 | +230% |

**Reading the data:** this is the cleanest result in the whole benchmark
suite because it has an obvious mechanism behind it. Every producer in the
CAS design contends on the *same* cache line (`enqueue_pos_`) for every
single message — more producers means more compare-exchange retries, so
throughput *degrades* as producers are added (15.9 → 6.8 M/s, 2→8
producers). The fan-in design gives each producer a private queue it never
shares with another producer, so producer-side contention is structurally
zero regardless of producer count; its only shared resource is the
consumer's round-robin scan, which is read-only from the producers'
perspective. Throughput doesn't just win, it trends the *opposite direction*
as load increases (18.7 → 22.5 M/s, 2→8 producers) — more queues to
round-robin costs the consumer a few more empty-queue checks per drained
message, which is far cheaper than the CAS retries it's avoiding.

**Why implement the CAS version at all, then, if it's not used?** Two
reasons. First, this is a "measure, don't assert" project — claiming the
simpler design is faster without the harder one to compare against would be
an opinion, not a result. Second, the fan-in design isn't free of
trade-offs itself: it requires the caller to know the producer count up
front (a template parameter) and to route each client to a specific,
fixed queue index, whereas the CAS-based queue accepts pushes from an
unbounded, unknown set of threads with no registration step. For this
project — a fixed set of client-gateway threads spun up at startup — that's
not a real constraint, which is exactly why fan-in is the better fit *here*.
A system that needs to accept order flow from an arbitrary, dynamically
changing set of connections (say, a naive thread-per-connection server
that spawns and destroys threads per client session) would find the CAS
queue's lack of a registration step genuinely useful despite its worse
throughput under contention — that's a real scenario where the measured
"loser" here would be the right call.

## Memory: hand-rolled slab pool vs. std::pmr

**Decision:** `jane::SlabPool` (two parallel fixed-size arrays: raw aligned
storage, and an index-based intrusive freelist) is the pool actually used
on the hot path. `jane::PmrPool` (`std::pmr::unsynchronized_pool_resource`
over a `std::pmr::monotonic_buffer_resource`) exists as a standard-library
comparison baseline, per the suggested stack. Both are fixed-capacity —
`allocate()` never calls into the OS or global allocator after
construction, and both signal exhaustion instead of silently growing
(`nullptr` for SlabPool, `bad_alloc` for PmrPool — see below for why that
difference is more consequential than it looks).

**Data** (`artifacts/bench_memory_pool.json`, single-threaded, ~48-byte
object matching `Order`'s shape):

| Pattern | SlabPool | PmrPool | `new`/`delete` |
|---|---:|---:|---:|
| Immediate cycle (alloc, dealloc same slot) | 1.08 ns | 14.6 ns | 6.80 ns |
| Churn (batch alloc, interleaved free, refill) | 366.7 M items/s | 41.9 M items/s | 37.4 M items/s |

**Reading the data:** SlabPool wins both patterns by a wide margin (6–13x),
which is the expected outcome, not a surprise — it does strictly less work
than either alternative: no virtual dispatch (`PmrPool` goes through
`memory_resource`'s virtual `do_allocate`/`do_deallocate` on every call),
no size-class/bucket lookup, just an index read and a placement-new. The
genuine surprise is that **`PmrPool` is slower than raw global `new`/`delete`**
in the immediate-cycle pattern (14.6 ns vs. 6.8 ns) despite `new`/`delete`
being the thing this whole exercise is nominally optimizing away from. The
likely explanation: glibc's allocator keeps a per-thread `tcache` — a small
per-size free list that makes "free this exact size, then immediately
`malloc` that exact size again" close to its best case, which is exactly
the immediate-cycle pattern. `PmrPool`'s extra indirection (virtual calls,
pool bucket lookup) can't beat that fast path for this specific access
pattern, even though it wins once the pattern is less LIFO-friendly
(churn: 41.9 vs. 37.4 M items/s). Neither pmr number is anywhere near
SlabPool's, but "does pmr even reliably beat the thing it's replacing" is
a fair question to ask before adopting it, and the honest answer here is
"only sometimes, and not by much" — the real win in this codebase comes
from the hand-rolled pool, not from moving off the global allocator per se.

**A correctness finding, not just a performance one:** getting `PmrPool` to
a fixed-capacity contract at all took two fixes past the obvious
implementation, both caught by `tests/test_memory_pool.cpp` rather than
anticipated up front:

1. `pool_options.max_blocks_per_chunk` bounds how many blocks a single
   upstream *replenishment* may request — it is not a total-capacity
   ceiling. `unsynchronized_pool_resource` will go back to its upstream for
   another chunk once a size class's free list runs dry again, so a
   generously-sized upstream buffer doesn't cap total allocations at the
   intended capacity; it just delays the point at which capacity stops
   being enforced.
2. Sizing the upstream buffer *tightly* to force the second chunk request
   to fail (so exhaustion is signaled by the upstream throwing) doesn't
   just misbehave — on this project's toolchain, **GCC 15.2 / libstdc++,
   `unsynchronized_pool_resource::do_allocate` segfaults on the call
   *after* its upstream throws `bad_alloc`**, reproduced in a ~30-line
   standalone program outside this codebase with no sanitizer required
   (`AddressSanitizer: SEGV ... in std::pmr::unsynchronized_pool_resource::do_allocate`,
   and the same crash with no sanitizer at all, exit code 139). Relying on
   "the upstream throws when exhausted" as the exhaustion signal is
   therefore not just imprecise, it's unsafe on this standard library
   version.

The fix sidesteps both issues the same way: `PmrPool` tracks `live_count_`
itself and refuses at exactly `capacity`, strictly *before* ever calling
into `pool_resource`. That makes it mathematically impossible for the
pool to ever ask its upstream for a second chunk, which means the libstdc++
crash path can never trigger and the declared capacity is exactly
enforced — both problems traced back to the same root cause (don't let
`pool_resource`'s own chunk-growth policy be the thing enforcing your
capacity; enforce it yourself and never give `pool_resource` the chance to
find out it's out of room).

## Price-level structures: bitmap ladder vs. std::set vs. sorted vector

**Decision:** `jane::book::OrderBook` uses the bitmap-scanned ladder
(`LadderSide`, see the order book section above) as its price index. This
section is the isolated, controlled comparison behind that choice — and it
overturned the initial assumption partway through, which is the most
important result here.

**Setup:** `include/jane/book/price_index_variants.hpp` implements the
*same* "track occupied prices, report the best" responsibility three ways
(bitmap/bit-scan, `std::set`, sorted `std::vector`) with everything else —
FIFO management, allocation — deliberately excluded, since those are
identical regardless of this choice and already measured in
`bench_order_book.cpp` / `bench_memory_pool.cpp`. Mixing them in would
leave it ambiguous which variable caused a given result.

**Data, scenario 1** (`artifacts/bench_order_book_backends.json`): book
built from `depth` *uniformly random* prices drawn from a 65,536-tick
range, then 5,000 steady-state ops of "erase the current best, insert a
new random price":

| Depth | Ladder (M ops/s) | `std::set` (M ops/s) | Sorted vector (M ops/s) |
|---:|---:|---:|---:|
| 20   |  8.8 | 41.9 | 92.4 |
| 2000 | 17.5 | 15.2 | 18.9 |

**Data, scenario 2** (`artifacts/bench_order_book.json`, from the order
book's own benchmark): book built from 2,048 *consecutive* price levels
(`0, 1, 2, ..., 2047`), same erase-best/insert churn pattern: **36.6M
ops/s** for the ladder — roughly double scenario 1's depth-2000 number,
on a comparable depth.

**Reading the data — the ladder is not universally fastest:** at depth 20
with scattered prices, the bitmap ladder is the *slowest* of the three by
a wide margin (10.5x slower than the sorted vector), which directly
contradicts what the order book's own benchmark (dense, consecutive
levels) seemed to show. Both numbers are real; they're measuring different
occupancy *patterns*, and the ladder's cost is driven by pattern, not just
count:

- `find_highest_at_or_below` after evicting the best price walks 64-bit
  words until it hits one with a set bit. With 20 prices scattered
  uniformly across 65,536 slots, the average gap between two occupied
  slots is ~3,277 — about 51 empty 64-bit words to skip, on average,
  *every single time the best price changes*. A sorted vector's `back()`
  is one memory read regardless of how sparse the occupied set is, and
  shifting 20 `int64_t` elements on insert/erase is fast enough (a handful
  of nanoseconds) to not matter at this scale — this is the classic
  regime where a flat, cache-friendly structure with worse Big-O beats a
  cleverer one with better Big-O.
- At depth 2,000 (still uniformly random), the average gap shrinks to
  ~33 slots — about one word — so the bit-scan is cheap again, and the
  ladder pulls back ahead of `std::set` (whose pointer-chasing tree
  traversal is cache-unfriendly at any depth) while landing close to the
  sorted vector.
- At depth 2,048 *consecutive* levels (scenario 2), essentially every word
  the scan touches has a set bit, so `find_highest_at_or_below` almost
  always resolves within the same or adjacent word — the best case for
  this data structure, and why that number is the highest of the three
  scenarios.

**What this means for the choice actually made:** real order books are
not uniformly random across the full representable price range — resting
orders cluster near the current touch (traders quote at or close to the
best price, not scattered uniformly across a multi-percent band), which
is structurally closer to scenario 2 (dense/local) than scenario 1
(sparse/global). That clustering assumption is *why* the ladder is the
right default here, not a universal "bitmaps are faster" claim — and
scenario 1 exists specifically to show the assumption has a real cost
when it doesn't hold, rather than asserting the clustering benefit
without a counter-example to weigh it against. A venue expecting mostly
resting liquidity scattered across a wide band with a thin book (closer
to scenario 1's depth-20 row) would have a genuine case for the sorted
vector instead. Neither this codebase nor this document tests real
market-data clustering directly (e.g. a Gaussian/exponential distribution
of resting prices around a moving touch) — that's a natural follow-up
benchmark, called out here rather than silently assumed away.

## Matching engine: caller-provided fill buffer vs. returning a fresh vector

**Decision:** `MatchingEngine::submit()`/`replace()` append `Fill` events to
a `std::vector<Fill>&` the caller passes in, rather than returning a
`std::vector<Fill>` by value. The first implementation did the latter —
this section exists because benchmarking the matching engine (not writing
it) is what surfaced the allocation, at the one place in this codebase
where "no allocation on the hot path" was quietly not true despite being
true everywhere else (ring buffers, pools, the order book itself).

**Data** (`artifacts/bench_matching_engine.json`, single core pinned,
`Iterations` auto-tuned, `cv` ≤ 0.9% across 3 repetitions — see the file
for the repeated-run stddev):

| Scenario | Reused `out_fills` | Fresh vector per call | Difference |
|---|---:|---:|---:|
| 1 fill per call  | 526 ns | 526 ns  | ~0 (noise-level) |
| 20 fills per call | 1,520 ns | 1,583 ns | +63 ns (+4.1%) |

**Reading the data:** the improvement is real and in the expected
direction (grows with fill count, since a fresh vector pays repeated
reallocate-and-copy steps as it grows past its small initial capacity),
but it's far more modest than the 6–13x differences found for
`SlabPool` vs. `new`/`delete` in the memory-pool comparison. The reason
is `Fill` itself: a small (~40-byte), trivially-copyable struct, so both
the allocation and the copy-on-reallocation are cheap on this glibc
(consistent with the memory-pool section's finding that small allocations
hit a fast tcache path) — there just isn't as much to save here as there
was for a type close enough to `Order`'s actual size to benefit from
`SlabPool`'s zero-indirection allocate/deallocate. The fix was still worth
making: it's a real, measured, if modest win, it costs nothing (the API
is not meaningfully more complex — an out-parameter instead of a return
value), and it's consistent with a codebase that otherwise avoids
allocation on this exact path everywhere else. It's a good example of a
case where "measure before optimizing" cuts the other way too: the
instinct to fix it was right, but claiming a bigger win than 4% would
have been the kind of unearned claim this whole project tries to avoid.
