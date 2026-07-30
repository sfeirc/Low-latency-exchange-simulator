#pragma once

#include <cstddef>

namespace jane {

// Hardcoded rather than std::hardware_destructive_interference_size: that
// constant is implementation-defined per translation unit, GCC ties it to
// -mtune/ABI flags, and mixing TUs compiled with different flags is a real
// footgun (the value can legally differ, silently breaking the padding
// assumption). 64 bytes is the L1 cache line size for every x86-64 and
// ARM64 part this project targets — hardcoding it is more predictable than
// a "portable" constant that isn't actually stable across this project's
// own build.
inline constexpr std::size_t kCacheLineSize = 64;

}  // namespace jane
