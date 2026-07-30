#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <new>

namespace jane {

// An object pool built entirely from <memory_resource>: a fixed-size
// buffer backs a std::pmr::monotonic_buffer_resource, which in turn backs
// a std::pmr::unsynchronized_pool_resource that does the actual
// size-class free-list management. Exists as a standard-library-idiomatic
// comparison point for jane::SlabPool — see docs/tradeoffs.md for which
// one this project actually uses on the hot path, and for the two pmr
// gotchas that shaped this implementation:
//
//  1. `pool_options.max_blocks_per_chunk` bounds how many blocks a single
//     upstream *replenishment* may request — it is not a total-capacity
//     ceiling. unsynchronized_pool_resource will happily go back to its
//     upstream for another chunk once a size class's free list runs dry
//     again, so a monotonic_buffer_resource sized generously enough to
//     always satisfy the first chunk will also often satisfy a second,
//     silently letting more than the "declared capacity" through.
//  2. Worse: when the upstream *does* refuse (throws bad_alloc) partway
//     through a replenishment, libstdc++ 15.2's
//     unsynchronized_pool_resource::do_allocate does not fail cleanly —
//     it segfaults on the *next* call, not just the failing one (confirmed
//     with a standalone repro outside this codebase, no sanitizer
//     required to reproduce it). Relying on "the upstream throws when
//     exhausted" as an exhaustion signal is therefore not just imprecise,
//     it's unsafe on this standard library version.
//
// The fix for both is the same: PmrPool tracks live_count_ itself and
// refuses (throws bad_alloc from *this* class) at exactly `capacity`,
// strictly before ever calling pool_.allocate() — so the underlying pool
// resource is mathematically guaranteed to never need more than one
// chunk's worth of blocks in the object's lifetime, and its upstream is
// never given the chance to run out. The monotonic buffer is still sized
// generously (comfortably more than one chunk needs); that no longer
// risks over-admitting objects, because the count check above happens
// first and independently of how the upstream is sized.
template <typename T>
class PmrPool {
public:
    explicit PmrPool(std::size_t capacity)
        : capacity_(capacity),
          buffer_(std::make_unique<std::byte[]>(buffer_bytes(capacity))),
          monotonic_(buffer_.get(), buffer_bytes(capacity), std::pmr::null_memory_resource()),
          pool_(std::pmr::pool_options{.max_blocks_per_chunk = capacity,
                                        .largest_required_pool_block = sizeof(T)},
                &monotonic_) {}

    PmrPool(const PmrPool&) = delete;
    PmrPool& operator=(const PmrPool&) = delete;

    [[nodiscard]] T* allocate() {
        if (live_count_ >= capacity_) {
            throw std::bad_alloc{};
        }
        auto* mem = static_cast<T*>(pool_.allocate(sizeof(T), alignof(T)));
        ++live_count_;
        return ::new (static_cast<void*>(mem)) T();
    }

    void deallocate(T* ptr) noexcept {
        std::destroy_at(ptr);
        pool_.deallocate(ptr, sizeof(T), alignof(T));
        --live_count_;
    }

    [[nodiscard]] std::size_t live_count() const noexcept { return live_count_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    [[nodiscard]] static std::size_t buffer_bytes(std::size_t capacity) noexcept {
        return capacity * sizeof(T) * 2 + (1u << 16);
    }

    std::size_t capacity_;
    std::size_t live_count_ = 0;
    std::unique_ptr<std::byte[]> buffer_;
    std::pmr::monotonic_buffer_resource monotonic_;
    std::pmr::unsynchronized_pool_resource pool_;
};

}  // namespace jane
