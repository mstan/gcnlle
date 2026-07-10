# oracle/ — Dolphin differential-oracle scaffolding.
#
# This subtree carries no Dolphin code (the oracle must stay independent, and
# Dolphin is GPL — the trace-tap lives as a LOCAL patch plan in PATCH_PLAN.md,
# never transplanted here). The only thing to build is a compile/layout check
# for the shared binary trace format so CI catches ABI drift in the header.

cmake_minimum_required(VERSION 3.16)
project(gcnrecomp_oracle C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(trace_format_check trace_format_check.c)
target_include_directories(trace_format_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT MSVC)
  target_compile_options(trace_format_check PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME trace_format_layout COMMAND trace_format_check)
