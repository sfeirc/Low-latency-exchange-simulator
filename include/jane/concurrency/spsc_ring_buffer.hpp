#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

#include "jane/concurrency/cache_line.hpp"

namespace jane {

// A wait-free single-producer/single-consumer ring buffer.
//
// Design (this is the well-known "Rigtorp" SPSC layout, not a novel
// scheme — see docs/tradeoffs.md for the reasoning and a citation):
//
//  - Capacity is a compile-time power of two, so slot indexing is
//    `index & (Capacity - 1)` instead of a modulo.
//  - The producer's write index (`head`) and the consumer's read index
//    (`tail`) each live in their own cache-line-aligned struct, so the
//    two threads never write the same cache line. `alignas(64)` on each
//    struct also forces the compiler to pad its *size* up to 64 bytes
//    (an object's size must be a multiple of its alignment), so there is
//    no shared line at the boundary either.
//  - Each side additionally keeps a plain (non-atomic) *cached* copy of
//    the other side's index: the producer caches `tail`, the consumer
//    caches `head`. The fast path only re-reads the other side's atomic
//    index when the cached value says the buffer might be full/empty —
//    the common case (queue neither full nor empty) touches only the
//    calling thread's own cache line. A naive implementation that loads
//    the opposite atomic on every call roughly doubles cross-core
//    coherency traffic under sustained throughput (measured in
//    bench/bench_spsc_ring_buffer.cpp).
//
// Correctness sketch: the producer writes buffer_[slot] and only *then*
// releases the updated `head`; the consumer's acquire-load of `head`
// therefore also observes that buffer_ write (release/acquire pairing on
// `head`). Symmetrically, the consumer reads buffer_[slot] and only then
// releases `tail`, so the producer never reuses a slot the consumer
// hasn't finished reading yet (release/acquire pairing on `tail`).
//
// T must be trivially copyable — slots are plain-assigned, there is no
// per-slot construct/destroy machinery. This holds for every message
// type in jane::protocol and for jane::Order.
template <typename T, std::size_t Capacity>
class SPSCRingBuffer {
    static_assert(Capacity > 1 && std::has_single_bit(Capacity),
                  "Capacity must be a power of two greater than 1");
    static_assert(std::is_trivially_copyable_v<T>,
                  "SPSCRingBuffer stores T by value via plain assignment");

public:
    using value_type = T;

    SPSCRingBuffer() noexcept = default;
    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer(SPSCRingBuffer&&) = delete;
    SPSCRingBuffer& operator=(SPSCRingBuffer&&) = delete;

    // Producer-thread only. Returns false if the buffer is full.
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const std::uint64_t head = producer_.head.load(std::memory_order_relaxed);
        if (head - producer_.tail_cache >= Capacity) {
            // Cached view says full; refresh from the consumer's real
            // progress before giving up.
            producer_.tail_cache = consumer_.tail.load(std::memory_order_acquire);
            if (head - producer_.tail_cache >= Capacity) {
                return false;
            }
        }
        buffer_[static_cast<std::size_t>(head & kMask)] = item;
        producer_.head.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer-thread only. Returns false if the buffer is empty.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::uint64_t tail = consumer_.tail.load(std::memory_order_relaxed);
        if (tail == consumer_.head_cache) {
            consumer_.head_cache = producer_.head.load(std::memory_order_acquire);
            if (tail == consumer_.head_cache) {
                return false;
            }
        }
        out = buffer_[static_cast<std::size_t>(tail & kMask)];
        consumer_.tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Convenience wrapper around try_pop(T&) for call sites that prefer
    // an optional to an out-parameter. Requires T to be default
    // constructible (every message type in this codebase is).
    [[nodiscard]] std::optional<T> try_pop() noexcept {
        std::optional<T> out{std::in_place};
        if (try_pop(*out)) {
            return out;
        }
        return std::nullopt;
    }

    // Racy with respect to the other thread's concurrent progress — usable
    // for metrics/monitoring, not as a precondition for push/pop.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::uint64_t head = producer_.head.load(std::memory_order_acquire);
        const std::uint64_t tail = consumer_.tail.load(std::memory_order_acquire);
        return static_cast<std::size_t>(head - tail);
    }
    [[nodiscard]] bool empty_approx() const noexcept { return size_approx() == 0; }
    [[nodiscard]] bool full_approx() const noexcept { return size_approx() >= Capacity; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    static constexpr std::uint64_t kMask = Capacity - 1;

    struct alignas(kCacheLineSize) ProducerState {
        std::atomic<std::uint64_t> head{0};
        std::uint64_t tail_cache{0};
    };
    struct alignas(kCacheLineSize) ConsumerState {
        std::atomic<std::uint64_t> tail{0};
        std::uint64_t head_cache{0};
    };

    alignas(kCacheLineSize) std::array<T, Capacity> buffer_{};
    ProducerState producer_{};
    ConsumerState consumer_{};
};

}  // namespace jane
