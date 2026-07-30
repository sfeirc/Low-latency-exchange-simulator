set(JANE_TEST_SOURCES
  test_version.cpp
  test_core_types.cpp
  test_spsc_ring_buffer.cpp
  test_mpsc.cpp
  test_memory_pool.cpp
  test_protocol.cpp
  test_bitmap.cpp
  test_price_level.cpp
  test_order_book.cpp
  test_price_index_variants.cpp
  test_matching_engine.cpp
  test_correctness_invariants.cpp
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
