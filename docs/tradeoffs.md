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

*(more sections land here as the corresponding components do: price-level
structures, allocation strategy, MPSC fan-in vs. CAS.)*
