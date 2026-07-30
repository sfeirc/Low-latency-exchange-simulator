set(JANE_SOURCES
  version.cpp
)

add_library(jane STATIC ${JANE_SOURCES})
add_library(jane::jane ALIAS jane)

target_include_directories(jane PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(jane
  PUBLIC
    Threads::Threads
    unordered_dense::unordered_dense
  PRIVATE
    jane_warnings
    jane_options
)
