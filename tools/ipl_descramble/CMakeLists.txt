cmake_minimum_required(VERSION 3.16)
project(ipl_descramble C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(ipl_descramble_core STATIC descramble_core.c)
target_include_directories(ipl_descramble_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The CLI tool + its self-test are only meaningful for this project's OWN
# standalone build (build.sh's `tools/ipl_descramble`). When runtime/CMakeLists.txt
# pulls this directory in via add_subdirectory (to reuse ipl_descramble_core —
# M1's runtime EXI descrambler — as one compiled object instead of a second
# copy), CMAKE_PROJECT_NAME is the OUTER project ("gcnrecomp_runtime"), so this
# guard skips re-registering `ipl_descramble`/`test_descramble`/its ctest entry
# into that unrelated project's test list and default build set.
if(CMAKE_PROJECT_NAME STREQUAL "ipl_descramble")
  add_executable(ipl_descramble main.c)
  target_link_libraries(ipl_descramble PRIVATE ipl_descramble_core)

  enable_testing()
  add_executable(test_descramble test_descramble.c)
  target_link_libraries(test_descramble PRIVATE ipl_descramble_core)
  add_test(NAME descramble_selftest COMMAND $<TARGET_FILE:test_descramble>)
endif()
