#include <catch2/catch_test_macros.hpp>

#include "jane/core/version.hpp"

TEST_CASE("version is well-formed", "[version]") {
    constexpr auto v = jane::version();
    STATIC_REQUIRE(v.major == 0);
    REQUIRE(jane::version_string() == "0.1.0");
}
