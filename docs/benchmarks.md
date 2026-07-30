# Benchmarks

Every number below came from `tools/run_benchmarks.sh`, which builds in
Release (`-O3 -march=native`, LTO, no sanitizers) and runs each
`bench_*` binary pinned to one isolated physical core. Raw output is in
`artifacts/*.json`; `python3 tools/analyze_benchmarks.py artifacts/*.json`
reproduces the tables below from those files directly — nothing here is
hand-typed from a terminal that's since scrolled away.

**Machine:** AMD Ryzen 9 5900X (12C/24T), 30 GiB RAM, Ubuntu, GCC 15.2.
**Caveat:** this is a shared, non-isolated development machine (a
persistent unrelated process holds a CPU core at 100% throughout —
visible as the "Library was built as DEBUG" warning is *not* this, that
one's benign, see below). Core-pinning controls for this but doesn't
eliminate it; treat absolute numbers as "what this code does on this
box today," and relative comparisons (the actual content of
docs/tradeoffs.md) as the more portable takeaway.

**The "Library was built as DEBUG" warning every run prints** is Google
Benchmark's own build-config check on the *installed system
`libbenchmark`* (the apt package), not this project's code —
confirmed once in task 3 by inspecting the actual compile flags
(`-O3 -DNDEBUG -march=native`) and is otherwise irrelevant here since it
affects every benchmark in this suite equally.

## Two methodologies, used for different things

- **Google Benchmark's own aggregate timing** (`bench_spsc_ring_buffer.cpp`,
  `bench_memory_pool.cpp`, `bench_order_book.cpp`, `bench_matching_engine.cpp`,
  `bench_order_book_backends.cpp`, `bench_protocol.cpp`): times a tight loop
  of millions of iterations as a whole and reports the mean per iteration.
  Correct and efficient for isolating one fast, low-variance operation's
  typical cost — measurement overhead is amortized across the whole loop,
  not paid per call.
- **Individual per-call histograms** (`bench_end_to_end.cpp`, built on
  `jane::metrics::LatencyHistogram`): times *each* call with its own clock
  reading and records it, at a scale (100k–500k samples) large enough for
  p99.9 to be a real statistic. This is strictly necessary to report a
  *distribution* rather than a mean, but pays per-call clock overhead —
  quantified directly by `bench_measurement_overhead` below rather than
  ignored.

Component-level operations (ring buffer push/pop, pool alloc/dealloc, a
bare order-book add) are fast and low-variance enough that the GBench
mean is the more trustworthy number for them; the full pipeline is where
realistic tail variance (hash lookups, sweep length, occasional cache
misses) actually shows up, which is why that's where the histogram
methodology is used.

## Headline: full pipeline, per order

`bench_end_to_end_pipeline_percentiles` — sequencer clock → pre-trade
risk → matching → market-data publication, one `ReplayEngine::
process_new_order` call per sample, 500,000 samples of realistic
synthetic order flow (`SyntheticOrderGenerator`, seeded, ~5% market
orders, prices concentrated enough to cross frequently):

| Percentile | Latency |
|---|---:|
| Measurement floor (`bench_measurement_overhead`, p50) | 20 ns |
| p50 | 110 ns |
| p99 | 1,690 ns (1.69 µs) |
| p99.9 | 4,060 ns (4.06 µs) |
| max (500,000 samples) | 179,404 ns (179 µs) |
| mean | 210 ns |

**Reading it:** p50 (110ns) is close to the sum of the component parts
below — a non-crossing or lightly-crossing order barely touches more
than the book and one risk check. The gap to p99 (1.69µs, ~15x p50) and
p99.9 (4.06µs, ~37x p50) is the realistic cost of the *tail* — sweeps
across more resting orders, hash map probes that miss cache, the
occasional level requiring a bitmap relocation — not a red flag. The max
(179µs) is a single sample out of 500,000 (0.0002%), four orders of
magnitude past p99.9; see the "what this replaced" note below for why
trusting a bare max over percentiles here would have been actively
misleading.

