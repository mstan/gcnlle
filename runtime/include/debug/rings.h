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
#define GCN_GPR_PROBE_RING_CAP (1u << 13) /* env-gated block-entry GPR snapshots */
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
    /* ROADMAP M5: DI (disc interface) events. Low-volume (a handful of
     * commands per boot/menu transition, mount/eject rarer still), so a plain
     * enum addition — same shape as GCN_EV_EXI_XFER — rather than a dedicated
     * ring (contrast the high-volume memcard ring, GCN_MEMCARD_RING_CAP). */
    GCN_EV_DI_CMD     = 7,  /* DI command issued (DICR TSTART write);
                             * detail = opcode (cmdbuf[0]>>24),
                             * aux = subcommand<<8 | resulting GCN_DI_INT_*    */
    GCN_EV_DI_MOUNT   = 8,  /* disc mounted(detail=1)/ejected(detail=0), via
                             * GCN_DISC at boot or the insert_disc/eject_disc
                             * debug commands                                 */
    GCN_EV_NATIVE_MISS = 9, /* native dispatch miss; detail=pc, aux=page CRC32 */
    GCN_EV_INTERP_EDGE = 10,/* interpreted edge; detail=from PC, aux=to PC     */
} GcnEventKind;

/* Bring the rings up (clears all three, resets the block index). Idempotent. */
void gcn_rings_init(void);

/* SNAPSHOT_RESUME (docs/SNAPSHOT_RESUME.md) restore-side: full reset of
 * EVERY ring including psq/watch (gcn_rings_init alone misses those — see
 * its own doc note). Always safe: nothing here is guest-visible. */
void gcn_rings_reset(void);

/* --- hot-path record calls (cheap; safe from any runtime path) --- */

/* One MMIO access. rw: 0=read 1=write. mapped: 1 if a device claimed the addr,
 * 0 if it hit the unmapped fallback. Stamped with the current block index. */
void gcn_ring_mmio(u32 pc, u32 addr, u32 value, u8 size, u8 rw, u8 mapped);

/* One retired recompiled block: entry PC plus the CPU-state fields needed to
 * reconstruct HOW control arrived there (an indirect branch via a corrupt
 * ctr/lr, or an exception vector via a corrupt srr0/msr) without re-running
 * anything — lr/ctr/srr0/srr1/msr are cheap register reads already resident
 * in CPUState, so recording them every block costs one extra cache line, not
 * a re-arm-and-hope trace. `exception` is ctx->exception AS SEEN ON ENTRY
 * (the pending flag the dispatcher clears right before this call) — non-zero
 * exactly when this block is the first block of an exception handler.
 * Advances the global block index. */
void gcn_ring_block_ex(u32 pc, u32 lr, u32 ctr, u32 srr0, u32 srr1, u32 msr,
                       u32 exception);

typedef struct CPUState CPUState;

/* Env-gated guest-PC probe. Set GCN_PROBE_PCS to a comma/semicolon/space
 * separated list of block-entry PCs; matching entries snapshot GPRs so TCP
 * tools can inspect call/return arguments without patching generated code.
 * Optional GCN_PROBE_MEM specs snapshot small RAM windows at the same instant,
 * avoiding post-run reads of reused guest structs. */
void gcn_ring_gpr_probe(CPUState* cpu, u32 pc, const u32 gpr[32], u32 lr,
                        u32 ctr, u32 cr, u32 xer, u32 fpscr);

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

/* stderr dump of the newest `max_entries` resident block-ring entries
 * (oldest->newest), each with its full lr/ctr/srr0/srr1/msr/exception —
 * the entry-chain evidence for "how did ctx->pc come to equal X": read
 * backward from the trigger block to see what block ran immediately before
 * it and what its lr/ctr/srr0 were. */
void gcn_ring_block_dump_stderr(int max_entries);

/* Exact, always-on dispatch-entry coverage. Unlike the rolling block ring,
 * these bits do not evict: MEM1/MEM2 executable aliases and the 2 MiB IPL
 * window remain queryable for the life of the process. This deliberately
 * records AOT dispatcher boundaries, not every instruction executed inside a
 * generated shard. */
int gcn_ring_pc_seen(u32 pc);

/* --- query: emit a bounded JSON tail (oldest->newest of the last N) --- *
 * Each returns bytes written into `out` (never exceeding cap). These run on the
 * main thread during a server pump, reading the same rings the hot path writes.
 *
 * mmio: if have_filter, only entries whose addr == addr_filter are emitted;
 *       if rw_filter >= 0, only that direction (0 read / 1 write). */
int gcn_ring_mmio_json(char* out, int cap, int max_entries,
                       u32 addr_filter, int have_filter, int rw_filter);
