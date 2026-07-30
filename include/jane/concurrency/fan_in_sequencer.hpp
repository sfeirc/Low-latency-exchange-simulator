#pragma once

#include <array>
#include <cstddef>

#include "jane/concurrency/spsc_ring_buffer.hpp"

namespace jane {

// N independent SPSC ring buffers — one per producer — fanned into a
// single consumer via round-robin polling.
//
// This is this project's primary multi-client order-entry path (see
// docs/architecture.md): each client/gateway thread owns exclusive write
// access to its own queue, which makes every one of the N queues a
// genuinely single-producer SPSCRingBuffer — reusing an already-proven
// primitive instead of a new lock-free algorithm. The only new concurrency
// concern this class introduces is *which order* the single sequencer
// thread drains the N queues in, which is deliberately just round-robin
// (see class comment on try_drain_one for why).
//
// Compare with jane::MPSCRingBuffer, which gets N producers down to one
// shared buffer via a fundamentally different (and trickier) lock-free
// algorithm; docs/tradeoffs.md has the measured throughput/latency
// comparison between the two.
template <typename T, std::size_t NumProducers, std::size_t QueueCapacity>
class FanInSequencer {
    static_assert(NumProducers > 0);

public:
    using InputQueue = SPSCRingBuffer<T, QueueCapacity>;

    // Only producer thread `producer_index` may ever call this for that
    // index — each queue must have exactly one writer.
    [[nodiscard]] bool push_from(std::size_t producer_index, const T& item) noexcept {
        return queues_[producer_index].try_push(item);
    }

    // Sequencer-thread only (exactly one thread may ever call this).
    //
    // Round-robins across all N input queues rather than draining one
    // queue fully before moving to the next, and rather than trying to
    // reconstruct "true" cross-client arrival order from client-supplied
    // timestamps. Round-robin is what this project treats as ground
    // truth for ordering across clients: waiting to see if an earlier-
    // timestamped message from a slower client is "about to arrive"
    // before sequencing a later-timestamped one already in hand would
    // mean either unbounded latency or a policy for how long to wait —
    // and a client-supplied timestamp is also not trustworthy (clock
    // skew, or a client that lies). Real exchanges sequence by arrival
    // at the matching engine for exactly this reason; "sequence number"
    // and "wall-clock send order" are not the same thing anywhere.
    //
    // Returns false only if every queue was empty on this sweep.
    [[nodiscard]] bool try_drain_one(T& out, std::size_t& source_producer) noexcept {
        for (std::size_t attempt = 0; attempt < NumProducers; ++attempt) {
            const std::size_t idx = next_queue_;
            next_queue_ = (next_queue_ + 1) % NumProducers;
            if (queues_[idx].try_pop(out)) {
                source_producer = idx;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] static constexpr std::size_t producer_count() noexcept { return NumProducers; }
    [[nodiscard]] static constexpr std::size_t queue_capacity() noexcept { return QueueCapacity; }

private:
    std::array<InputQueue, NumProducers> queues_{};
    std::size_t next_queue_ = 0;  // sequencer-thread-only state
};

}  // namespace jane