## Component-level latency (Google Benchmark aggregate mean)

| Operation | Mean | Source |
|---|---:|---|
| Order book add+cancel round trip | 25 ns | `bench_order_book.cpp` |
| Order book best_bid query | 6.7 ns | `bench_order_book.cpp` |
| Matching engine: non-crossing submit + cancel | 40 ns | `bench_matching_engine.cpp` |
| Matching engine: crossing submit (1 fill) + re-rest | 288–530 ns† | `bench_matching_engine.cpp` |
| Matching engine: sweep 10 resting orders | ~1.1–1.3 µs | `bench_matching_engine.cpp` |
| Protocol encode (NewOrder) | 0.87 ns | `bench_protocol.cpp` |
| Protocol decode (NewOrder) | 7.3 ns | `bench_protocol.cpp` |
| SPSC ring buffer, single-threaded round trip | 1.4 ns | `bench_spsc_ring_buffer.cpp` |
| SlabPool alloc+dealloc (immediate cycle) | 1.1 ns | `bench_memory_pool.cpp` |

† Range reflects the caller-provided-`out_fills`-vector change from task 9
(see docs/tradeoffs.md's matching-engine section) — both numbers measured,
neither cherry-picked.

Throughput and multi-way comparisons (SPSC cached-vs-naive, MPSC
fan-in-vs-CAS, price-index ladder-vs-tree-vs-vector, SlabPool-vs-pmr-vs-
`new`/`delete`) are in docs/tradeoffs.md, not repeated here — this
document is "what is the number," tradeoffs.md is "why, and what it cost
to find out."

## A measurement artifact this suite found and fixed

The first run of `bench_end_to_end_pipeline_percentiles` reported a max
of **25.9 million ns (25.9ms)** — not the 179µs above. Root-caused with a
standalone diagnostic (not guesswork) before touching any code: nine
samples exceeded 100µs, at order indices 655, 2576, 5133, 10307, 20591,
41138, 82270, 164496, 328802 — each roughly double the previous index,
each roughly double the previous duration. That signature is
`std::vector`'s doubling-reallocation strategy, not the matching engine:
`MarketDataPublisher` writes every trade/delta/execution-report through
`InMemorySink`, whose backing buffer was never `reserve()`d, so it grew
by repeated full-buffer-copy reallocations as ~150MB of market data
accumulated over 500,000 orders — each doubling costing more than the
last, in wall-clock time attributed to whatever order happened to be
in flight when it fired.

Fixed two ways, both real: `InMemorySink::reserve()` was added (not
present before this was found) and the benchmark now calls it with a
generous upfront estimate. Before/after, same code otherwise:

| | max | p50 | p99 | p99.9 | wall time |
|---|---:|---:|---:|---:|---:|
| Before (unreserved sink) | 25.9 ms | 160 ns | 1.68 µs | 4.08 µs | ~196 ms |
| After (`sink.reserve(...)`) | 179 µs | 110 ns | 1.69 µs | 4.06 µs | ~131 ms |

p50/p99/p99.9 barely moved (confirming the reallocation cost was
isolated to a handful of samples, not smeared across the distribution),
while max dropped >100x and wall time dropped by a third. This is the
concrete version of "measure, don't assert" this whole project claims to
practice: the bug was in the *benchmark harness*, not the engine, and
the only way to know that — rather than either shipping a wrong 25.9ms
"worst case" or shipping a fix without proof it was the right one — was
to look at exactly which samples were slow and why.

## Reproducing this

```sh
tools/run_benchmarks.sh                              # builds Release, runs everything, writes artifacts/*.json
python3 tools/analyze_benchmarks.py artifacts/*.json  # human-readable summary
```

`TASKSET_CORE=N tools/run_benchmarks.sh` to pin to a different physical
core — check `lscpu -e` first if running on hardware where core 5 might
be a hyperthread sibling of something else being used concurrently (see
docs/tradeoffs.md's SPSC section for why that distinction changed a
result once already).
