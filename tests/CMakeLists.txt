set(JANE_TEST_SOURCES
  test_version.cpp
  test_core_types.cpp
)

add_executable(jane_tests ${JANE_TEST_SOURCES})

target_link_libraries(jane_tests
  PRIVATE
    jane::jane
    Catch2::Catch2WithMain
    jane_warnings
    jane_options
)

include(CTest)
include(Catch)
catch_discover_tests(jane_tests)
