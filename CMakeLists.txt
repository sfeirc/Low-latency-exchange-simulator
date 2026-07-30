cmake_minimum_required(VERSION 3.25)

project(jane
  VERSION 0.1.0
  DESCRIPTION "JANE — a low-latency electronic exchange core: order entry, matching, risk, market data"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(JANE_BUILD_TESTS      "Build unit / property tests"        ON)
option(JANE_BUILD_BENCH      "Build Google Benchmark suites"      ON)
option(JANE_BUILD_APPS       "Build executables (apps/bookview)"  ON)
option(JANE_NATIVE_ARCH      "Compile with -march=native"         ON)
option(JANE_ENABLE_ASAN      "Enable Address/UndefinedBehavior sanitizers" OFF)
option(JANE_ENABLE_TSAN      "Enable ThreadSanitizer (exclusive with ASAN)" OFF)
option(JANE_ENABLE_LTO       "Enable interprocedural (LTO) optimization for Release" ON)
option(JANE_WERROR           "Treat warnings as errors"           OFF)

if(JANE_ENABLE_ASAN AND JANE_ENABLE_TSAN)
  message(FATAL_ERROR "JANE_ENABLE_ASAN and JANE_ENABLE_TSAN are mutually exclusive")
endif()

# ---------------------------------------------------------------------------
# Warnings / compile options (INTERFACE libraries — link, don't inherit blindly)
# ---------------------------------------------------------------------------
add_library(jane_warnings INTERFACE)
target_compile_options(jane_warnings INTERFACE
  -Wall -Wextra -Wpedantic
  -Wshadow
  -Wconversion -Wsign-conversion
  -Wcast-align
  -Wold-style-cast
  -Woverloaded-virtual
  -Wnon-virtual-dtor
  -Wnull-dereference
  -Wdouble-promotion
  -Wformat=2
  -Wimplicit-fallthrough
)
if(JANE_WERROR)
  target_compile_options(jane_warnings INTERFACE -Werror)
endif()

add_library(jane_options INTERFACE)
target_compile_features(jane_options INTERFACE cxx_std_23)

if(JANE_NATIVE_ARCH)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-march=native" JANE_HAS_MARCH_NATIVE)
  if(JANE_HAS_MARCH_NATIVE)
    target_compile_options(jane_options INTERFACE -march=native)
  endif()
endif()

# Release-ish builds: full optimization, keep frame pointer for profiling (perf/async-profiler)
target_compile_options(jane_options INTERFACE
  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:RelWithDebInfo>:-O3;-g;-fno-omit-frame-pointer>
)

if(JANE_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT JANE_IPO_SUPPORTED OUTPUT JANE_IPO_ERROR)
  if(JANE_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
  endif()
endif()

if(JANE_ENABLE_ASAN)
  target_compile_options(jane_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(jane_options INTERFACE -fsanitize=address,undefined)
endif()
if(JANE_ENABLE_TSAN)
  target_compile_options(jane_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(jane_options INTERFACE -fsanitize=thread)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Third-party
# ---------------------------------------------------------------------------
include(FetchContent)

# ankerl::unordered_dense — header-only, open-addressing flat hash map/set.
# Used on the order-lookup hot path (cancel/replace) in place of std::unordered_map.
FetchContent_Declare(
  unordered_dense
  GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
  GIT_TAG        v4.8.1
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(unordered_dense)

if(JANE_BUILD_TESTS)
  find_package(Catch2 3 REQUIRED)
endif()
if(JANE_BUILD_BENCH)
  find_package(benchmark REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(JANE_BUILD_APPS)
  add_subdirectory(apps)
endif()

if(JANE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(JANE_BUILD_BENCH)
  add_subdirectory(bench)
endif()

message(STATUS "jane: build type            = ${CMAKE_BUILD_TYPE}")
message(STATUS "jane: native arch            = ${JANE_NATIVE_ARCH}")
message(STATUS "jane: LTO                    = ${CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE}")
message(STATUS "jane: ASan/UBSan             = ${JANE_ENABLE_ASAN}")
message(STATUS "jane: TSan                   = ${JANE_ENABLE_TSAN}")
message(STATUS "jane: build tests            = ${JANE_BUILD_TESTS}")
message(STATUS "jane: build benchmarks       = ${JANE_BUILD_BENCH}")
