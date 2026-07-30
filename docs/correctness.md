# Correctness

This project's stance: a benchmark number is worthless if the thing being
measured is wrong. Every property below is checked by an automated test —
this document says *where*, not just *that*, so a claim here can always be
traced back to the assertion that backs it.

## The properties

### 1. Conservation of volume

At every price level, the cached aggregate (`PriceLevel::total_quantity`,
`order_count`) must equal what the FIFO linked list actually contains —
walked and summed independently, not trusted. Checked structurally by
[`jane::correctness::check_book_invariants`](../include/jane/correctness/invariants.hpp)
after *every single action* (submit/cancel/replace) in an 8,000-iteration
randomized session
([`test_correctness_invariants.cpp`](../tests/test_correctness_invariants.cpp)),
plus a fully independent per-order ledger the test maintains itself and
cross-checks against the engine's actual state at every step — not just
"does the book look internally consistent" but "does an outside observer's
independent tally of what happened agree with it."

This exact check **caught a real bug**: `OrderBook::reduce_quantity` zeroed
`node->order.remaining` *before* calling `level_unlink` in the
full-fill branch. `level_unlink` subtracts `node->order.remaining` from
the level's aggregate — with it already zeroed, that subtraction became a
silent no-op, so a level's `total_quantity` permanently overcounted by the
fill amount every time an order on it was fully (not partially) filled.
No single-scenario unit test caught this — it only shows up when a level
that once held a now-fully-filled order later gets a *new* resting order
at the same price, which the 8,000-iteration random session hit at
iteration 55 of a specific seeded run but no hand-written test happened to
construct. Fixed by deciding "is this a full fill" and running
`level_unlink` *before* zeroing `remaining`, not after. This is the
single best argument in this codebase for property-based testing over
example-based testing alone: the bug was a real, reachable data
corruption, and it survived 100+ passing example-based test cases before
the fuzzer found it in under a hundred iterations.

### 2. Strict FIFO (price/time priority)

Within a price level, orders match in the order they arrived — no
reordering, ever, including under partial fills (a partial fill shrinks
`remaining` but never moves the order within the linked list).

- Structural: [`test_price_level.cpp`](../tests/test_price_level.cpp) checks
  the intrusive linked-list operations in isolation (push_back, unlink from
  head/tail/middle, an interleaved push/remove sequence against a
  hand-computed expected order).
- End-to-end: `test_matching_engine.cpp`'s
  *"consumes resting orders in strict FIFO order within a level"* submits
  three resting orders at the same price and asserts the fills come back
  in submission order, not any other order glibc's allocator or a hash
  map might have been tempted to produce.
- Explicitly verified to survive a partial fill: *"resting order partially
  filled keeps its FIFO position"* fills 40 of a 100-quantity resting
  order and confirms it's still the front of its level's FIFO, not pushed
  to the back.

### 3. No crossed / impossible trades

The book must never show `best_bid >= best_ask` — a crossed book means
there was executable liquidity the matching engine failed to execute.
Checked on every step of the same 8,000-iteration property test as
volume conservation.

The one place this was genuinely at risk of happening was
`MatchingEngine::replace()`: naively cancel-and-re-add (what
`OrderBook::replace()` alone does — it has no notion of crossing at all,
by design) would let a client move a resting bid *above* the current best
ask and just rest it there, crossed. `MatchingEngine::replace()` instead
re-submits the new terms through the exact same crossing-aware path a
brand new order takes, so a replace that becomes marketable executes
immediately instead. Verified directly by
*"a replace that becomes marketable executes immediately instead of
resting at a crossed price"* in `test_matching_engine.cpp`, which replaces
a bid through a resting ask and asserts `best_ask()` is empty (consumed by
a real trade) rather than checking the (wrong) alternative of "the book is
now crossed but that's what was asked for."

Also checked: a trade never prints at a worse price than either party
would accept. Every fill executes at the *resting* order's price — an
aggressor with a better limit than the touch gets price improvement, never
degradation (`"an aggressive limit order fills at the resting price
(price improvement)"`, `test_matching_engine.cpp`).

### 4. Fill-or-Kill is actually all-or-nothing

An FOK order must never partially fill — either the complete quantity
executes immediately or none of it does, book left byte-for-byte
untouched. `MatchingEngine::submit()` runs a read-only liquidity
pre-check (`for_each_level`, no mutation) before committing to any match;
since matching is single-threaded and the check uses the identical
crossing condition the real match loop uses, nothing can change between
the check and the execution. Verified both ways: enough liquidity
(`"FOK fills completely across multiple levels"`) and *not* enough
(`"FOK with insufficient liquidity fills NOTHING"` — asserts the
resting order that wasn't enough is completely undisturbed afterward, not
just that the aggressor got rejected), plus the exact boundary
(quantity == available liquidity, not more or less) and the
price-respecting case (liquidity exists but not at an acceptable price).

### 5. Bounded, not just "probably fine": pools never silently misbehave

Every allocator in this codebase (`SlabPool`, `PmrPool`, the order book's
internal order pool) reports exhaustion as a typed error
(`AddError::PoolExhausted`, `std::bad_alloc`) rather than corrupting state
or growing unboundedly. `SlabPool`'s tests include a 1,000-round randomized
allocate/deallocate/reallocate cycle asserting `live_count()` never
exceeds capacity. `PmrPool` additionally documents (docs/tradeoffs.md) a
real crash discovered in libstdc++ 15.2's `unsynchronized_pool_resource`
when its upstream throws — worked around by never letting that path
trigger, not by hoping it doesn't.

### 6. Lock-free structures are actually race-free, not just "looks right"

`SPSCRingBuffer` and `MPSCRingBuffer` are verified under ThreadSanitizer,
not just reasoned about — see docs/tradeoffs.md for the memory-ordering
argument, and `tests/test_spsc_ring_buffer.cpp` /
`tests/test_mpsc.cpp` for the concurrent stress tests (up to 6 producer
threads, 200k messages each, checked for per-producer ordering and
exact-once delivery) that TSan runs clean against.

## What isn't (yet) checked

Named directly rather than left implicit:

- **Determinism under replay** — the design (book/matching engine never
  touch a clock; the caller stamps sequence/timestamp) is in place, but
  the actual "replay the same input twice, diff the output" test lives
  with `jane::replay` (a later component), not here.
- **Self-trade prevention** — explicitly not implemented; a client's own
  resting order can be matched against by that same client's aggressive
  order. Tested and documented as a scope decision in
  `test_matching_engine.cpp`, not a silent gap.
- **Realistic market-data clustering** — the property test above draws
  prices from a uniform distribution over a narrow band (chosen to force
  frequent crossing, which is what stresses the matching logic); it does
  not model the price-clustering-near-the-touch pattern
  docs/tradeoffs.md's price-index section discusses. The invariants
  checked don't depend on the input distribution, so this doesn't weaken
  what's verified — it's a note on what workload shape produced the
  8,000-iteration run, not a gap in the checker itself.
