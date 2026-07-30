#pragma once

#include <chrono>
#include <concepts>

#include "jane/core/types.hpp"

namespace jane {

// Anything that can hand out a monotonically non-decreasing Nanos reading.
// Lets the sequencer / book take a clock as a template parameter so tests
// and jane::replay can substitute a ManualClock and get fully deterministic
// timestamps instead of wall-clock noise.
template <typename C>
concept ClockLike = requires {
    { C::now() } -> std::same_as<Nanos>;
};

// The real clock used outside of tests/replay: steady (monotonic, immune to
// NTP/wall-clock adjustments), converted to nanoseconds explicitly rather
// than assuming the platform's steady_clock::duration is already ns.
class SystemClock {
public:
    [[nodiscard]] static Nanos now() noexcept {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch());
        return Nanos{ns.count()};
    }
};

static_assert(ClockLike<SystemClock>);

}  // namespace jane
