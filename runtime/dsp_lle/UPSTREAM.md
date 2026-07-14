# Vendored DSP LLE core

`dsp/` is Dolphin's GameCube/Wii DSP low-level-emulation **interpreter** core,
vendored from `oracle/dolphin/Source/Core/Core/DSP/` (GPL-2.0-or-later, which we
carry under this project's GPL-3.0). It runs the real DSP firmware
(`bios/dsp_rom.bin` IROM + `bios/dsp_coef.bin` DROM) plus the ucode the IPL
uploads — i.e. genuine LLE of the DSP, not an HLE stub.

Included: DSPCore, DSPTables, DSPHWInterface, DSPAccelerator, DSPMemoryMap,
DSPStacks, DSPAnalyzer, DSPCaptureLogger, and `Interpreter/` (Int{Arithmetic,
Multiplier,ExtOps,Branch,LoadStore,Tables,CCUtil}). Excluded: `Jit/` (we use the
interpreter — deterministic), Assembler/Disassembler (not needed to execute).

The release/dev build uses GCC LTO on this small C++ library only
(`GCN_DSP_LTO=ON`, disable-able). It lets the compiler inline between the
interpreter's opcode-family translation units; it is not a DSP JIT and does
not skip or replace firmware instructions.

Integration (net-new, ours):
- `host.cpp` implements the `DSP::Host::*` callbacks against the runtime's MEM1
  (ReadHostMemory/WriteHostMemory/DMAToDSP/DMAFromDSP) and PI (InterruptRequest).
- `compat/` provides minimal shims for the handful of Dolphin Common/Core
  headers the interpreter pulls in (CommonTypes, Logging, MemoryUtil, Event,
  Assert, plus a 4-call-site SystemTimers/CoreTiming timing shim for the
  DSPInitCode window).
- `dsp_lle_c.{h,cpp}` exposes a C API (init/reset/run/mailbox/control-reg) that
  `runtime/src/dsp.c` wires the 0xCC005000 MMIO block to.

The DSP AR/AI DMA engine (0xCC005020+) stays in `dsp.c` — that is the Flipper
ARAM DMA (CPU-side hardware), not the DSP core.

Validation: our harvested core IS Dolphin's reference DSP LLE running the real
firmware, so it is ground truth by construction. Dolphin-with-DSP-LLE is
impractical as a headless oracle (cycle-accurate ARAM clear crawls), so we
validate the boot handshake against the HLE oracle (values that agree confirm
both) and cross-check DSP-region values against IROM/ucode disassembly.
