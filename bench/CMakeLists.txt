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
