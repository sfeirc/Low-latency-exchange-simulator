#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>

namespace jane {

// A fixed-capacity, allocation-free (after construction) object pool.
//
// Two parallel arrays: `storage_` holds raw, correctly-aligned bytes for
// up to Capacity live T's; `next_free_` holds, for each *free* slot, the
// index of the next free slot (an intrusive freelist, but kept as its own
// array rather than threaded through the object storage itself — a few
// extra bytes per slot in exchange for allocate/deallocate that are
// obviously well-defined: no union active-member questions, no type
// punning through the object representation, just placement-new into
// untouched raw storage and plain index bookkeeping). allocate() and
// deallocate() are O(1) and touch no syscall or global allocator.
//
// T must be trivially destructible: deallocate() returns the slot to the
// freelist without calling ~T(), because the pool has no correct point at
// which to call it beyond "the caller said they're done" — which is
// exactly what deallocate() means, so a non-trivial destructor would
// simply never run. Order and book::OrderNode are both trivially
// destructible, so this holds for everything actually pooled here.
template <typename T, std::size_t Capacity>
class SlabPool {
    static_assert(Capacity > 0);
    static_assert(std::is_trivially_destructible_v<T>,
                  "SlabPool never calls ~T(); T must not need one");

    using Index = std::uint32_t;
    static_assert(Capacity <= std::numeric_limits<Index>::max());
    static constexpr Index kNone = static_cast<Index>(-1);

    struct alignas(T) AlignedStorage {
        std::byte bytes[sizeof(T)];
    };

public:
    SlabPool() noexcept {
        for (Index i = 0; i + 1 < Capacity; ++i) {
            next_free_[i] = i + 1;
        }
        next_free_[Capacity - 1] = kNone;
    }
    SlabPool(const SlabPool&) = delete;
    SlabPool& operator=(const SlabPool&) = delete;

    // Returns nullptr on exhaustion — never throws, never allocates, so
    // the caller (e.g. order-entry on the hot path) can decide how to
    // react (reject the order) rather than crash or stall on a syscall.
    [[nodiscard]] T* allocate() noexcept {
        if (free_head_ == kNone) {
            return nullptr;
        }
        const Index idx = free_head_;
        free_head_ = next_free_[idx];
        ++live_count_;
        return ::new (static_cast<void*>(&storage_[idx])) T();
    }

    // ptr must have come from this pool's allocate() and not already have
    // been deallocated (use-after-free / double-free are on the caller,
    // same contract as malloc/free — this is a hot-path primitive, not a
    // safety net).
    void deallocate(T* ptr) noexcept {
        const Index idx = index_of(ptr);
        next_free_[idx] = free_head_;
        free_head_ = idx;
        --live_count_;
    }

    [[nodiscard]] std::size_t live_count() const noexcept { return live_count_; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    [[nodiscard]] Index index_of(T* ptr) const noexcept {
        const auto* slot = reinterpret_cast<const AlignedStorage*>(ptr);
        return static_cast<Index>(slot - storage_.data());
    }

    std::array<AlignedStorage, Capacity> storage_;
    std::array<Index, Capacity> next_free_{};
    Index free_head_ = 0;
    std::size_t live_count_ = 0;
};

}  // namespace jane
