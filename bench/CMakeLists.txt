# Each benchmark binary links against jane::jane + benchmark::benchmark_main
# and is added here as its component lands.

function(jane_add_benchmark name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name}
    PRIVATE
      jane::jane
      benchmark::benchmark_main
      jane_warnings
      jane_options
  )
endfunction()

jane_add_benchmark(bench_spsc_ring_buffer bench_spsc_ring_buffer.cpp)
jane_add_benchmark(bench_mpsc bench_mpsc.cpp)
jane_add_benchmark(bench_memory_pool bench_memory_pool.cpp)
jane_add_benchmark(bench_protocol bench_protocol.cpp)
jane_add_benchmark(bench_order_book bench_order_book.cpp)
jane_add_benchmark(bench_order_book_backends bench_order_book_backends.cpp)
jane_add_benchmark(bench_matching_engine bench_matching_engine.cpp)
jane_add_benchmark(bench_end_to_end bench_end_to_end.cpp)