int gcn_ring_block_json(char* out, int cap, int max_entries);
int gcn_ring_gpr_probe_json(char* out, int cap, int max_entries);
int gcn_ring_event_json(char* out, int cap, int max_entries);
int gcn_ring_fifo_json(char* out, int cap, int max_entries);
int gcn_ring_memcard_json(char* out, int cap, int max_entries);

/* [gx-fifoprov] Newest gather burst whose destination (wptr & 0x1FFFFFFF)
 * equals `phys`. Fills the producing pc, seq, block stamp and the 32 raw
 * bytes; returns 0 if no resident burst targeted that address. Lets the GX
 * decoder compare the bytes it consumed against the bytes the guest actually
 * pushed to the same FIFO slot (guest-wrote-garbage vs drain-corruption). */
int gcn_ring_fifo_find(u32 phys, u64* seq, u32* pc, u64* block, u8 out[32]);

/* [gx-fifoprov] One paired-single load/store: the guest pc, ps0-lane EA and
 * the f32 bit patterns moved through both lanes (see the PsqEntry comment in
 * rings.c). Recorded by cpu_glue.c's ppc_psq_load/ppc_psq_store; dumped by
 * the GX corrupt-payload detector to separate RAM-was-already-corrupt from
 * corrupted-between-load-and-store. */
void gcn_ring_psq(u32 cia, u32 ea, u32 ps0, u32 ps1, u8 is_store);
void gcn_ring_psq_dump_stderr(int max_entries);

/* [gx-fifoprov] Print every resident psq ring entry whose ps0/ps1 bit
 * pattern equals one of `words` (zeros skipped) — the full producer chain of
 * a corrupt value: the store that first materialized it (its pc names the
 * computing routine) and every copy since. Oldest->newest, capped. */
void gcn_ring_psq_value_trace(const u32* words, int nwords, int max_print);

/* [gx-fifoprov] newest `max_entries` psq ring entries whose pc lies in
 * [pc_lo,pc_hi), printed oldest->newest — isolates one guest routine's
 * recent load/store traffic (e.g. PSMTXConcat's inputs at the corrupt call). */
void gcn_ring_psq_dump_pc_range(u32 pc_lo, u32 pc_hi, int max_entries);

/* [gx-fifoprov] locate the NEWEST psq store whose ea falls inside the
 * GCN_WATCH range and print +/-radius ring entries around it — the copy
 * loop's paired loads there name the corrupt source matrix exactly. */
void gcn_ring_psq_dump_around_watched_store(int radius);

/* [gx-fifoprov] guest-RAM write watch. Armed by GCN_WATCH=<lo>:<hi> (hex,
 * any alias; stored physical). gcn_watch_len stays 0 when unset, so the
 * store-path check below is one sub+compare. Call gcn_ring_watch_init once
 * at boot; stores report through gcn_ring_watch_hit (stderr, capped). */
extern u32 gcn_watch_lo, gcn_watch_len;
void gcn_ring_watch_init(void);
void gcn_ring_watch_hit(u32 ea, u64 value, u32 size, u32 cia);
void gcn_ring_watch_dump_stderr(int max_entries);
static inline void gcn_ring_watch_check(u32 ea, u64 value, u32 size, u32 cia) {
    u32 p = ea & 0x1FFFFFFFu;
    if (gcn_watch_len != 0u &&
        p < gcn_watch_lo + gcn_watch_len && p + size > gcn_watch_lo)
        gcn_ring_watch_hit(ea, value, size, cia);
}

/* Span variant for block writers (DMA engines, gather bursts, dcbz):
 * records one hit if [ea, ea+len) overlaps the watch range. `tag` labels
 * the writer in place of a guest pc (0xD5BD.. DSP DMA, 0xEC1E.. EXI DMA,
 * 0x6A6A.. gather burst, 0xDCB2.. dcbz) — see each call site. */
void gcn_ring_watch_hit_span(u32 ea, u32 len, u32 tag);
static inline void gcn_ring_watch_check_span(u32 ea, u32 len, u32 tag) {
    u32 p = ea & 0x1FFFFFFFu;
    if (gcn_watch_len != 0u &&
        p < gcn_watch_lo + gcn_watch_len && p + len > gcn_watch_lo)
        gcn_ring_watch_hit_span(ea, len, tag);
}

/* [gx-fifoprov] stderr dump of the newest `max_entries` event-ring entries
 * (interrupt/DMA edges with block stamps) — correlation data for a corrupt
 * FIFO payload (did an IRQ land while the guest was building/pushing it?). */
void gcn_ring_event_dump_stderr(int max_entries);

#ifdef __cplusplus
}
#endif

#endif /* GCN_DEBUG_RINGS_H */
