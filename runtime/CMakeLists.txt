cmake_minimum_required(VERSION 3.16)
project(gcnrecomp_runtime C CXX)

option(GCN_USE_CCACHE "Use ccache when it is available" ON)
if(GCN_USE_CCACHE)
  find_program(GCN_CCACHE_PROGRAM ccache)
  if(GCN_CCACHE_PROGRAM)
    if(NOT CMAKE_C_COMPILER_LAUNCHER)
      set(CMAKE_C_COMPILER_LAUNCHER "${GCN_CCACHE_PROGRAM}")
    endif()
    if(NOT CMAKE_CXX_COMPILER_LAUNCHER)
      set(CMAKE_CXX_COMPILER_LAUNCHER "${GCN_CCACHE_PROGRAM}")
    endif()
    message(STATUS "gcnrecomp compiler cache: ${GCN_CCACHE_PROGRAM}")
  endif()
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(GCN_VULKAN "Build the opt-in headless Vulkan GX shadow capability path" ON)
if(GCN_VULKAN)
  find_package(Vulkan QUIET)
endif()

# DSP LLE core (C++ static lib running the real DSP firmware). Built always so
# the default runtime build catches breakage; linked into gcn_boot.
add_subdirectory(dsp_lle)

# The DSP interpreter is split across the opcode-family translation units in
# dsp_lle/Core/DSP/Interpreter. GCC otherwise cannot inline through those TU
# boundaries, leaving a member-function call for every real firmware
# instruction. LTO only this small C++ library (NOT the 100+ giant generated
# PPC C chunks), then enable the linker plugin on gcn_boot below. This remains
# genuine LLE: every DSP instruction and cycle is still executed. On the IPL
# derived workload it reduced the interpreter's measured ns/instruction by
# roughly 27% on top of the fused dispatch table. Disable for compiler/debug
# troubleshooting with -DGCN_DSP_LTO=OFF.
option(GCN_DSP_LTO "Use GCC LTO for the vendored DSP-LLE interpreter only" ON)
if(GCN_DSP_LTO AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(gcn_dsp_lle PRIVATE -flto=auto)
endif()

# M1: the vendored segher IPL descrambler (tools/ipl_descramble/descramble_core.c)
# as its OWN existing CMake target (ipl_descramble_core) — reused verbatim by
# gcn_boot's EXI model (exi.c gcn_exi_set_rom_scrambled) rather than recompiled
# a second time under a different TU, per PRINCIPLES "one algorithm, every call
# site switches to it". EXCLUDE_FROM_ALL: gcn_boot links it on demand; it does
# not become part of `all` for builds that never enable GCN_WITH_GENERATED.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tools/ipl_descramble
                  ${CMAKE_CURRENT_BINARY_DIR}/ipl_descramble EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# LLE runtime for the recompiled GameCube IPL.
#
# This is the psxrecomp/runtime analogue: it hosts the recompiled firmware and
# provides faithful low-level models of the GameCube hardware the IPL menu
# touches. It is deliberately LLE — we model raw Flipper/EXI/DI/VI MMIO, not the
# Dolphin SDK. HLE is only ever a *validated* per-subsystem replacement on top
# of this baseline (see ../PRINCIPLES.md).
#
# M0 (declarative seed contract) modules that exist now:
#   src/memory.c    flat big-endian MEM1 (24MB) + uncached mirror + DolRecomp
#                   bus primitives (mem_read/write 8/16/32/64) + lifecycle.
#   src/cpu_glue.c  the ppc_* semantic helper family the generated C links
#                   against (exceptions, SPR/TB, psq, fma, fctiw, fres/frsqrte).
#   src/seed.c      the M0 declarative seed loader (BS2 payload + entry PC +
#                   MEM1 reset image + EXI SRAM/RTC fixtures, every field with
#                   a provenance tag). See ../runtime/ABI.md for the DolRecomp
#                   codegen ABI these target and how Track A's output reconciles.
#
# Device models to grow here in later milestones (cf. reshine runtime/src and
# GXRuntime's C device models which we harvest under GPL-3.0):
#   exi.c   EXI channels: RTC (date/time) + SRAM (settings) + memory cards
#   vi.c    Video Interface: framebuffer scanout / XFB -> host window
#   gx.c    Flipper GX FIFO command processor -> host renderer
#   dsp.c/ai.c  DSP + Audio Interface (boot chime, menu sound)
#   di.c    Disc Interface (disc-load screen; stubbed until disc phase)
#   si.c    Serial Interface (controller input in the menu)
#   gcn_dispatch.c  block dispatch loop over Track A's generated func_* table
#   debug_server.c  TCP debug surface + always-on ring buffers (MMIO/PC/event),
#                   screenshot, state queries, input injection — cross-cutting,
#                   stood up before M2 (see docs/ROADMAP.md "Observability").
# ---------------------------------------------------------------------------

# Core runtime library: the ABI implementation + seed. Kept separate so both the
# executable and the unit test link the same objects (and so future generated C
# links against exactly these symbols).
add_library(gcn_runtime_core STATIC
  src/memory.c
  src/cpu_glue.c
  src/interpreter.c
  src/aot_module.c
  src/native_code.c
  src/dsp_regs.c
  src/title_module_none.c
  src/rings.c
  src/seed.c
  src/trace.c
  src/util/crc32.c
  ../recompiler/src/frontend/decoder.c
  ../recompiler/src/backend/ppc_cycles.c
)
target_include_directories(gcn_runtime_core PUBLIC include)
target_include_directories(gcn_runtime_core PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/src)
# trace.c emits the shared oracle record format (oracle/trace_format.h).
target_include_directories(gcn_runtime_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../oracle)

# Host executable (M0: applies the seed to a supplied descrambled BS2 dump).
add_executable(gcn_runtime
  src/main.c
)
target_link_libraries(gcn_runtime PRIVATE gcn_runtime_core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

add_executable(test_seed tests/test_seed.c)
target_link_libraries(test_seed PRIVATE gcn_runtime_core)
add_test(NAME seed_contract COMMAND test_seed)

add_executable(test_interpreter tests/test_interpreter.c)
target_link_libraries(test_interpreter PRIVATE gcn_runtime_core)
add_test(NAME gekko_interpreter COMMAND test_interpreter)

add_executable(test_aot_module tests/test_aot_module.c)
target_link_libraries(test_aot_module PRIVATE gcn_runtime_core)
add_test(NAME aot_module_identity COMMAND test_aot_module)

add_executable(test_dsp_registers tests/test_dsp_registers.c)
target_link_libraries(test_dsp_registers PRIVATE gcn_runtime_core)
add_test(NAME dsp_register_reset COMMAND test_dsp_registers)

add_executable(test_decrementer tests/test_decrementer.c)
target_link_libraries(test_decrementer PRIVATE gcn_runtime_core)
add_test(NAME decrementer_timing COMMAND test_decrementer)

add_executable(test_locked_l1 tests/test_locked_l1.c)
target_link_libraries(test_locked_l1 PRIVATE gcn_runtime_core)
add_test(NAME locked_l1_memory COMMAND test_locked_l1)

add_executable(test_vi_xfb tests/test_vi_xfb.c src/vi.c)
target_link_libraries(test_vi_xfb PRIVATE gcn_runtime_core)
add_test(NAME vi_xfb_geometry COMMAND test_vi_xfb)

# M4: EXI memcard device model + GCMemcard image module. Both are standalone
# (only libc + their own headers), so they build/test without the generated C.
add_executable(test_memcard tests/test_memcard.c src/memcard.c)
target_include_directories(test_memcard PRIVATE include)
add_test(NAME memcard_device COMMAND test_memcard)

add_executable(test_memcard_image tests/test_memcard_image.c src/memcard_image.c)
target_include_directories(test_memcard_image PRIVATE include)
add_test(NAME memcard_image COMMAND test_memcard_image)

add_executable(test_rtc tests/test_rtc.c src/rtc.c)
target_include_directories(test_rtc PRIVATE include)
add_test(NAME rtc_boot_sync COMMAND test_rtc)

add_executable(test_host_audio tests/test_host_audio.c src/host_audio.c)
target_include_directories(test_host_audio PRIVATE include)
if(WIN32)
  target_link_libraries(test_host_audio PRIVATE ole32 uuid avrt)
endif()
add_test(NAME host_audio_pcm COMMAND test_host_audio)

add_executable(test_gx_pipeline tests/test_gx_pipeline.c)
target_include_directories(test_gx_pipeline PRIVATE include)
add_test(NAME gx_pipeline_carry_boundary COMMAND test_gx_pipeline)

# Interpreter counted-cache-loop batching fast path: exactness of batched vs.
# unbatched icbi/dcbi loop execution (GPR/CTR/cycles/native-invalidation),
# including a cycle-deadline crossing mid-loop.
add_executable(test_cache_loop_batch tests/test_cache_loop_batch.c)
target_link_libraries(test_cache_loop_batch PRIVATE gcn_runtime_core)
add_test(NAME cache_loop_batch_exactness COMMAND test_cache_loop_batch)

# Native-miss page-CRC memo: duplicate misses on an unchanged page reuse the
# cached identity; any writer through the gcn_native_code_invalidate funnel
# (store/DMA/icbi) forces a rehash with unchanged identity semantics.
add_executable(test_page_crc_memo tests/test_page_crc_memo.c)
target_link_libraries(test_page_crc_memo PRIVATE gcn_runtime_core)
add_test(NAME page_crc_memo_identity COMMAND test_page_crc_memo)

add_executable(gcn_memcard_inspect tools/memcard_inspect.c src/memcard_image.c)
target_include_directories(gcn_memcard_inspect PRIVATE include)

# ---------------------------------------------------------------------------
# M0 execution target: link Track A's recompiled BS2 and run it.
#
# Off by default because it requires runtime/generated/ to be populated first
# (run `runtime/generate.sh`, which needs bios/ipl.bin + the built recompiler).
# Enable with -DGCN_WITH_GENERATED=ON.
# ---------------------------------------------------------------------------
option(GCN_WITH_GENERATED "Build gcn_boot with the recompiled BS2 (needs runtime/generated/)" OFF)
if(GCN_WITH_GENERATED)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/generated/generated.h)
    message(FATAL_ERROR "GCN_WITH_GENERATED=ON but runtime/generated/ is empty. Run runtime/generate.sh first.")
  endif()
  file(GLOB GCN_GENERATED_CHUNKS CONFIGURE_DEPENDS
       ${CMAKE_CURRENT_SOURCE_DIR}/generated/chunks/*.c)
  set(GCN_GENERATED_BUILD_PROFILE "ITERATE" CACHE STRING
      "Generated guest-code compile profile: ITERATE or RELEASE")
  set_property(CACHE GCN_GENERATED_BUILD_PROFILE PROPERTY STRINGS ITERATE RELEASE)
  # Phase D2 (codegen speed campaign, static analysis): -fopt-info-missed on a
  # representative 16KB chunk (chunk_0000, ~4096 case labels/one giant
  # function) showed 66% (797/1206) of the Phase C
  # dolrecomp_mem_read/write*_fast calls -- the `static inline` helpers whose
  # entire point is to get inlined into a branchless RAM-window check on the
  # hot path -- were NOT actually inlined at plain -O2. Root cause: GCC's
  # large-function-growth/inline-unit-growth budgets are sized for ordinary
  # functions and get exhausted partway through a single 4096-instruction
  # function, so only the FIRST fraction of call sites get inlined and the
  # rest silently fall back to a real CALL, defeating Phase C's inlining
  # premise for roughly two-thirds of memory accesses in a chunk this size.
  # Raising the three budgets below (scoped to ONLY the generated chunk TUs,
  # never globally -- these are giant-function-specific, would be the wrong
  # default for every other TU in this project) cuts uninlined fast-path
  # calls from 797 to 391 (-51%) on the same representative chunk, at a
  # measured +30% instruction count / +60% compile time for that one file
  # (verified: `-fno-jump-tables` and raw `-O3` were also measured and are
  # NOT adopted here -- see Phase D2 report for the full comparison table).
  # The remaining 391 are blocked by GCC's cold/unlikely-call heuristics, not
  # a size budget, and closing that gap needs riskier global flags (branch-
  # probability heuristics) not attempted here. This is a static, mechanical
  # measurement (real CALL instructions removed on the hot RAM-hit path);
  # the actual wall-clock effect needs the coordinator's boot-time/rdtsc
  # attribution harness (see docs/ROADMAP.md), not asserted from this alone.
  if(GCN_GENERATED_BUILD_PROFILE STREQUAL "ITERATE")
    # The default development loop values compile latency over final host-code
    # shape. -O1 follows the configuration's -O2 and therefore wins; -g1 keeps
    # usable symbols without emitting full debug data for every guest label.
    set_source_files_properties(${GCN_GENERATED_CHUNKS} PROPERTIES
      COMPILE_OPTIONS "-O1;-g1"
    )
  elseif(GCN_GENERATED_BUILD_PROFILE STREQUAL "RELEASE")
    set_source_files_properties(${GCN_GENERATED_CHUNKS} PROPERTIES
      COMPILE_OPTIONS "--param;large-function-insns=20000;--param;large-function-growth=250;--param;inline-unit-growth=100"
    )
  else()
    message(FATAL_ERROR
      "GCN_GENERATED_BUILD_PROFILE must be ITERATE or RELEASE")
  endif()

  # Optional per-title DOL module. Its generated dispatcher is linked beside,
  # never instead of, the IPL dispatcher. Live MEM1 bytes must match the
  # generated content hashes before any title chunk can execute natively.
  set(GCN_TITLE_GENERATED_DIR "" CACHE PATH
      "DolRecomp output directory for an optional content-validated title DOL")
  set(GCN_TITLE_DOL "" CACHE FILEPATH
      "Exact DOL input used to generate GCN_TITLE_GENERATED_DIR")
  set(GCN_TITLE_MODULE_SOURCE)
  set(GCN_TITLE_GENERATED_CHUNKS)
  set(GCN_TITLE_MODULE_TABLES)
  if(GCN_TITLE_GENERATED_DIR)
    if(NOT GCN_TITLE_DOL)
      message(FATAL_ERROR "GCN_TITLE_GENERATED_DIR requires GCN_TITLE_DOL")
    endif()
    foreach(GCN_TITLE_REQUIRED generated.h generated_abi.h)
      if(NOT EXISTS "${GCN_TITLE_GENERATED_DIR}/${GCN_TITLE_REQUIRED}")
        message(FATAL_ERROR
          "title module is missing ${GCN_TITLE_GENERATED_DIR}/${GCN_TITLE_REQUIRED}")
      endif()
    endforeach()
    if(NOT EXISTS "${GCN_TITLE_DOL}")
      message(FATAL_ERROR "title DOL does not exist: ${GCN_TITLE_DOL}")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    file(GLOB GCN_TITLE_GENERATED_CHUNKS CONFIGURE_DEPENDS
         "${GCN_TITLE_GENERATED_DIR}/chunks/*.c")
    if(NOT GCN_TITLE_GENERATED_CHUNKS)
      message(FATAL_ERROR
        "title module has no chunks: ${GCN_TITLE_GENERATED_DIR}/chunks")
    endif()
    set(GCN_TITLE_MODULE_TABLES
        "${CMAKE_CURRENT_BINARY_DIR}/title_module_tables.inc")
    add_custom_command(
      OUTPUT "${GCN_TITLE_MODULE_TABLES}"
      COMMAND Python3::Interpreter
              "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_title_module_tables.py"
              --generated-header "${GCN_TITLE_GENERATED_DIR}/generated.h"
              --dol "${GCN_TITLE_DOL}"
              --output "${GCN_TITLE_MODULE_TABLES}"
      DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_title_module_tables.py"
        "${GCN_TITLE_GENERATED_DIR}/generated.h"
        "${GCN_TITLE_DOL}"
      VERBATIM
      COMMENT "Generating content identities for the title AOT module")
    file(TO_CMAKE_PATH "${GCN_TITLE_GENERATED_DIR}/generated.h"
         GCN_TITLE_GENERATED_HEADER)
    configure_file(src/title_module.c.in
                   "${CMAKE_CURRENT_BINARY_DIR}/title_module.c" @ONLY)
    set(GCN_TITLE_MODULE_SOURCE
        "${CMAKE_CURRENT_BINARY_DIR}/title_module.c")
    set_source_files_properties("${GCN_TITLE_MODULE_TABLES}" PROPERTIES
      GENERATED TRUE HEADER_FILE_ONLY TRUE)
    if(GCN_GENERATED_BUILD_PROFILE STREQUAL "ITERATE")
      set_source_files_properties(${GCN_TITLE_GENERATED_CHUNKS} PROPERTIES
        COMPILE_OPTIONS "-O1;-g1")
    else()
      set_source_files_properties(${GCN_TITLE_GENERATED_CHUNKS} PROPERTIES
        COMPILE_OPTIONS
          "--param;large-function-insns=20000;--param;large-function-growth=250;--param;inline-unit-growth=100")
    endif()
    message(STATUS
      "LLE content-validated title module: ${GCN_TITLE_GENERATED_DIR}")
  endif()

  add_executable(gcn_boot
    src/boot.c
    src/dispatch.c
    src/mmio.c
    src/exi.c
    src/rtc.c
    src/memcard.c
    src/memcard_image.c
    src/si.c
    src/pi.c
    src/dsp.c
    src/ai.c
    src/vi.c
    src/di.c
    src/mi.c
    src/pe.c
    src/cp.c
    src/gp.c
    src/gx.c
    src/gx_render.c
    src/gx_raster.c
    src/gx_vulkan.c
    src/debug_server.c
    src/host_audio.c
    src/host_window.c
    ${GCN_TITLE_MODULE_SOURCE}
    ${GCN_TITLE_MODULE_TABLES}
    ${GCN_TITLE_GENERATED_CHUNKS}
    generated/generated.c
    ${GCN_GENERATED_CHUNKS}
  )
  # The late IPL menus spend most of their time in the exact R/S raster setup
  # paths.  Optimize that translation unit independently of stale whole-boot
  # PGO weights.  FP contraction must stay off: fused multiply-add changed
  # clip-boundary classification in the exact triangle-count gate.
  option(GCN_X86_64_V3
         "Use the AVX2-class x86-64-v3 baseline for the rendered IPL path" ON)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(GCN_GX_RASTER_OPTIONS -O3 -ffp-contract=off)
    if(GCN_X86_64_V3 AND CMAKE_SYSTEM_PROCESSOR MATCHES
       "^(AMD64|amd64|x86_64)$")
      # A fixed ISA/tuning pair keeps release artifacts reproducible while
      # retaining the scheduling win measured on the rendered R/S path.
      list(APPEND GCN_GX_RASTER_OPTIONS -march=x86-64-v3 -mtune=skylake)
    endif()
    set_property(SOURCE src/gx_raster.c APPEND PROPERTY
                 COMPILE_OPTIONS ${GCN_GX_RASTER_OPTIONS})
  endif()
  # generated/ for "generated.h"; include/ for cpu|memory|seed|dispatch headers.
  # descramble_core.h comes in transitively via ipl_descramble_core's PUBLIC
  # include dir (M1: exi.c/boot.c call the vendored segher descrambler
  # verbatim — one algorithm, one compiled object, two consumers).
  target_include_directories(gcn_boot PRIVATE include generated)
  if(GCN_TITLE_GENERATED_DIR)
    target_include_directories(gcn_boot PRIVATE
      "${GCN_TITLE_GENERATED_DIR}"
      "${CMAKE_CURRENT_BINARY_DIR}")
  endif()
  # ws2_32: Winsock for the TCP debug server (debug_server.c).
  # synchronization: WaitOnAddress/WakeByAddressAll for the GX-MT worker
  # pool's hybrid spin/sleep wait (gx_raster.c).
  # winmm: timeBeginPeriod for GCN_THROTTLE's Sleep() granularity (dispatch.c).
  # ole32/uuid/avrt: event-driven WASAPI host audio + MMCSS worker priority
  # (host_audio.c).
  # user32/gdi32: window class/message pump + StretchDIBits for the opt-in
  # GCN_WINDOW=1 native display window (host_window.c).
  # opengl32: opt-in WGL/fixed-function GL 1.1 present path (GCN_GL=1)
  # alongside the default GDI one, same file.
  target_link_libraries(gcn_boot PRIVATE gcn_runtime_core gcn_dsp_lle
                         ipl_descramble_core ws2_32 synchronization winmm ole32 uuid avrt
                         user32 gdi32 opengl32)
  if(Vulkan_FOUND)
    set(GX_CLEAR_SPV_INC ${CMAKE_CURRENT_BINARY_DIR}/gx_clear.comp.inc)
    set(GX_XFB_COPY_SPV_INC ${CMAKE_CURRENT_BINARY_DIR}/gx_xfb_copy.comp.inc)
    set(GX_XFB_SUBGROUP_SPV_INC ${CMAKE_CURRENT_BINARY_DIR}/gx_xfb_subgroup.comp.inc)
    set(GX_DRAW_F_SPV_INC ${CMAKE_CURRENT_BINARY_DIR}/gx_draw_f.comp.inc)
    add_custom_command(
      OUTPUT ${GX_CLEAR_SPV_INC}
      COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.1 -O
              -mfmt=c -o ${GX_CLEAR_SPV_INC}
              ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gx_clear.comp
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gx_clear.comp
      VERBATIM
      COMMENT "Compiling exact GX EFB clear compute shader"
    )
    add_custom_command(
      OUTPUT ${GX_XFB_COPY_SPV_INC}
      COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.1 -O
              -mfmt=c -o ${GX_XFB_COPY_SPV_INC}
              ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gx_xfb_copy.comp
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gx_xfb_copy.comp
      VERBATIM
      COMMENT "Compiling exact GX EFB-to-XFB compute shader"
    )
    add_custom_command(
      OUTPUT ${GX_XFB_SUBGROUP_SPV_INC}
      COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.1 -O
              -DGCN_XFB_SUBGROUP=1 -mfmt=c -o ${GX_XFB_SUBGROUP_SPV_INC}
              ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gx_xfb_copy.comp
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gx_xfb_copy.comp
      VERBATIM
      COMMENT "Compiling exact subgroup GX EFB-to-XFB compute shader"
    )
    add_custom_command(
      OUTPUT ${GX_DRAW_F_SPV_INC}
      COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.1 -O
              -mfmt=c -o ${GX_DRAW_F_SPV_INC}
              ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gx_draw_f.comp
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gx_draw_f.comp
      VERBATIM
      COMMENT "Compiling exact GX fused-program A--AD compute shader"
    )
    add_custom_target(gcn_gx_shaders DEPENDS ${GX_CLEAR_SPV_INC}
                                              ${GX_XFB_COPY_SPV_INC}
                                              ${GX_XFB_SUBGROUP_SPV_INC}
                                              ${GX_DRAW_F_SPV_INC})
    add_dependencies(gcn_boot gcn_gx_shaders)
    set_source_files_properties(src/gx_vulkan.c PROPERTIES
      OBJECT_DEPENDS "${GX_CLEAR_SPV_INC};${GX_XFB_COPY_SPV_INC};${GX_XFB_SUBGROUP_SPV_INC};${GX_DRAW_F_SPV_INC}")
    target_include_directories(gcn_boot PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_definitions(gcn_boot PRIVATE GCN_HAVE_VULKAN=1)
    target_link_libraries(gcn_boot PRIVATE Vulkan::Vulkan)
  endif()
  if(GCN_DSP_LTO AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(gcn_boot PRIVATE -flto=auto)
  endif()

  # E2 (perf campaign 2): profile-guided optimization. The boot is fully
  # deterministic, which makes it an ideal PGO training input, and the
  # block-exec plateau evidence (E1 flat, write-through micro-model negative)
  # points at code layout / branch weights / inlining decisions in the
  # 4096-label chunk megafunctions — exactly PGO's domain. Two-phase:
  #   cmake -DGCN_PGO=GEN  -> instrumented build (-fprofile-update=atomic:
  #                           GX-MT workers + the DSP worker write counters
  #                           concurrently)
  #   <run training boots: uniform 8M + derived 24M/56M>
  #   cmake -DGCN_PGO=USE  -> optimized build reading pgo-data/
  # Off (empty) by default — the plain build stays the dev-loop default; PGO
  # is a release-shaping step (adds a full extra compile + training run).
  set(GCN_PGO "" CACHE STRING "PGO for gcn_boot: GEN (instrument), USE (apply pgo-data/), empty = off")
  if(GCN_PGO STREQUAL "GEN")
    set(GCN_PGO_FLAGS -fprofile-generate=${CMAKE_BINARY_DIR}/pgo-data -fprofile-update=atomic)
  elseif(GCN_PGO STREQUAL "USE")
    # -fprofile-correction: tolerate the small counter races atomic updates
    # can still leave between threads; -Wno-missing-profile: TUs never
    # executed in training (cold debug paths) are expected, not an error.
    set(GCN_PGO_FLAGS -fprofile-use=${CMAKE_BINARY_DIR}/pgo-data -fprofile-correction -Wno-missing-profile)
  endif()
  if(DEFINED GCN_PGO_FLAGS)
    target_compile_options(gcn_boot PRIVATE ${GCN_PGO_FLAGS})
    target_link_options(gcn_boot PRIVATE ${GCN_PGO_FLAGS})
    target_compile_options(gcn_runtime_core PRIVATE ${GCN_PGO_FLAGS})
    target_compile_options(gcn_dsp_lle PRIVATE ${GCN_PGO_FLAGS})
  endif()
endif()
