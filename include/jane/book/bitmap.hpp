#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace jane::book {

// A fixed-size bitset with the two "find nearest set bit" queries a price
// ladder needs to relocate the best price in O(NumLevels/64) worst case
// (one hardware bit-scan per 64-bit word) instead of O(NumLevels) — see
// docs/tradeoffs.md for a measured comparison against a naive linear scan.
//
// Both find_* functions bounds-check `i` themselves rather than requiring
// the caller to: an off-by-one here would silently read past the last
// word, and the cost of checking is negligible next to the cost of the
// bug.
class Bitmap {
public:
    explicit Bitmap(std::size_t num_bits) : num_bits_(num_bits), words_((num_bits + 63) / 64, 0) {}

    void set(std::size_t i) noexcept { words_[i / 64] |= (std::uint64_t{1} << (i % 64)); }
    void clear(std::size_t i) noexcept { words_[i / 64] &= ~(std::uint64_t{1} << (i % 64)); }
    [[nodiscard]] bool test(std::size_t i) const noexcept {
        return ((words_[i / 64] >> (i % 64)) & 1u) != 0;
    }

    // Highest set bit at index <= i, if any. An out-of-range i is clamped
    // to the last valid bit — "at or below a huge i" is just "anywhere".
    [[nodiscard]] std::optional<std::size_t> find_highest_at_or_below(std::size_t i) const noexcept {
        if (num_bits_ == 0) {
            return std::nullopt;
        }
        if (i >= num_bits_) {
            i = num_bits_ - 1;
        }
        auto word_idx = static_cast<std::ptrdiff_t>(i / 64);
        const std::size_t bit_in_word = i % 64;
        // Keep bits [0, bit_in_word] of the first word; shifting by 64 is
        // UB, so the "keep everything" case (bit_in_word == 63) is explicit.
        const std::uint64_t first_mask =
            (bit_in_word == 63) ? ~std::uint64_t{0} : ((std::uint64_t{1} << (bit_in_word + 1)) - 1);
        std::uint64_t w = words_[static_cast<std::size_t>(word_idx)] & first_mask;
        for (;;) {
            if (w != 0) {
                const int pos_in_word = 63 - std::countl_zero(w);
                return static_cast<std::size_t>(word_idx) * 64 + static_cast<std::size_t>(pos_in_word);
            }
            --word_idx;
            if (word_idx < 0) {
                return std::nullopt;
            }
            w = words_[static_cast<std::size_t>(word_idx)];
        }
    }

    // Lowest set bit at index >= i, if any. An out-of-range i has no valid
    // position at or above it by definition.
    [[nodiscard]] std::optional<std::size_t> find_lowest_at_or_above(std::size_t i) const noexcept {
        if (i >= num_bits_) {
            return std::nullopt;
        }
        std::size_t word_idx = i / 64;
        const std::size_t bit_in_word = i % 64;
        const std::uint64_t first_mask = ~std::uint64_t{0} << bit_in_word;
        std::uint64_t w = words_[word_idx] & first_mask;
        for (;;) {
            if (w != 0) {
                const int pos_in_word = std::countr_zero(w);
                return word_idx * 64 + static_cast<std::size_t>(pos_in_word);
            }
            ++word_idx;
            if (word_idx >= words_.size()) {
                return std::nullopt;
            }
            w = words_[word_idx];
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return num_bits_; }

private:
    std::size_t num_bits_;
    std::vector<std::uint64_t> words_;
};

}  // namespace jane::book
