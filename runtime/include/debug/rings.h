/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gcnrecomp runtime — always-on diagnostic ring buffers.
 *
 * PRINCIPLES: observability is a standing, always-on surface. These rings
 * record continuously from runtime start (Release too; bounded by eviction).
 * Probes QUERY a window; they never arm-then-run-then-hope. The recording
 * calls are cheap bounded-array writes on the hot path, so they stay compiled
 * in every build. Only the TCP query surface (debug_server.c) is env-gated.
 *
 * Four event classes the ROADMAP calls for, implemented as three rings:
 *   - MMIO ring       every access through the device-dispatch bus (mmio.c).
 *                     The "device-write ring" is this ring filtered to rw==1.
 *   - block/PC ring   the entry PC of every retired recompiled block (dispatch.c).
 *   - event ring      interrupt/DMA/DSP/EXI edges (recorded at their source sites).
 *
 * Single-writer model: every record call and every JSON dump runs on the main
 * dispatch thread (the server is pumped from that same loop), so no locking is
 * needed. A monotonic block index (advanced by gcn_ring_block) timestamps MMIO
 * and event entries so a query can correlate the three timelines.
 */
#ifndef GCN_DEBUG_RINGS_H
#define GCN_DEBUG_RINGS_H

#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Capacities (power-of-two so index masking is a single AND). Sized so a few
 * seconds of boot activity stay resident: MMIO/block ~262K entries, events 64K. */
#define GCN_MMIO_RING_CAP  (1u << 18)
#define GCN_BLOCK_RING_CAP (1u << 18)
#define GCN_EVENT_RING_CAP (1u << 16)
#define GCN_FIFO_RING_CAP  (1u << 16)   /* GX gather-pipe bursts (32 bytes each) */
/* Memcard transaction ring (ROADMAP M4). EXI memory-card traffic is SPARSE
 * (presence poll + directory read + save read/write) compared to the VI/SI/GX
 * flood, so a small dedicated ring covers a very long wall-clock window — the
 * one-time directory-read/write burst stays resident and queryable long after
 * it happened, exactly the always-on capture the MMIO ring is too shallow to
 * provide for card traffic (see _work/M4_CAPTURE.md: the burst evicts from the
 * 256K MMIO ring in a few menu seconds). One entry per card EXI transaction. */
#define GCN_MEMCARD_RING_CAP (1u << 13)  /* 8192 card transactions */

/* Event kinds recorded in the event ring (interrupt/DMA/DSP/EXI edges). */
typedef enum {
    GCN_EV_NONE       = 0,
    GCN_EV_IRQ_RAISE  = 1,  /* a device raised an interrupt line; detail = source  */
    GCN_EV_IRQ_CLEAR  = 2,  /* an interrupt line was acknowledged/cleared          */
    GCN_EV_DSP_MAIL   = 3,  /* DSP<->CPU mailbox post; detail = 0 dsp->cpu/1 cpu->dsp, aux = mail hi */
    GCN_EV_ARAM_DMA   = 4,  /* Flipper AR DMA kick; detail = to_aram, aux = length  */
    GCN_EV_DSP_DMA    = 5,  /* DSP DMA (IDMA/DDMA); detail = ctl, aux = length       */
    GCN_EV_EXI_XFER   = 6,  /* EXI transfer; detail = channel, aux = payload         */
} GcnEventKind;

/* Bring the rings up (clears all three, resets the block index). Idempotent. */
void gcn_rings_init(void);

/* --- hot-path record calls (cheap; safe from any runtime path) --- */

/* One MMIO access. rw: 0=read 1=write. mapped: 1 if a device claimed the addr,
 * 0 if it hit the unmapped fallback. Stamped with the current block index. */
void gcn_ring_mmio(u32 pc, u32 addr, u32 value, u8 size, u8 rw, u8 mapped);

/* One retired recompiled block (its entry PC). Advances the global block index. */
void gcn_ring_block(u32 pc);

/* One device/timeline edge (see GcnEventKind). Stamped with the block index. */
void gcn_ring_event(u16 kind, u32 detail, u32 aux, u32 pc);

/* One 32-byte GX gather-pipe burst: the producing pc, the FIFO write pointer it
 * was DMA'd to, and the 32 raw bytes. Stamped with the block index. This is
 * ROADMAP M2's "GX FIFO recorder" — the packet inventory for the interpreter. */
void gcn_ring_fifo(u32 pc, u32 wptr, const u8* data32);

/* One EXI memory-card transaction (ROADMAP M4). Recorded from exi.c's card
 * path for every immediate/DMA transfer routed to a memcard device, so the
 * sparse card command stream stays continuously queryable (the MMIO ring is
 * too shallow to hold it under menu-phase traffic). Fields:
 *   channel  0=slot A (ch0 dev0), 1=slot B (ch1 dev0)
 *   cs       chip-select one-hot at transfer time (1 = card selected)
 *   command  the memcard's decoded command opcode (mc->command) at transfer end
 *   rw       0=read (device->CPU) 1=write (CPU->device)
 *   dma      0=immediate 1=DMA
 *   address  the card-side byte offset (mc->address) after the transfer
 *   length   transfer length in bytes (imm: 1-4; dma: dma_length)
 *   data     immediate data register value (imm) / guest DMA address (dma) */
void gcn_ring_memcard(u32 pc, u8 channel, u8 cs, u8 command, u8 rw, u8 dma,
                      u32 address, u32 length, u32 data);

/* Current monotonic block index (number of blocks retired so far). */
u64  gcn_ring_block_index(void);

/* --- query: emit a bounded JSON tail (oldest->newest of the last N) --- *
 * Each returns bytes written into `out` (never exceeding cap). These run on the
 * main thread during a server pump, reading the same rings the hot path writes.
 *
 * mmio: if have_filter, only entries whose addr == addr_filter are emitted;
 *       if rw_filter >= 0, only that direction (0 read / 1 write). */
int gcn_ring_mmio_json(char* out, int cap, int max_entries,
                       u32 addr_filter, int have_filter, int rw_filter);
int gcn_ring_block_json(char* out, int cap, int max_entries);
int gcn_ring_event_json(char* out, int cap, int max_entries);
int gcn_ring_fifo_json(char* out, int cap, int max_entries);
int gcn_ring_memcard_json(char* out, int cap, int max_entries);

#ifdef __cplusplus
}
#endif

#endif /* GCN_DEBUG_RINGS_H */
