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
#include "gp/gp.h"   /* GcnGp — gcn_gx_confirm_drained's gp->count check      */
#include <stddef.h>  /* size_t — gcn_gx_confirm_drained's why/why_size        */

/* Nominal bytes drained per tick. Fifo.cpp meters GPU work in cycles; we have no
 * cycle-accurate GPU model, so we drain a fixed budget per block. Exact GPU
 * timing is absorbed by the poll-collapsed value+order oracle diff (same
 * rationale as the VI beam rate in vi.h). 1024 = 32 gather-pipe chunks. */
#define GCN_GX_DRAIN_BYTES_PER_TICK  1024u

/* A scanner carry may re-enter the asynchronous producer only when the next
 * complete 32-byte gather can be appended without overflowing its fixed
 * buffer. Checking only carry_len <= capacity is insufficient: a 97-byte
 * Wind Waker primitive tail plus the next 32-byte gather poisoned the
 * pipeline permanently. */
static inline int gcn_gx_pipeline_carry_can_resume(u32 carry_len,
                                                   u32 capacity,
                                                   u32 gather_size) {
    return gather_size <= capacity && carry_len <= capacity - gather_size;
}

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

/* SNAPSHOT_RESUME SAVE-side accessors: the 256-entry BP register file and the
 * XF matrix/light-memory-plus-registers block (gcn_gx_xf_words() entries,
 * currently 0x1058), read-only. Always valid (both back the s_gx singleton,
 * static storage). Caller must have already confirmed drain (see
 * gcn_gx_confirm_drained) before treating these as a stable snapshot. */
const u32* gcn_gx_bp(void);
const u32* gcn_gx_xf(void);
u32 gcn_gx_xf_words(void);

/* SNAPSHOT_RESUME restore-side load-mirrors of the three accessors above. */
void gcn_gx_set_bp(const u32* bp);
void gcn_gx_set_xf(const u32* xf, u32 words);
void gcn_gx_set_tmem(const u8* data, u32 len);

/* G3 pipeline join (default on; GCN_GX_PIPELINE=0 disables, gx.c): block until the worker has
 * decoded every FIFO byte pushed so far. No-op when the pipeline is off.
 * Call before any gate-visible read of GX-produced state that PE fences
 * don't already cover: end-of-run (pre-GCN_MEM_DUMP), debug-server
 * screenshots, the PI fifo-reset hook. */
void gcn_gx_pipeline_drain(void);

/* SNAPSHOT_RESUME (docs/SNAPSHOT_RESUME.md) SAVE-side hard drain-assert.
 * Caller MUST call gcn_gx_pipeline_drain() first — see gx.c for the exact
 * conditions checked and why this only checks rather than drives-to-empty. */
int gcn_gx_confirm_drained(GcnGp* gp, GcnCp* cp, char* why, size_t why_size);

/* Host scanout snapshots and EFB->XFB materialization share guest MEM1. The
 * guest commonly reuses one XFB address, so a VI field can otherwise copy
 * the buffer while the GX worker is replacing its rows. These guards make
 * each host snapshot observe one complete old or new XFB without draining
 * unrelated queued GX work. Writers are the software and resident-Vulkan
 * EFB-copy paths; VI holds the shared side only for its synchronous mailbox
 * copy. */
void gcn_gx_xfb_read_begin(void);
void gcn_gx_xfb_read_end(void);
void gcn_gx_xfb_write_begin(void);
void gcn_gx_xfb_write_end(void);

/* GCN_GX_XFB_HASH=1: route-level content hash chain over every XFB
 * publication (default off, zero overhead: one branch per publication).
 * Call gcn_gx_xfb_hash_feed() with the exact destination region an EFB->XFB
 * copy just wrote (same bytes VI/scanout will read), then
 * gcn_gx_xfb_hash_publish_done() once per completed copy. Two runs of the
 * same route that print the same shutdown chain wrote byte-identical XFB
 * content, regardless of which internal path (software raster vs Vulkan
 * resident, fused vs unfused shaders, SIMD vs scalar) produced it. */
void gcn_gx_xfb_hash_feed(const u8* base, u32 stride, u32 row_bytes, u32 rows);
void gcn_gx_xfb_hash_publish_done(void);
void gcn_gx_xfb_dump_feed(const u8* base, u32 stride, u32 row_bytes, u32 rows);
u64 gcn_gx_xfb_pub_count(void);

/* SNAPSHOT_RESUME pass C: get/set the cumulative chain + publication count
 * (see gx.c's doc comment above gcn_gx_xfb_hash_get_state). Restore must
 * call the setter (when GCN_GX_XFB_HASH=1) BEFORE any further XFB
 * publication happens in the resumed process, or the chain silently
 * restarts from the FNV-1a offset basis instead of continuing. */
void gcn_gx_xfb_hash_get_state(u64* chain, u64* pubs);
void gcn_gx_xfb_hash_set_state(u64 chain, u64 pubs);

/* SNAPSHOT_RESUME pass C: load-mirror of gcn_gx_frame_count, so the resumed
 * process's "completed N IPL frames" bookkeeping (and anything keyed off
 * it, e.g. gx_raster's frame-anomaly census) continues the captured count
 * instead of restarting at 0. */
void gcn_gx_set_frame_count(u64 frames);

/* Completed-frame counter (accepted GXSetDrawDone writes). Provenance stamp
 * for diagnostics that need to name "which frame" (e.g. gx_raster's
 * big-triangle census). */
u64 gcn_gx_frame_count(void);

/* Guest address of the display list currently executing (0 = top-level). */
u32 gcn_gx_current_dl(void);

/* [gx-xfaudit] Dump the always-on XF/BP write-audit rings (last 64 XF /
 * MATINDEX and 48 BP register writes, each stamped with frame + source DL).
 * Fired by gx_raster's wrong-matrix trigger (a wall-scale position matrix
 * applied while executing the cube DL) to name which of write-skipped /
 * write-corrupted / wrong-index produced the IPL flood frames. Decode-thread
 * only, same single-writer model as the draw log. */
void gcn_gx_state_audit_dump(void);

/* Monotonic producer publication point. It advances only after
 * GXSetDrawDone has flushed/materialized every preceding EFB->XFB copy, so
 * VI can distinguish a finished guest frame from intermediate copies made
 * while constructing it. */
u64 gcn_gx_xfb_generation(void);

/* Drain, stop, and join the default-on GX worker before guest RAM/device
 * teardown. No-op when the synchronous fallback is selected. */
void gcn_gx_pipeline_shutdown(void);

#endif /* GCN_GX_GX_H */
