add_executable(ma-tests test_core.cpp)
target_link_libraries(ma-tests PRIVATE ma_core ma_io)
add_test(NAME core COMMAND ma-tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
