#pragma once

#include <string_view>

namespace jane {

struct Version {
    int major;
    int minor;
    int patch;
};

[[nodiscard]] constexpr Version version() noexcept { return {0, 1, 0}; }
[[nodiscard]] std::string_view version_string() noexcept;

}  // namespace jane
