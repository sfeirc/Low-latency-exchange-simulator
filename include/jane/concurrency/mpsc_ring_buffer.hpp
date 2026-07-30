#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "jane/concurrency/cache_line.hpp"

namespace jane {

// A lock-free multiple-producer/single-consumer bounded ring buffer.
//
// This is Dmitry Vyukov's bounded MPMC queue design, specialized to a
// single consumer (which lets try_pop() be a plain load/store — no CAS
// needed on the dequeue side, since only one thread ever touches it).
//
// Each cell carries its own `sequence` counter alongside its data instead
// of relying solely on the shared enqueue/dequeue counters:
//   - A cell starts at `sequence == index` (lap 0).
//   - A producer may claim logical position `pos` for cell `pos & mask`
//     only when `cell.sequence == pos`; after writing the data it stores
//     `cell.sequence = pos + 1`, publishing it to the consumer.
//   - The consumer may take logical position `pos` only when
//     `cell.sequence == pos + 1`; after reading it stores
//     `cell.sequence = pos + Capacity`, marking the cell ready for the
//     *next lap's* producer.
// This per-cell bookkeeping is what lets multiple producers race safely:
// a producer's compare_exchange on the shared `enqueue_pos_` only
// arbitrates *which logical position* it gets, while the cell's own
// sequence number is what actually gates safe reuse — so a producer that
// claims position N+Capacity can never overwrite position N's data before
// the consumer has read it, no matter how the producers interleave.
//
// See jane::FanInSequencer for the alternative used as this project's
// primary order-entry path (N independent SPSC queues, one per client,
// fanned into the sequencer thread) and docs/tradeoffs.md for measured
// throughput/latency/complexity trade-offs between the two.
template <typename T, std::size_t Capacity>
class MPSCRingBuffer {
    static_assert(Capacity > 1 && std::has_single_bit(Capacity),
                  "Capacity must be a power of two greater than 1");
    static_assert(std::is_trivially_copyable_v<T>,
                  "MPSCRingBuffer stores T by value via plain assignment");

    struct Cell {
        std::atomic<std::uint64_t> sequence;
        T data;
    };

public:
    using value_type = T;

    MPSCRingBuffer() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i) {
            cells_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }
    MPSCRingBuffer(const MPSCRingBuffer&) = delete;
    MPSCRingBuffer& operator=(const MPSCRingBuffer&) = delete;
    MPSCRingBuffer(MPSCRingBuffer&&) = delete;
    MPSCRingBuffer& operator=(MPSCRingBuffer&&) = delete;

    // Safe to call from any number of producer threads concurrently.
    // Preserves each individual producer's relative order (its own calls
    // are delivered in the order it made them) but makes no promise about
    // how different producers' messages interleave — see class comment.
    [[nodiscard]] bool try_push(const T& item) noexcept {
        std::uint64_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        Cell* cell;
        for (;;) {
            cell = &cells_[static_cast<std::size_t>(pos & kMask)];
            const std::uint64_t seq = cell->sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;  // claimed `pos`
                }
                // lost the race; compare_exchange_weak reloaded pos, retry
            } else if (diff < 0) {
                return false;  // full: consumer hasn't freed this slot's previous lap
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);  // stale pos, resample
            }
        }
        cell->data = item;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Consumer-thread only (exactly one thread may ever call this).
    [[nodiscard]] bool try_pop(T& out) noexcept {
        Cell& cell = cells_[static_cast<std::size_t>(dequeue_pos_ & kMask)];
        const std::uint64_t seq = cell.sequence.load(std::memory_order_acquire);
        const auto diff = static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(dequeue_pos_ + 1);
        if (diff != 0) {
            return false;  // not yet published by its producer
        }
        out = cell.data;
        cell.sequence.store(dequeue_pos_ + Capacity, std::memory_order_release);
        ++dequeue_pos_;
        return true;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    static constexpr std::uint64_t kMask = Capacity - 1;

    std::array<Cell, Capacity> cells_;
    alignas(kCacheLineSize) std::atomic<std::uint64_t> enqueue_pos_{0};
    alignas(kCacheLineSize) std::uint64_t dequeue_pos_{0};
};

}  // namespace jane
