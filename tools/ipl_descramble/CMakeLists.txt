cmake_minimum_required(VERSION 3.16)
project(ipl_descramble C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(ipl_descramble_core STATIC descramble_core.c)
target_include_directories(ipl_descramble_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(ipl_descramble main.c)
target_link_libraries(ipl_descramble PRIVATE ipl_descramble_core)

enable_testing()
add_executable(test_descramble test_descramble.c)
target_link_libraries(test_descramble PRIVATE ipl_descramble_core)
add_test(NAME descramble_selftest COMMAND $<TARGET_FILE:test_descramble>)
