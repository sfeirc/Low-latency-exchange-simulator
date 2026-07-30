#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "jane/concurrency/spsc_ring_buffer.hpp"

using jane::SPSCRingBuffer;

TEST_CASE("push/pop preserves FIFO order single-threaded", "[spsc]") {
    SPSCRingBuffer<int, 8> q;
    for (int i = 0; i < 5; ++i) {
        REQUIRE(q.try_push(i));
    }
    for (int i = 0; i < 5; ++i) {
        int out = -1;
        REQUIRE(q.try_pop(out));
        REQUIRE(out == i);
    }
    REQUIRE(q.empty_approx());
}

TEST_CASE("try_pop on empty buffer fails without touching the output", "[spsc]") {
    SPSCRingBuffer<int, 4> q;
    int out = 123;
    REQUIRE_FALSE(q.try_pop(out));
    REQUIRE(out == 123);
}

TEST_CASE("try_push reports full at exactly Capacity elements", "[spsc]") {
    SPSCRingBuffer<int, 4> q;
    REQUIRE(q.try_push(1));
    REQUIRE(q.try_push(2));
    REQUIRE(q.try_push(3));
    REQUIRE(q.try_push(4));
    REQUIRE(q.full_approx());
    REQUIRE_FALSE(q.try_push(5));  // one more than capacity must fail

    int out = 0;
    REQUIRE(q.try_pop(out));
    REQUIRE(out == 1);
    REQUIRE(q.try_push(5));  // freed one slot
}

TEST_CASE("index wraparound preserves order across many cycles", "[spsc]") {
    constexpr std::uint64_t kCapacity = 16;
    constexpr std::uint64_t kTotal = 100'000;  // >> capacity: forces many wraps
    SPSCRingBuffer<std::uint64_t, kCapacity> q;

    std::uint64_t next_push = 0;
    std::uint64_t next_expected_pop = 0;

    while (next_expected_pop < kTotal) {
        if (next_push < kTotal && q.try_push(next_push)) {
            ++next_push;
        }
        std::uint64_t out = 0;
        if (q.try_pop(out)) {
            REQUIRE(out == next_expected_pop);
            ++next_expected_pop;
        }
    }
    REQUIRE(next_push == kTotal);
    REQUIRE(q.empty_approx());
}

TEST_CASE("minimum valid capacity of 2 behaves correctly", "[spsc]") {
    SPSCRingBuffer<int, 2> q;
    REQUIRE(q.try_push(10));
    REQUIRE(q.try_push(20));
    REQUIRE_FALSE(q.try_push(30));
    int out = 0;
    REQUIRE(q.try_pop(out));
    REQUIRE(out == 10);
    REQUIRE(q.try_pop(out));
    REQUIRE(out == 20);
    REQUIRE_FALSE(q.try_pop(out));
}

TEST_CASE("optional-returning try_pop mirrors the out-parameter overload", "[spsc]") {
    SPSCRingBuffer<int, 4> q;
    REQUIRE_FALSE(q.try_pop().has_value());
    REQUIRE(q.try_push(42));
    auto v = q.try_pop();
    REQUIRE(v.has_value());
    REQUIRE(*v == 42);
}

TEST_CASE("works for a multi-field POD, not just a single integer", "[spsc]") {
    struct Msg {
        std::uint64_t seq;
        double price;
        char tag[3];
    };
    static_assert(std::is_trivially_copyable_v<Msg>);

    SPSCRingBuffer<Msg, 4> q;
    REQUIRE(q.try_push(Msg{1, 100.25, {'B', 'U', 'Y'}}));
    Msg out{};
    REQUIRE(q.try_pop(out));
    REQUIRE(out.seq == 1);
    REQUIRE(out.price == 100.25);
    REQUIRE(out.tag[0] == 'B');
}

// --- concurrent stress test: real producer/consumer threads -----------
//
// Each pushed element carries its sequence number plus a checksum
// derived from it. The consumer verifies both that sequence numbers
// arrive strictly in order with no gaps (catches reordering / lost /
// duplicated elements) and that the checksum matches (catches torn
// writes / partial-slot corruption). Run under ThreadSanitizer in CI to
// additionally catch any data race the memory-ordering reasoning missed.
TEST_CASE("concurrent producer/consumer preserves order and integrity", "[spsc][stress]") {
    struct Msg {
        std::uint64_t seq;
        std::uint64_t checksum;
    };
    static_assert(std::is_trivially_copyable_v<Msg>);

    constexpr std::uint64_t kTotal = 2'000'000;
    auto q = std::make_unique<SPSCRingBuffer<Msg, 1024>>();

    std::atomic<bool> producer_done{false};
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kTotal; ++i) {
            const Msg m{i, i * 2654435761ULL};  // Knuth multiplicative hash as a cheap checksum
            while (!q->try_push(m)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::uint64_t expected_seq = 0;
    Msg out{};
    while (expected_seq < kTotal) {
        if (q->try_pop(out)) {
            REQUIRE(out.seq == expected_seq);
            REQUIRE(out.checksum == expected_seq * 2654435761ULL);
            ++expected_seq;
        }
    }

    producer.join();
    REQUIRE(producer_done.load(std::memory_order_acquire));
    REQUIRE(expected_seq == kTotal);
    REQUIRE(q->empty_approx());
}
