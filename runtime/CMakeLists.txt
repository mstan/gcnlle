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
# Device modules to grow here (cf. psxrecomp runtime/src, reshine runtime/src,
# and GXRuntime's C device models which we harvest under GPL-3.0):
#   memory.c    flat big-endian MEM1 (24MB) + MEM2 stub, bus primitives
#   cpu_glue.c  PPCContext <-> recompiled code entry/exit, exceptions, rfi
#   exi.c       EXI channels: RTC (date/time) + SRAM (settings) + memory cards
#   vi.c        Video Interface: framebuffer scanout / XFB -> host window
#   gx.c        Flipper GX FIFO command processor -> host renderer
#   dsp.c/ai.c  DSP + Audio Interface (boot chime, menu sound)
#   di.c        Disc Interface (disc-load screen; stubbed until disc phase)
#   si.c        Serial Interface (controller input in the menu)
#   dispatch.c  block dispatch loop + host-intercept table
#
# For now this target is a skeleton that compiles a placeholder so the tree
# builds green end-to-end. Replace src/main.c as real modules land.
# ---------------------------------------------------------------------------

add_executable(gcn_runtime
  src/main.c
)
target_include_directories(gcn_runtime PRIVATE include)
