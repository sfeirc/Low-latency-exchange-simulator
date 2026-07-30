#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "jane/concurrency/fan_in_sequencer.hpp"
#include "jane/concurrency/mpsc_ring_buffer.hpp"

using jane::FanInSequencer;
using jane::MPSCRingBuffer;

namespace {

// {producer, local sequence within that producer} — lets a multi-producer
// test verify per-producer FIFO order and detect loss/duplication, which
// a bare counter can't distinguish from "arrived out of order".
struct TaggedMsg {
    std::uint32_t producer;
    std::uint64_t local_seq;
};
static_assert(std::is_trivially_copyable_v<TaggedMsg>);

}  // namespace

// ---------------------------------------------------------------------
// MPSCRingBuffer
// ---------------------------------------------------------------------

TEST_CASE("MPSC: single-threaded push/pop preserves order", "[mpsc]") {
    MPSCRingBuffer<int, 8> q;
    for (int i = 0; i < 5; ++i) {
        REQUIRE(q.try_push(i));
    }
    for (int i = 0; i < 5; ++i) {
        int out = -1;
        REQUIRE(q.try_pop(out));
        REQUIRE(out == i);
    }
    int out = 0;
    REQUIRE_FALSE(q.try_pop(out));
}

TEST_CASE("MPSC: full at exactly Capacity, frees on pop", "[mpsc]") {
    MPSCRingBuffer<int, 4> q;
    REQUIRE(q.try_push(1));
    REQUIRE(q.try_push(2));
    REQUIRE(q.try_push(3));
    REQUIRE(q.try_push(4));
    REQUIRE_FALSE(q.try_push(5));

    int out = 0;
    REQUIRE(q.try_pop(out));
    REQUIRE(out == 1);
    REQUIRE(q.try_push(5));
}

TEST_CASE("MPSC: index wraparound preserves order across many cycles", "[mpsc]") {
    constexpr std::uint64_t kCapacity = 16;
    constexpr std::uint64_t kTotal = 100'000;
    MPSCRingBuffer<std::uint64_t, kCapacity> q;

    std::uint64_t next_push = 0;
    std::uint64_t next_expected = 0;
    while (next_expected < kTotal) {
        if (next_push < kTotal && q.try_push(next_push)) {
            ++next_push;
        }
        std::uint64_t out = 0;
        if (q.try_pop(out)) {
            REQUIRE(out == next_expected);
            ++next_expected;
        }
    }
    REQUIRE(next_push == kTotal);
}

TEST_CASE("MPSC: concurrent producers preserve per-producer order, no loss/dup", "[mpsc][stress]") {
    constexpr std::uint32_t kNumProducers = 6;
    constexpr std::uint64_t kPerProducer = 200'000;
    constexpr std::uint64_t kTotal = kNumProducers * kPerProducer;

    auto q = std::make_unique<MPSCRingBuffer<TaggedMsg, 1024>>();
    std::vector<std::thread> producers;
    producers.reserve(kNumProducers);
    for (std::uint32_t p = 0; p < kNumProducers; ++p) {
        producers.emplace_back([&q, p] {
            for (std::uint64_t i = 0; i < kPerProducer; ++i) {
                const TaggedMsg m{p, i};
                while (!q->try_push(m)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::vector<std::uint64_t> next_expected_per_producer(kNumProducers, 0);
    std::uint64_t received = 0;
    TaggedMsg out{};
    while (received < kTotal) {
        if (q->try_pop(out)) {
            REQUIRE(out.producer < kNumProducers);
            REQUIRE(out.local_seq == next_expected_per_producer[out.producer]);
            ++next_expected_per_producer[out.producer];
            ++received;
        }
    }

    for (auto& t : producers) t.join();

    REQUIRE(received == kTotal);
    for (std::uint32_t p = 0; p < kNumProducers; ++p) {
        REQUIRE(next_expected_per_producer[p] == kPerProducer);
    }
}

// ---------------------------------------------------------------------
// FanInSequencer
// ---------------------------------------------------------------------

TEST_CASE("FanIn: single producer round-trips in order", "[fanin]") {
    FanInSequencer<int, 3, 8> seq;
    REQUIRE(seq.push_from(1, 10));
    REQUIRE(seq.push_from(1, 20));
    REQUIRE(seq.push_from(1, 30));

    int out = 0;
    std::size_t src = 99;
    REQUIRE(seq.try_drain_one(out, src));
    REQUIRE(out == 10);
    REQUIRE(src == 1);
    REQUIRE(seq.try_drain_one(out, src));
    REQUIRE(out == 20);
    REQUIRE(seq.try_drain_one(out, src));
    REQUIRE(out == 30);
    REQUIRE_FALSE(seq.try_drain_one(out, src));
}

TEST_CASE("FanIn: round-robins fairly across simultaneously-ready queues", "[fanin]") {
    FanInSequencer<int, 4, 8> seq;
    // producer 2 has data waiting before any drain happens; everyone else empty
    REQUIRE(seq.push_from(0, 100));
    REQUIRE(seq.push_from(1, 200));
    REQUIRE(seq.push_from(2, 300));
    REQUIRE(seq.push_from(3, 400));

    std::vector<int> drained;
    std::vector<std::size_t> sources;
    int out = 0;
    std::size_t src = 0;
    for (int i = 0; i < 4; ++i) {
        REQUIRE(seq.try_drain_one(out, src));
        drained.push_back(out);
        sources.push_back(src);
    }
    // every producer's single message must show up exactly once
    std::vector<std::size_t> sorted_sources = sources;
    std::sort(sorted_sources.begin(), sorted_sources.end());
    REQUIRE(sorted_sources == std::vector<std::size_t>{0, 1, 2, 3});
}

TEST_CASE("FanIn: concurrent producers preserve per-producer order, no loss/dup", "[fanin][stress]") {
    constexpr std::size_t kNumProducers = 6;
    constexpr std::uint64_t kPerProducer = 200'000;
    constexpr std::uint64_t kTotal = kNumProducers * kPerProducer;

    auto seq = std::make_unique<FanInSequencer<TaggedMsg, kNumProducers, 1024>>();
    std::vector<std::thread> producers;
    producers.reserve(kNumProducers);
    for (std::size_t p = 0; p < kNumProducers; ++p) {
        producers.emplace_back([&seq, p] {
            for (std::uint64_t i = 0; i < kPerProducer; ++i) {
                const TaggedMsg m{static_cast<std::uint32_t>(p), i};
                while (!seq->push_from(p, m)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::vector<std::uint64_t> next_expected_per_producer(kNumProducers, 0);
    std::uint64_t received = 0;
    TaggedMsg out{};
    std::size_t src = 0;
    while (received < kTotal) {
        if (seq->try_drain_one(out, src)) {
            REQUIRE(src < kNumProducers);
            REQUIRE(out.producer == src);
            REQUIRE(out.local_seq == next_expected_per_producer[src]);
            ++next_expected_per_producer[src];
            ++received;
        }
    }

    for (auto& t : producers) t.join();

    REQUIRE(received == kTotal);
    for (std::size_t p = 0; p < kNumProducers; ++p) {
        REQUIRE(next_expected_per_producer[p] == kPerProducer);
    }
}
