add_executable(bookview bookview/main.cpp)
target_link_libraries(bookview PRIVATE jane::jane jane_warnings jane_options)
