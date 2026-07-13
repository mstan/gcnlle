# Generates the synthetic bl/blr chunk (GENERATOR_EXE, from
# test_bl_shadow_gen.c), compiles it together with the FIXED harness
# (HARNESS_SRC, test_bl_shadow_harness.c) into a standalone executable, and
# RUNS it -- a real link+execute exactness check for the in-chunk bl/blr
# shadow fast path (recompiler/src/backend/emitter.c), not just a compile
# check. Modeled on codegen_compile.cmake's generate-then-build pattern, one
# stage further (build an executable and execute it rather than stopping at
# an object file).
execute_process(
    COMMAND "${GENERATOR_EXE}" "${OUTPUT_C}"
    RESULT_VARIABLE gen_result
    OUTPUT_VARIABLE gen_stdout
    ERROR_VARIABLE gen_stderr
)
if(NOT gen_result EQUAL 0)
    message(FATAL_ERROR "bl_shadow chunk generation failed:\n${gen_stdout}\n${gen_stderr}")
endif()

get_filename_component(output_dir "${OUTPUT_C}" DIRECTORY)
set(check_src_dir "${output_dir}/bl_shadow_check_project")
set(check_build_dir "${output_dir}/bl_shadow_check_build")

file(TO_CMAKE_PATH "${OUTPUT_C}" output_c_cmake)
file(TO_CMAKE_PATH "${HARNESS_SRC}" harness_src_cmake)
file(TO_CMAKE_PATH "${REPO_SRC}" repo_src_cmake)

file(MAKE_DIRECTORY "${check_src_dir}")
file(WRITE "${check_src_dir}/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.16)
project(BlShadowCheck C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_executable(bl_shadow_check \"${output_c_cmake}\" \"${harness_src_cmake}\")
target_include_directories(bl_shadow_check PRIVATE \"${repo_src_cmake}\")
")

set(configure_args -S "${check_src_dir}" -B "${check_build_dir}")
if(DEFINED HOST_GENERATOR AND NOT HOST_GENERATOR STREQUAL "")
    list(APPEND configure_args -G "${HOST_GENERATOR}")
endif()
if(DEFINED HOST_GENERATOR_PLATFORM AND NOT HOST_GENERATOR_PLATFORM STREQUAL "")
    list(APPEND configure_args -A "${HOST_GENERATOR_PLATFORM}")
endif()
if(DEFINED HOST_GENERATOR_TOOLSET AND NOT HOST_GENERATOR_TOOLSET STREQUAL "")
    list(APPEND configure_args -T "${HOST_GENERATOR_TOOLSET}")
endif()
if(DEFINED HOST_C_COMPILER AND NOT HOST_C_COMPILER STREQUAL "")
    list(APPEND configure_args "-DCMAKE_C_COMPILER=${HOST_C_COMPILER}")
endif()
if(DEFINED HOST_BUILD_CONFIG
        AND NOT HOST_BUILD_CONFIG STREQUAL ""
        AND NOT HOST_GENERATOR MATCHES "Visual Studio|Xcode|Ninja Multi-Config")
    list(APPEND configure_args "-DCMAKE_BUILD_TYPE=${HOST_BUILD_CONFIG}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${configure_args}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "bl_shadow configure failed:\n${configure_stdout}\n${configure_stderr}")
endif()

set(build_args --build "${check_build_dir}")
if(DEFINED HOST_BUILD_CONFIG AND NOT HOST_BUILD_CONFIG STREQUAL "")
    list(APPEND build_args --config "${HOST_BUILD_CONFIG}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${build_args}
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "bl_shadow check project did not build:\n${build_stdout}\n${build_stderr}")
endif()

# Locate the built executable (single-config generators put it directly in
# check_build_dir; multi-config generators nest it under $<CONFIG>/).
set(exe_candidates
    "${check_build_dir}/bl_shadow_check${CMAKE_EXECUTABLE_SUFFIX}"
    "${check_build_dir}/bl_shadow_check.exe"
)
if(DEFINED HOST_BUILD_CONFIG AND NOT HOST_BUILD_CONFIG STREQUAL "")
    list(APPEND exe_candidates
        "${check_build_dir}/${HOST_BUILD_CONFIG}/bl_shadow_check${CMAKE_EXECUTABLE_SUFFIX}"
        "${check_build_dir}/${HOST_BUILD_CONFIG}/bl_shadow_check.exe"
    )
endif()
set(exe_path "")
foreach(candidate ${exe_candidates})
    if(EXISTS "${candidate}")
        set(exe_path "${candidate}")
        break()
    endif()
endforeach()
if(exe_path STREQUAL "")
    message(FATAL_ERROR "bl_shadow_check executable not found after build (looked in: ${exe_candidates})")
endif()

execute_process(
    COMMAND "${exe_path}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_stdout
    ERROR_VARIABLE run_stderr
)
message(STATUS "${run_stdout}")
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "bl_shadow_check reported failure (exit ${run_result}):\n${run_stdout}\n${run_stderr}")
endif()
