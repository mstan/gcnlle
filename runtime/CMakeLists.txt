cmake_minimum_required(VERSION 3.16)
project(gcnrecomp_runtime C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

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
  src/seed.c
  src/trace.c
)
target_include_directories(gcn_runtime_core PUBLIC include)
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
add_test(NAME seed_contract COMMAND $<TARGET_FILE:test_seed>)

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
  add_executable(gcn_boot
    src/boot.c
    src/dispatch.c
    src/mmio.c
    src/exi.c
    src/si.c
    generated/generated.c
    ${GCN_GENERATED_CHUNKS}
  )
  # generated/ for "generated.h"; include/ for cpu|memory|seed|dispatch headers.
  target_include_directories(gcn_boot PRIVATE include generated)
  target_link_libraries(gcn_boot PRIVATE gcn_runtime_core)
endif()
