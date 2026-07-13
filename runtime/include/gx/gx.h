/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GX FIFO consumer — the GPU-side command drain for the Flipper GX pipe.
 *
 * This is a *device-less* module: it is not registered on the MMIO bus (the CP
 * register file at 0xCC000000 and the gather pipe at 0xCC008000 already are, in
 * cp.c / gp.c). Instead it is ticked once per retired block from the dispatch
 * loop — exactly like gcn_vi_tick / gcn_di_tick — and pulls command bytes out of
 * the guest-RAM FIFO ring that gp.c fills, decoding + executing them so the GPU
 * side of the FIFO handshake becomes real.
 *
 * Every behavior below is transcribed from Dolphin — our independent oracle —
 * with the file/lines it comes from, never invented (PRINCIPLES: transcribe):
 *   FIFO drain (single-core RunGpuLoop) : VideoCommon/Fifo.cpp:286-354 (read a
 *                                         32-byte chunk at CPReadPointer, run the
 *                                         opcode decoder over the accumulated
 *                                         video buffer, advance the read pointer
 *                                         wrapping CPEnd->CPBase, subtract 32
 *                                         from CPReadWriteDistance, re-run
 *                                         SetCPStatusFromGPU). Simplified to a
 *                                         synchronous drain called per block.
 *   opcode set + payload sizes          : VideoCommon/OpcodeDecoding.h:24-42 +
 *                                         detail::RunCommand:125-253
 *   CP state / vertex sizing            : VideoCommon/CPMemory.{h,cpp}
 *                                         (LoadCPReg) + VertexLoaderBase.cpp
 *                                         GetVertexSize:175-203 + the per-
 *                                         component GetSize tables in
 *                                         VertexLoader_{Position,Normal,Color,
 *                                         TextCoord}.h
 *   XF register memory                  : VideoCommon/XFMemory.h:193-240
 *                                         (matrix/light memory 0x000-0x67f +
 *                                         registers 0x1000-0x1057)
 *   BP register file + side effects     : VideoCommon/BPMemory.h:29-110 (offsets)
 *                                         + BPStructs.cpp BPWritten:55-396
 *                                         (SETDRAWDONE / PE_TOKEN / EFB copy)
 *   XFB YUY2 encode                     : VideoBackends/Software/SWEfbInterface.cpp
 *                                         ConvertColorToYUV:546-562 +
 *                                         EncodeXFB:577-664; clear-color unpack
 *                                         VideoBackends/Software/EfbCopy.cpp:18-19
 *
 * SCOPE THIS INCREMENT (ROADMAP M2 first half): command drain + CP/XF/BP state +
 * the EFB copy-CLEAR to the XFB. Geometry rasterization is the NEXT increment:
 * draw commands (0x80-0xBF) have their vertex payload SKIPPED by the exact byte
 * count computed from VCD/VAT — a wrong count desyncs the stream, so if the size
 * cannot be computed exactly the drain ERRORS loudly and stops rather than
 * guessing. Everything unimplemented is LOUD (one-time stderr per opcode /
 * register), never silently skipped.
 *
 * DELIBERATELY DEFERRED (diverges loudly if exercised, never silently faked):
 *   - Geometry: draws are decoded + skipped, one-time loud per primitive type.
 *   - Real EFB->XFB content copy (needs a rasterizer): destination left
 *     untouched, one-time loud. Honest stale/black output beats fake content.
 *   - EFB->texture copy (copy_to_xfb=0): one-time loud no-op.
 *   - LOAD_INDX_A..D: one-time loud (reported if the IPL uses them).
 */
#ifndef GCN_GX_GX_H
#define GCN_GX_GX_H

#include "cpu/cpu.h"
#include "cp/cp.h"
#include "pe/pe.h"

/* Nominal bytes drained per tick. Fifo.cpp meters GPU work in cycles; we have no
 * cycle-accurate GPU model, so we drain a fixed budget per block. Exact GPU
 * timing is absorbed by the poll-collapsed value+order oracle diff (same
 * rationale as the VI beam rate in vi.h). 1024 = 32 gather-pipe chunks. */
#define GCN_GX_DRAIN_BYTES_PER_TICK  1024u

/* Bring the GX consumer up. Needs the guest CPU (for FIFO/DL/XFB guest-RAM
 * access), the CP register file (read pointer / distance / enables), and the PE
 * (token/finish latches raised by SETDRAWDONE / PE_TOKEN commands). */
void gcn_gx_init(CPUState* cpu, GcnCp* cp, GcnPe* pe);

/* Drain up to GCN_GX_DRAIN_BYTES_PER_TICK from the FIFO and execute whole
 * commands. Called once per retired block by the dispatch loop, after
 * gcn_di_tick. `cycles` is accepted for signature symmetry with the other ticks
 * (the drain budget is fixed, not cycle-derived). No-op until CP GPReadEnable is
 * set and CPReadWriteDistance > 0. */
void gcn_gx_tick(u32 cycles);

/* Modeled TMEM (1MB). LOADTLUT1 BP writes copy palettes from guest RAM into
 * it; gx_raster.c's paletted texture decode (C4/C8/C14X2) reads them back.
 * Always valid (static storage), contents all-zero until the first TLUT load. */
const u8* gcn_gx_tmem(void);

/* G3 pipeline join (GCN_GX_PIPELINE=1, gx.c): block until the worker has
 * decoded every FIFO byte pushed so far. No-op when the pipeline is off.
 * Call before any gate-visible read of GX-produced state that PE fences
 * don't already cover: end-of-run (pre-GCN_MEM_DUMP), debug-server
 * screenshots, the PI fifo-reset hook. */
void gcn_gx_pipeline_drain(void);

#endif /* GCN_GX_GX_H */
