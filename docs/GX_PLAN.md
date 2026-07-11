# GX wall — investigation record + increment plan (M2)

Evidence-backed plan for modeling the GPU-side MMIO blocks. Produced from
Dolphin source study + the runtime's observed IPL traffic; every behavior
cited to the Dolphin file/lines it is transcribed from (PRINCIPLES: transcribe,
never invent).

## The decisive evidence

**PI INTMR ramps 0xF0 → 0xF8 → 0xFC → 0x1FC → 0x9FC** (all through
__OSUnmaskInterrupts at pc 0x81336544; the final step confirmed by the
extended oracle after the SI fixes). 0x1FC = DI|SI|EXI|AI|DSP|MI|VI; the
final **0x9FC adds INT_CAUSE_CP (0x800)** — the menu DOES enable the
Command Processor interrupt once its GX init completes. PE_TOKEN (0x200) /
PE_FINISH (0x400) stay masked throughout — no draw-done handshake.

**Where the runtime stalls:** it never issues the 0x1FC→0x9FC write. The
menu's GX/CP init path is gated before the CP unmask — the runtime idles in
the PI INTSR/INTMR read loop (pc 0x81336680/0x81336694) each frame while
Dolphin proceeds. Most plausible gate: CP/PE register READ-BACKS currently
hit the unmapped fallback (return 0) inside the GX init path. Consequently:

- (a) Register files + the real gather-pipe write path are the correct next
  increment, and with the CP interrupt enabled at 0x9FC, cp.c must include
  the CPU-side watermark/status evaluation (SetCPStatusFromCPU,
  CommandProcessor.cpp:442-479): hi/lo-watermark conditions are computed
  purely from FIFO pointer state the CPU side owns, so raising INT_CAUSE_CP
  from them is REAL behavior, not fake — no GPU consumer needed.
- (b) Pixels DO require the interpreter: the XFB is only written by the GPU
  executing an EFB→XFB copy from the FIFO stream (the same processing that
  raises PE token/finish — Dolphin PixelEngine.cpp:229-252). Until then an
  all-black screenshot is the correct output.

## Register maps (authoritative sources)

- **CP 0xCC000000** (VideoCommon/CommandProcessor.h:56-153, .cpp:128-407,
  552-613): STATUS 0x00 (RO, computed from FIFO pointer state —
  SetCpStatusRegister .cpp:552-564; never fake "idle"), CTRL 0x02 (RW,
  val&0x3F), CLEAR 0x04 (W, Dolphin no-ops .cpp:597-601), PERF_SELECT 0x06,
  UNK 0x0A, FIFO base/end/hi-wm/lo-wm/rw-distance/wptr/rptr/bp at
  0x20-0x3E as 16-bit lo/hi pairs (LO write-masked 0xffe0, HI masked to
  phys 0x03ff on GCN — .cpp:130-163), metrics constants 0x40-0x64 (all 0
  except CLKS_PER_VTX_OUT 0x64 = 4 — .cpp:165-191). CP→PI INT_CAUSE_CP only
  from watermark/breakpoint gated by GPReadEnable (.cpp:409-550) — dormant
  until a FIFO consumer exists.
- **PE 0xCC001000** (VideoCommon/PixelEngine.h:29-218, .cpp:27-252): ZCONF
  0x00, ALPHACONF 0x02, DSTALPHACONF 0x04, ALPHAMODE 0x06, ALPHAREAD 0x08
  (all RW direct), CTRL 0x0A (bits: token_enable:0, finish_enable:1,
  token:2 W-only-ack, finish:3 W-only-ack; reads return control with bits
  2-3 forced 0 — .cpp:125-144), TOKEN 0x0E RO. PE→PI INT_CAUSE_PE_TOKEN
  0x200 / PE_FINISH 0x400 asserted ONLY from SetToken/SetFinish (video
  backend, .cpp:229-252) — entry points exist for the future parser, never
  from register writes (that would be fake-the-answer).
- **MI 0xCC004000** (Core/HW/MemoryInterface.cpp:17-124): pure u16 R/W
  register file, memset init, zero side effects; 32-bit access splits into
  16-bit halves. Observed IPL write: 0x1C = MI_IRQMASK.
- **Gather pipe 0xCC008000, 32 bytes** (Core/HW/GPFifo.h:19-22,
  .cpp:85-131): accumulate writes across the whole 0x20 window (the
  recompiler emits stores at +0/+4 — treat any offset as "append bytes");
  on each full 32-byte burst, write the bytes into guest RAM at PI
  FIFO_WPTR, advance WPTR wrapping END→BASE (GPFifo.cpp:93-104), and when
  CP GPLinkEnable, mirror WPTR into the CP write pointer and add 32 to
  CPReadWriteDistance (CommandProcessor.cpp:363-386). Also mirror each
  burst into an always-on FIFO recorder ring (ROADMAP M2: "recorder
  first") for the packet inventory that scopes the interpreter.
- **PI FIFO regs** (ProcessorInterface.h:49-67, .cpp:91-119): our pi.c
  already stores BASE/END/WPTR as plain registers; add PI_FIFO_RESET
  (0x18) handling to reset the gather-pipe staging (GXAbortFrame).

## Loud-divergence deferrals (documented, never silently faked)

1. No FIFO interpreter / EFB / XFB copy → no pixels yet (M2 deliverable).
2. No FIFO consumer: the read pointer does NOT advance; CPReadWriteDistance
   climbs. Do NOT auto-advance the read pointer. If the guest ever polls CP
   STATUS for idle or nears the hi-watermark, it diverges loudly — that is
   the signal the consumer is due (Dolphin ASSERTs on overflow,
   CommandProcessor.cpp:390-394).
3. PE token/finish + CP interrupt lines wired but dormant until the parser
   raises them — faithful (hardware only fires them from processed
   commands), and the menu masks them anyway.

## File plan

`mi.c/mi.h`, `pe.c/pe.h`, `cp.c/cp.h`, `gp.c/gp.h` following the vi.c/di.c
device pattern (register file + irq callback + boot.c registration +
CMakeLists). gp needs {pi, cp} context via its user pointer. New FIFO
recorder ring in debug/rings.h + a debug-server dump command.
