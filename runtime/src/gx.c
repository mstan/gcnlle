/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GX FIFO consumer (impl). See include/gx/gx.h for scope and the exact Dolphin
 * files/lines every behavior is transcribed from. This is the GPU side of the
 * FIFO handshake: it drains command bytes gp.c parked in the guest-RAM FIFO
 * ring, decodes + executes whole commands, and advances the CP read pointer /
 * read-write distance exactly as Dolphin's single-core RunGpuLoop does.
 */
#include "gx/gx.h"
#include "gx/gx_raster.h"
#include "debug/rings.h"

#include <stdio.h>
#include <stdlib.h>      /* getenv — GCN_GX_STATS cached read, see below */
#include <string.h>
#include <x86intrin.h>   /* __rdtsc — GCN_GX_STATS attribution only */
#include <windows.h>     /* G3 pipeline worker: CreateThread +
                          * WaitOnAddress/WakeByAddressAll (same idiom as
                          * gx_raster.c's GX-MT pool and dsp.c's worker). */
#include <immintrin.h>   /* _mm_pause in the pipeline spin phases */

/* Staging ("video buffer") capacity. Fifo.cpp keeps a linear buffer that
 * accumulates 32-byte chunks and is consumed a whole-command-at-a-time; leftover
 * partial commands persist to the next chunk. 256 KiB comfortably holds any
 * single command the IPL emits (the largest is a bounded XF/BP/DL load — draws
 * are skipped, not buffered whole beyond their payload). Overflow is loud. */
#define GX_BUF_CAP  (256u * 1024u)

/* CP LoadCPReg sub-command classes (CPMemory.h:28-55). */
#define GX_CP_MATINDEX_A   0x30u
#define GX_CP_MATINDEX_B   0x40u
#define GX_CP_VCD_LO       0x50u
#define GX_CP_VCD_HI       0x60u
#define GX_CP_VAT_REG_A    0x70u
#define GX_CP_VAT_REG_B    0x80u
#define GX_CP_VAT_REG_C    0x90u
#define GX_CP_ARRAY_BASE   0xA0u
#define GX_CP_ARRAY_STRIDE 0xB0u
#define GX_CP_COMMAND_MASK 0xF0u

/* Opcodes (OpcodeDecoding.h:24-42). */
#define GX_OP_NOP              0x00u
#define GX_OP_LOAD_CP_REG      0x08u
#define GX_OP_LOAD_XF_REG      0x10u
#define GX_OP_LOAD_INDX_A      0x20u
#define GX_OP_LOAD_INDX_B      0x28u
#define GX_OP_LOAD_INDX_C      0x30u
#define GX_OP_LOAD_INDX_D      0x38u
#define GX_OP_CALL_DL          0x40u
#define GX_OP_UNKNOWN_METRICS  0x44u
#define GX_OP_INVL_VC          0x48u
#define GX_OP_LOAD_BP_REG      0x61u
#define GX_OP_PRIM_START       0x80u
#define GX_OP_PRIM_END         0xBFu

/* BP registers with side effects (BPMemory.h:52-64). */
#define GX_BP_LOADTLUT0        0x64u   /* TLUT source: guest addr >> 5 */
#define GX_BP_LOADTLUT1        0x65u   /* TLUT dest: tmem_addr | line_count<<10 */
#define GX_BP_SETDRAWDONE      0x45u
#define GX_BP_PE_TOKEN_ID      0x47u
#define GX_BP_PE_TOKEN_INT_ID  0x48u
#define GX_BP_EFB_TL           0x49u
#define GX_BP_EFB_WH           0x4Au
#define GX_BP_EFB_ADDR         0x4Bu
#define GX_BP_EFB_STRIDE       0x4Du
#define GX_BP_COPYYSCALE       0x4Eu
#define GX_BP_CLEAR_AR         0x4Fu
#define GX_BP_CLEAR_GB         0x50u
#define GX_BP_CLEAR_Z          0x51u
#define GX_BP_TRIGGER_EFB_COPY 0x52u

#define GX_XF_REGISTERS_START  0x1000u
#define GX_XF_REGISTERS_END    0x1058u   /* XFMemory.h:240 (register region end) */
#define GX_XF_MEM_WORDS        0x1058u   /* covers matrix/light mem + registers  */

/* CP-state mirror needed to size + load vertices (CPMemory.h CPState:744-761).
 * The type is defined in gx_raster.h so the rasterizer's vertex loader shares
 * exactly this layout; all fields are stored faithfully. */

typedef struct {
    CPUState* cpu;
    GcnCp*    cp;
    GcnPe*    pe;

    GxCpState cpst;
    u32 bp[256];                     /* BPMemory register file */
    u32 xf[GX_XF_MEM_WORDS];         /* XF matrix/light memory + registers */

    u8  buf[GX_BUF_CAP];             /* linear staging (Fifo.cpp video buffer) */
    u32 buf_len;
    int dl_depth;                    /* display-list recursion guard */
    u32 cur_dl_addr;                 /* guest addr of the DL currently executing, else 0
                                      * (context for one-time draw logs — which DL a
                                      * primitive lives in, not just the gather-pipe pc) */

    /* one-time-log bitsets (report which regs/opcodes the IPL exercised) */
    u8 seen_opcode[256];
    u8 seen_bp[256];
    u8 seen_cp[256];
    u8 seen_prim[8];
    u8 seen_xf_reg[0x60];            /* 0x1000..0x105F register loads */
    u8 seen_xf_mem;                  /* any matrix/light-memory load  */
    /* dedicated EFB-copy log flags (must NOT reuse seen_bp[] — TEV_KSEL occupies
     * BP 0xF6..0xFD and BP_MASK is 0xFE, so those slots are real guest regs) */
    u8 seen_efb_clear, seen_efb_content, seen_efb_tex, seen_efb_zero, seen_efb_oob;
} GcnGx;

static GcnGx s_gx;

/* ============================================================================
 * GCN_GX_STATS=1: rdtsc-based wall attribution WITHIN gcn_gx_tick, split
 * across the four dominant sub-paths dispatch-stats (GCN_DISPATCH_STATS,
 * dispatch.c) proved gx_tick itself dominates:
 *   FIFO   - the 32-byte chunk memcpy + gcn_cp_gpu_consume_chunk + the
 *            leftover memmove (gcn_gx_tick, guest-RAM <-> staging buffer)
 *   DECODE - gx_run/gx_run_command: CP/XF/BP loads, NOP runs, CALL_DL
 *            bookkeeping — i.e. gx_run's own wall time with the nested
 *            DRAW/EFB time (below) subtracted back out, so the four buckets
 *            partition gx_run's tree exactly instead of double-counting.
 *   DRAW   - gx_raster_draw (software rasterizer), timed at its call site in
 *            gx_run_command
 *   EFB    - gx_raster_efb_copy, timed at its call site in gx_on_bp
 * Plus event counters over the same window (chunks/commands/draws/verts/
 * DL calls/EFB copies) so a % share can be read alongside "how much work".
 *
 * Same style as GCN_DISPATCH_STATS: one cached getenv (s_gxstats, lazily
 * resolved to 0/1 on the first tick), then straight-line `if (s_gxstats)`
 * guards at each timed site. Off by default; the only per-tick/per-command
 * cost when off is those untaken branches — no rdtsc calls, no counter
 * writes, identical GX behavior either way. */
static int s_gxstats = -1;
enum { GX_STAT_FIFO = 0, GX_STAT_DECODE, GX_STAT_DRAW, GX_STAT_EFB, GX_STAT_N };
static u64 s_gx_tsc[GX_STAT_N];
static u64 s_gx_ticks;      /* gcn_gx_tick calls that passed the CP/enable gate */
static u64 s_gx_chunks;     /* 32-byte FIFO chunks drained */
static u64 s_gx_commands;   /* gx_run_command invocations that consumed >0 bytes */
static u64 s_gx_draws;      /* GX_OP_PRIM_* commands rasterized */
static u64 s_gx_verts;      /* total vertices across those draws */
static u64 s_gx_dlcalls;    /* CALL_DL commands actually executed (non-recursive) */
static u64 s_gx_efbcopies;  /* GX_BP_TRIGGER_EFB_COPY writes */

/* Log a first-occurrence once; returns 1 the first time a flag is raised. */
static int note_once(u8* flag) {
    if (*flag) return 0;
    *flag = 1;
    return 1;
}

/* ============================================================================
 * G3: CPU/GX pipeline (GCN_GX_PIPELINE=1, perf campaign 2 — see
 * docs/PERF_CAMPAIGN_2.md and the recon notes therein).
 *
 * The split that keeps this exact: the CP FIFO STATE MACHINE is pure
 * arithmetic (32-byte chunks, the per-tick drain budget, the breakpoint
 * gate — no command decode involved), so it STAYS on the CPU thread inside
 * gcn_gx_tick, byte-identical to the synchronous design: every CP register
 * the guest can read (STATUS/RPTR/WPTR/RW_DIST, watermark interrupt state)
 * is computed from CPU-side state at exactly the synchronous positions,
 * with no cross-thread coordination at all. Only EXECUTION is deferred:
 * the drained chunk bytes (already a snapshot — same memcpy the sync path
 * does into the staging buffer) go into an SPSC ring; the worker decodes
 * them (gx_run: XF/BP/CP-vertex state, draws incl. the nested GX-MT
 * fork/join, EFB copies, TLUT snapshots) at its own pace.
 *
 * PE finish/token is the one observable that would drift (INTSR-visible,
 * the guest polls INTSR constantly): a producer-side SCANNER sizes the
 * top-level command stream as it is pushed and, on any LOAD_BP touching PE
 * regs 0x45/0x47/0x48, records a FENCE at that command's end offset; the
 * tick that pushed a fence waits for the worker to retire through it
 * before returning — so the PE signal (raised by the worker mid-wait, on
 * the PI struct the CPU is provably not touching while it spins here)
 * lands in the SAME tick the synchronous design raised it. Boot cost: one
 * short join per frame (SETDRAWDONE), full overlap inside the frame.
 *
 * The scanner only understands the fixed-size top-level opcodes (NOP,
 * LOAD_CP/XF/INDX/BP, CALL_DL, 0x44, INVL_VC, unknown-1-byte). A top-level
 * PRIMITIVE needs live VCD/VAT state to size (owned by the worker), so it
 * POISONS the pipeline: drain fully, hand the staging leftover back to the
 * CPU (scanner carry == worker leftover by construction: both are "bytes
 * past the last whole top-level command"), and run synchronously from then
 * on, loudly. The IPL never emits top-level primitives (all 994 observed
 * draws live inside display lists — docs/GX_INVENTORY.md), so the boot
 * never poisons; a future game that does gets bit-exact sync behavior and
 * a log line instead of silent risk.
 *
 * Deliberate, gate-arbitrated residual (same class the DSP thread ships
 * with, and the same model Dolphin's dual-core uses): CALL_DL bytes,
 * indexed vertex arrays, textures and TLUT sources are read from guest RAM
 * at WORKER execution time, not producer push time. If the guest mutated
 * that memory between push and execution the result could differ from
 * synchronous consumption — the four golden XFB hashes (repeat-run
 * deterministic) + the oracle value+order diff are the arbiters, exactly
 * as they were for GCN_DSP_THREAD. EFB->XFB copies also land in guest RAM
 * from the worker: the end-of-run drain (boot.c, before GCN_MEM_DUMP), the
 * debug-server screenshot drain, and the fifo-reset drain keep every
 * gate-visible reader exact; the mid-run host-window present may see a
 * field one frame stale, which is a visual non-event.
 * ==========================================================================*/
static u32 rd32(const u8* p);                                    /* defined below */
static u32 gx_run(GcnGx* gx, const u8* data, u32 available);     /* defined below */

#define GX_PIPE_CAP (1u << 20)   /* 1 MiB ring; multiple of 32 so chunks never wrap */
static u8           s_pipe_ring[GX_PIPE_CAP];
static volatile s64 s_pipe_produced = 0;   /* CPU: bytes pushed (always 32-aligned) */
static volatile s64 s_pipe_retired  = 0;   /* worker: bytes fully decoded via gx_run */
static volatile s64 s_pipe_done_upto = 0;  /* worker: produced-offset it has fully
                                            * PROCESSED (pulled + gx_run ran) — a
                                            * partial command tail keeps retired
                                            * below this forever, so full drains
                                            * wait on THIS, not on retired. */
static volatile s32 s_pipe_parked   = 0;   /* worker inside WaitOnAddress            */
static volatile s32 s_pipe_waiting  = 0;   /* CPU inside WaitOnAddress               */
static volatile s32 s_pipe_quit     = 0;
static HANDLE       s_pipe_h        = NULL;
static int          s_pipe_on       = -1;  /* GCN_GX_PIPELINE=1 enables; -1 unresolved */
static int          s_pipe_poisoned = 0;   /* scanner overflow (bug guard): permanent sync */
static int          s_pipe_syncmode = 0;   /* unsizable top-level cmd (e.g. a top-level
                                            * PRIM — the IPL draws one 4-vertex quad
                                            * per frame outside any display list):
                                            * execute synchronously until the scanner
                                            * can re-seed from the staging leftover,
                                            * then resume pipelining. Recoverable. */

/* Producer-side scanner state: bytes of a top-level command split across
 * chunk boundaries. Largest sizable command is LOAD_XF (5 + 16*4 = 69), so
 * the worst carry leftover is 68 bytes (one byte short of whole) and the
 * buffer must hold leftover + one chunk = 100; 128 for slack. */
static u8  s_scan_carry[128];
static u32 s_scan_carry_len = 0;
static s64 s_scan_pos = 0;        /* cumulative stream offset of carry[0] */

static DWORD WINAPI gx_pipe_worker(LPVOID param);

static int gx_pipe_on(void) {
    if (s_pipe_on < 0) {
        const char* e = getenv("GCN_GX_PIPELINE");
        s_pipe_on = (e && e[0] == '1') ? 1 : 0;
        if (s_pipe_on) {
            s_pipe_h = CreateThread(NULL, 0, gx_pipe_worker, NULL, 0, NULL);
            if (!s_pipe_h) s_pipe_on = 0;
        }
    }
    return s_pipe_on && !s_pipe_poisoned && !s_pipe_syncmode;
}

/* Wait until the worker has retired through `target` produced-bytes. */
static void gx_pipe_wait_retired(s64 target) {
    int spins = 0;
    for (;;) {
        s64 r = __atomic_load_n(&s_pipe_retired, __ATOMIC_ACQUIRE);
        if (r >= target) return;
        if (++spins < 16384) { _mm_pause(); continue; }
        s64 expect = r;
        __atomic_store_n(&s_pipe_waiting, 1, __ATOMIC_SEQ_CST);
        WaitOnAddress((volatile VOID*)&s_pipe_retired, &expect, sizeof expect, INFINITE);
        __atomic_store_n(&s_pipe_waiting, 0, __ATOMIC_SEQ_CST);
    }
}

/* Public join: every whole command pushed so far is decoded (a partial
 * command tail may legitimately remain in the worker's staging — exactly
 * like the synchronous design's gx->buf leftover). Waits on done_upto, NOT
 * retired: retired can never reach `produced` while a partial tail exists,
 * and waiting on it deadlocked the first implementation. Called before any
 * gate-visible read of GX-produced state that is not already fenced:
 * end-of-run (boot.c pre-GCN_MEM_DUMP), debug-server screenshots, the PI
 * fifo-reset hook (gp.c), and the poison transition below. */
void gcn_gx_pipeline_drain(void) {
    if (s_pipe_on != 1) return;
    s64 target = __atomic_load_n(&s_pipe_produced, __ATOMIC_RELAXED);
    int spins = 0;
    for (;;) {
        s64 d = __atomic_load_n(&s_pipe_done_upto, __ATOMIC_ACQUIRE);
        if (d >= target) return;
        if (++spins < 16384) { _mm_pause(); continue; }
        s64 expect = d;
        __atomic_store_n(&s_pipe_waiting, 1, __ATOMIC_SEQ_CST);
        WaitOnAddress((volatile VOID*)&s_pipe_done_upto, &expect, sizeof expect, INFINITE);
        __atomic_store_n(&s_pipe_waiting, 0, __ATOMIC_SEQ_CST);
    }
}

static void gx_pipe_push_chunk(const u8* src) {
    /* Ring-full backpressure (worker a full MiB behind): wait for space.
     * retired <= pulled, so gating space on retired is conservative. */
    s64 produced = s_pipe_produced;
    gx_pipe_wait_retired(produced + GCN_CP_GATHER_PIPE_SIZE - (s64)GX_PIPE_CAP);
    memcpy(s_pipe_ring + (produced & (GX_PIPE_CAP - 1)), src, GCN_CP_GATHER_PIPE_SIZE);
    __atomic_store_n(&s_pipe_produced, produced + GCN_CP_GATHER_PIPE_SIZE,
                     __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_pipe_parked, __ATOMIC_SEQ_CST))
        WakeByAddressAll((PVOID)&s_pipe_produced);
}

/* Size one top-level command at data[0..len). Returns the command size, 0 if
 * len doesn't yet hold enough to size it, or (u32)-1 for PRIM/unsizable
 * (poison). Mirrors gx_run_command's sizes EXACTLY (incl. the NOP run and
 * the unknown-opcode 1-byte advance). */
static u32 gx_scan_command(const u8* data, u32 len, int* fence) {
    if (len < 1u) return 0;
    const u8 op = data[0];
    switch (op) {
    case GX_OP_NOP: {
        u32 count = 1;
        while (count < len && data[count] == GX_OP_NOP) count++;
        /* A NOP run truncated by the chunk edge is fine to split: the next
         * scan treats the continuation as a fresh NOP run — identical
         * consumption to gx_run_command seeing it whole. */
        return count;
    }
    case GX_OP_LOAD_CP_REG:  return len < 6u ? 0u : 6u;
    case GX_OP_LOAD_XF_REG: {
        if (len < 5u) return 0;
        u32 n = ((rd32(&data[1]) >> 16) & 0xFu) + 1u;
        return len < 5u + n * 4u ? 0u : 5u + n * 4u;
    }
    case GX_OP_LOAD_INDX_A: case GX_OP_LOAD_INDX_B:
    case GX_OP_LOAD_INDX_C: case GX_OP_LOAD_INDX_D:
        return len < 5u ? 0u : 5u;
    case GX_OP_CALL_DL:      return len < 9u ? 0u : 9u;
    case GX_OP_UNKNOWN_METRICS: return 1;
    case GX_OP_INVL_VC:      return 1;
    case GX_OP_LOAD_BP_REG:
        if (len < 5u) return 0;
        /* PE-observable BP writes: SETDRAWDONE (0x45), PE token (0x47),
         * PE token+int (0x48). Conservative: fence on the register alone
         * (value/mask semantics stay the worker's business — an extra
         * fence only costs a short same-tick join, never exactness). */
        if (data[1] == GX_BP_SETDRAWDONE || data[1] == GX_BP_PE_TOKEN_ID ||
            data[1] == GX_BP_PE_TOKEN_INT_ID)
            *fence = 1;
        return 5;
    default:
        if (op >= GX_OP_PRIM_START && op <= GX_OP_PRIM_END)
            return (u32)-1;   /* needs live VCD/VAT to size: poison */
        return 1;             /* unknown: gx_run_command advances 1 byte too */
    }
}

/* Scan a pushed chunk; returns the produced-offset of the last fence command
 * end in this chunk (or 0 if none), sets *poison on a top-level PRIM. */
static s64 gx_scan_chunk(const u8* chunk, s64 chunk_start_off, int* poison) {
    s64 last_fence = 0;
    /* Append to carry, then consume whole commands from the front. */
    u32 take = GCN_CP_GATHER_PIPE_SIZE;
    if (s_scan_carry_len + take > sizeof s_scan_carry) {
        /* Cannot happen (max sizable command 69 => max leftover 68; 68+32
         * fits 128): a scanner bug, not a stream feature. Permanent sync
         * fallback, loudly. */
        fprintf(stderr, "gx-pipe: scanner carry overflow (%u+%u) — BUG; "
                        "permanent synchronous fallback\n",
                s_scan_carry_len, take);
        s_pipe_poisoned = 1;
        *poison = 1;
        return 0;
    }
    memcpy(s_scan_carry + s_scan_carry_len, chunk, take);
    s_scan_carry_len += take;

    u32 off = 0;
    for (;;) {
        int fence = 0;
        u32 sz = gx_scan_command(s_scan_carry + off, s_scan_carry_len - off, &fence);
        if (sz == 0u) break;
        if (sz == (u32)-1) {
            /* Expected once per frame (the IPL's top-level quad) — announce
             * the first occurrence with context, then stay quiet. */
            static int logged = 0;
            if (!logged) {
                u32 ctx = s_scan_carry_len - off; if (ctx > 12u) ctx = 12u;
                fprintf(stderr, "gx-pipe: unsizable top-level opcode 0x%02X at "
                                "stream offset %lld (sync-mode handoff; logged "
                                "once); next:", s_scan_carry[off],
                        (long long)(s_scan_pos + off));
                for (u32 i = 0; i < ctx; i++)
                    fprintf(stderr, " %02X", s_scan_carry[off + i]);
                fprintf(stderr, "\n");
                logged = 1;
            }
            *poison = 1; break;
        }
        off += sz;
        if (fence) last_fence = s_scan_pos + off;
        if (off >= s_scan_carry_len) break;
    }
    if (off) {
        memmove(s_scan_carry, s_scan_carry + off, s_scan_carry_len - off);
        s_scan_carry_len -= off;
        s_scan_pos += off;
    }
    (void)chunk_start_off;
    return last_fence;
}

static DWORD WINAPI gx_pipe_worker(LPVOID param) {
    (void)param;
    GcnGx* gx = &s_gx;
    s64 pulled = 0;
    for (;;) {
        s64 produced = __atomic_load_n(&s_pipe_produced, __ATOMIC_ACQUIRE);
        if (produced == pulled) {
            if (__atomic_load_n(&s_pipe_quit, __ATOMIC_ACQUIRE)) return 0;
            int spins = 0;
            for (;;) {
                produced = __atomic_load_n(&s_pipe_produced, __ATOMIC_ACQUIRE);
                if (produced != pulled ||
                    __atomic_load_n(&s_pipe_quit, __ATOMIC_ACQUIRE)) break;
                if (++spins < 16384) { _mm_pause(); continue; }
                s64 expect = produced;
                __atomic_store_n(&s_pipe_parked, 1, __ATOMIC_SEQ_CST);
                WaitOnAddress((volatile VOID*)&s_pipe_produced, &expect,
                              sizeof expect, INFINITE);
                __atomic_store_n(&s_pipe_parked, 0, __ATOMIC_SEQ_CST);
            }
            continue;
        }
        /* Pull as much as fits: contiguous ring run, capped by staging room.
         * (Chunks are 32-aligned and CAP is a multiple of 32, so runs are
         * whole chunks.) */
        u32 room  = GX_BUF_CAP - gx->buf_len;
        u32 avail = (u32)(produced - pulled);
        u32 cont  = GX_PIPE_CAP - (u32)(pulled & (GX_PIPE_CAP - 1));
        u32 take  = avail < cont ? avail : cont;
        if (take > room) take = room & ~31u;
        if (take == 0u) {
            /* Staging full of one partial command — matches the sync path's
             * loud halt; nothing can progress. */
            static int warned = 0;
            if (!warned) {
                fprintf(stderr, "gx-pipe: staging overflow (single command > %u) "
                                "— worker halted\n", (unsigned)GX_BUF_CAP);
                warned = 1;
            }
            return 0;
        }
        memcpy(gx->buf + gx->buf_len, s_pipe_ring + (pulled & (GX_PIPE_CAP - 1)), take);
        gx->buf_len += take;
        pulled += take;

        u32 consumed = gx_run(gx, gx->buf, gx->buf_len);
        if (consumed > 0u) {
            if (consumed < gx->buf_len)
                memmove(gx->buf, gx->buf + consumed, gx->buf_len - consumed);
            gx->buf_len -= consumed;
        }
        /* Retired = every pulled byte no longer sitting un-decoded in staging
         * (fence waits key on this — fences are whole commands, so they
         * always retire). done_upto = "processed everything through this
         * produced offset" (full drains key on THIS — a partial-command tail
         * legitimately never retires). */
        __atomic_store_n(&s_pipe_retired, pulled - (s64)gx->buf_len, __ATOMIC_SEQ_CST);
        __atomic_store_n(&s_pipe_done_upto, pulled, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&s_pipe_waiting, __ATOMIC_SEQ_CST)) {
            WakeByAddressAll((PVOID)&s_pipe_retired);
            WakeByAddressAll((PVOID)&s_pipe_done_upto);
        }
    }
}

/* Sync-mode transition: an unsizable top-level command (the IPL draws one
 * 4-vertex quad per frame OUTSIDE any display list, so this fires every
 * frame). Drain the worker fully — after that the worker is parked-idle and
 * the CPU owns all GX state race-free, so the tick's synchronous fall-
 * through can execute from gx->buf exactly like the pre-pipeline design.
 * gx_pipe_try_resume() re-seeds the scanner from the staging leftover at
 * the end of each sync tick and turns the pipeline back on. */
static void gx_pipe_enter_syncmode(void) {
    static int announced = 0;
    if (!announced) {
        fprintf(stderr, "gx-pipe: unsizable top-level command — executing "
                        "synchronously and re-seeding (recoverable; announced "
                        "once)\n");
        announced = 1;
    }
    gcn_gx_pipeline_drain();
    s_pipe_syncmode = 1;
}

/* End-of-tick in sync mode: if the staging leftover (a partial top-level
 * command) fits the scanner carry, adopt it as the new scan state and
 * resume pipelining. Offset origin: scan_pos = produced - buf_len keeps
 * fence offsets comparable with the worker's retired accounting
 * (retired = pulled - buf_len; the leftover bytes never entered the ring,
 * so both sides discount them identically). A leftover too large (tick
 * budget ended mid-PRIM) just stays in sync mode another tick. */
static void gx_pipe_try_resume(void) {
    GcnGx* gx = &s_gx;
    if (gx->buf_len > sizeof s_scan_carry)
        return;
    memcpy(s_scan_carry, gx->buf, gx->buf_len);
    s_scan_carry_len = gx->buf_len;
    s_scan_pos = __atomic_load_n(&s_pipe_produced, __ATOMIC_RELAXED) - (s64)gx->buf_len;
    s_pipe_syncmode = 0;
}

/* ---- big-endian readers (FIFO bytes are GameCube big-endian) ---- */
static u32 rd32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}
static u32 rd24(const u8* p) {
    return ((u32)p[0] << 16) | ((u32)p[1] << 8) | (u32)p[2];
}
static u16 rd16(const u8* p) {
    return (u16)(((u16)p[0] << 8) | (u16)p[1]);
}

/* ============================================================================
 * Vertex size computation — transcribed from VertexLoaderBase::GetVertexSize
 * (VertexLoaderBase.cpp:175-203) + the per-component size tables in
 * VertexLoader_{Position,Normal,Color,TextCoord}.h. Component/format enums are
 * CPMemory.h:100-224. A wrong size desyncs the FIFO, so this must be exact.
 * ==========================================================================*/

/* GetElementSize (CPMemory.h:142-161): 0/1 -> 1, 2/3 -> 2, 4..7 -> 4. */
static u32 elem_size(u32 fmt) {
    switch (fmt & 7u) {
    case 0: case 1: return 1;
    case 2: case 3: return 2;
    default:        return 4;   /* Float + InvalidFloat5..7 behave as float */
    }
}

/* VertexComponentFormat: 0 NotPresent, 1 Direct, 2 Index8, 3 Index16. */
enum { VCF_NONE = 0, VCF_DIRECT = 1, VCF_INDEX8 = 2, VCF_INDEX16 = 3 };

/* VertexLoader_Position.h s_table_size. */
static u32 pos_size(u32 type, u32 fmt, u32 elements /*0 XY,1 XYZ*/) {
    switch (type) {
    case VCF_NONE:    return 0;
    case VCF_INDEX8:  return 1;
    case VCF_INDEX16: return 2;
    default:          return (elements ? 3u : 2u) * elem_size(fmt);   /* Direct */
    }
}

/* VertexLoader_Normal.h s_table_size (elements: 0 N, 1 NTB; index3 bool). */
static u32 norm_size(u32 type, u32 fmt, u32 elements, u32 index3) {
    switch (type) {
    case VCF_NONE:    return 0;
    case VCF_DIRECT:  return (elements ? 9u : 3u) * elem_size(fmt);
    case VCF_INDEX8:  return elements ? (index3 ? 3u : 1u) : 1u;
    default:          return elements ? (index3 ? 6u : 2u) : 2u;      /* Index16 */
    }
}

/* VertexLoader_Color.h s_table_size (Direct sizes keyed by ColorFormat 0..5). */
static u32 color_size(u32 type, u32 cfmt) {
    static const u32 direct[6] = { 2, 3, 4, 2, 3, 4 };  /* 565,888,888x,4444,6666,8888 */
    switch (type) {
    case VCF_NONE:    return 0;
    case VCF_INDEX8:  return 1;
    case VCF_INDEX16: return 2;
    default:          return (cfmt <= 5u) ? direct[cfmt] : 0u;        /* Direct */
    }
}

/* VertexLoader_TextCoord.h s_table_size (elements: 0 S, 1 ST). */
static u32 tc_size(u32 type, u32 fmt, u32 elements) {
    switch (type) {
    case VCF_NONE:    return 0;
    case VCF_INDEX8:  return 1;
    case VCF_INDEX16: return 2;
    default:          return (elements ? 2u : 1u) * elem_size(fmt);   /* Direct */
    }
}

/* Per-tex-coord VAT field extraction (CPMemory.h VAT::GetTexFormat/Elements). */
static u32 tex_elements(const GxCpState* s, u32 vat, u32 idx) {
    u32 g0 = s->vat_g0[vat], g1 = s->vat_g1[vat], g2 = s->vat_g2[vat];
    switch (idx) {
    case 0: return (g0 >> 21) & 1u;
    case 1: return (g1 >> 0)  & 1u;
    case 2: return (g1 >> 9)  & 1u;
    case 3: return (g1 >> 18) & 1u;
    case 4: return (g1 >> 27) & 1u;
    case 5: return (g2 >> 5)  & 1u;
    case 6: return (g2 >> 14) & 1u;
    default:return (g2 >> 23) & 1u;
    }
}
static u32 tex_format(const GxCpState* s, u32 vat, u32 idx) {
    u32 g0 = s->vat_g0[vat], g1 = s->vat_g1[vat], g2 = s->vat_g2[vat];
    switch (idx) {
    case 0: return (g0 >> 22) & 7u;
    case 1: return (g1 >> 1)  & 7u;
    case 2: return (g1 >> 10) & 7u;
    case 3: return (g1 >> 19) & 7u;
    case 4: return (g1 >> 28) & 7u;
    case 5: return (g2 >> 6)  & 7u;
    case 6: return (g2 >> 15) & 7u;
    default:return (g2 >> 24) & 7u;
    }
}

static u32 popcount9(u32 v) {
    u32 n = 0;
    v &= 0x1FFu;
    while (v) { n += v & 1u; v >>= 1; }
    return n;
}

/* Full vertex size for a VAT index (VertexLoaderBase.cpp:175-203). */
static u32 gx_vertex_size(const GxCpState* s, u32 vat) {
    u32 low = s->vtx_desc_lo, high = s->vtx_desc_hi;
    u32 g0 = s->vat_g0[vat];
    u32 size = popcount9(low);   /* PosMatIdx + 8 TexMatIdx, one byte each */

    size += pos_size((low >> 9) & 3u, (g0 >> 1) & 7u, g0 & 1u);
    size += norm_size((low >> 11) & 3u, (g0 >> 10) & 7u, (g0 >> 9) & 1u, (g0 >> 31) & 1u);

    /* Two color channels (CPMemory.h Color0Comp bits14-16, Color1Comp bits18-20) */
    size += color_size((low >> 13) & 3u, (g0 >> 14) & 7u);
    size += color_size((low >> 15) & 3u, (g0 >> 18) & 7u);

    for (u32 i = 0; i < 8; i++) {
        u32 type = (high >> (2u * i)) & 3u;
        size += tc_size(type, tex_format(s, vat, i), tex_elements(s, vat, i));
    }
    return size;
}

/* ============================================================================
 * CP register load (CPMemory.cpp LoadCPReg:90-199). Only VCD/VAT affect vertex
 * size; everything is stored faithfully anyway.
 * ==========================================================================*/
static void gx_on_cp(GcnGx* gx, u8 cmd, u32 value) {
    if (note_once(&gx->seen_cp[cmd]))
        fprintf(stderr, "gx: CP reg 0x%02X first loaded (val 0x%08X)\n", cmd, value);

    GxCpState* s = &gx->cpst;
    switch (cmd & GX_CP_COMMAND_MASK) {
    case GX_CP_MATINDEX_A:   s->matrix_index_a = value; break;
    case GX_CP_MATINDEX_B:   s->matrix_index_b = value; break;
    case GX_CP_VCD_LO:       s->vtx_desc_lo = value; break;
    case GX_CP_VCD_HI:       s->vtx_desc_hi = value; break;
    case GX_CP_VAT_REG_A:    s->vat_g0[cmd & 7u] = value; break;
    case GX_CP_VAT_REG_B:    s->vat_g1[cmd & 7u] = value; break;
    case GX_CP_VAT_REG_C:    s->vat_g2[cmd & 7u] = value; break;
    case GX_CP_ARRAY_BASE:   s->array_bases[cmd & 0xFu]   = value & 0x1FFFFFFFu; break;
    case GX_CP_ARRAY_STRIDE: s->array_strides[cmd & 0xFu] = value & 0xFFu; break;
    default:
        /* 0x00/0x10/0x20 are perf-query commands (LoadCPReg:94-104, no state). */
        break;
    }
}

/* ============================================================================
 * XF register load (OpcodeDecoding.h:157-175 -> LoadXFReg). Store `count` u32
 * words starting at `address` into the XF memory array. Matrix/light memory is
 * <0x1000; registers are 0x1000..0x1057.
 * ==========================================================================*/
static void gx_on_xf(GcnGx* gx, u16 address, u8 count, const u8* data) {
    if (address >= GX_XF_REGISTERS_START &&
        address < GX_XF_REGISTERS_START + 0x60u) {
        if (note_once(&gx->seen_xf_reg[address - GX_XF_REGISTERS_START]))
            fprintf(stderr, "gx: XF register 0x%04X first loaded (count %u)\n",
                    address, count);
    } else if (address < GX_XF_REGISTERS_START) {
        if (note_once(&gx->seen_xf_mem))
            fprintf(stderr, "gx: XF matrix/light memory first loaded "
                            "(addr 0x%04X count %u)\n", address, count);
    }

    for (u8 i = 0; i < count; i++) {
        u32 addr = (u32)address + i;
        if (addr < GX_XF_MEM_WORDS) {
            gx->xf[addr] = rd32(&data[i * 4u]);
        } else {
            static int warned = 0;
            if (!warned) {
                fprintf(stderr, "gx: XF load out of range (addr 0x%04X) — ignored\n",
                        addr);
                warned = 1;
            }
        }
    }
}

/* ============================================================================
 * BP register load (BPStructs.cpp BPWritten:55-396). Store the value; run the
 * side effects for the registers that have them.
 * ==========================================================================*/
/* Modeled TMEM (1MB, matching Dolphin's s_tex_mem/TMEM_SIZE). Only the TLUT
 * path uses it so far: LOADTLUT1 writes snapshots of guest RAM here, and the
 * rasterizer's paletted decode (C4/C8/C14X2) reads palette entries from it
 * via gcn_gx_tmem(). Texture image preloads (BPMEM_PRELOAD_*) are not
 * modeled — textures are sampled straight from MEM1, Dolphin's
 * non-cache_manually_managed path (see gx_raster.c tex_sample scope note). */
static u8 s_gx_tmem[0x100000];

const u8* gcn_gx_tmem(void) { return s_gx_tmem; }

static void gx_on_bp(GcnGx* gx, u8 cmd, u32 value) {
    if (note_once(&gx->seen_bp[cmd]))
        fprintf(stderr, "gx: BP reg 0x%02X first written (val 0x%06X)\n", cmd, value);

    gx->bp[cmd] = value;

    switch (cmd) {
    case GX_BP_SETDRAWDONE:            /* BPStructs.cpp:180-201 */
        if ((value & 0xFFu) == 0x02u) {
            gcn_pe_set_finish(gx->pe);
            fprintf(stderr, "gx: GXSetDrawDone -> PE finish (val 0x%04X)\n",
                    value & 0xFFFFu);
        } else {
            fprintf(stderr, "gx: GXSetDrawDone ??? (val 0x%04X)\n", value & 0xFFFFu);
        }
        break;
    case GX_BP_PE_TOKEN_ID:            /* BPStructs.cpp:202-217 (no interrupt) */
        gcn_pe_set_token(gx->pe, (u16)(value & 0xFFFFu), 0);
        break;
    case GX_BP_PE_TOKEN_INT_ID:        /* BPStructs.cpp:218-233 (interrupt) */
        gcn_pe_set_token(gx->pe, (u16)(value & 0xFFFFu), 1);
        break;
    case GX_BP_LOADTLUT1: {            /* BPStructs.cpp BPMEM_LOADTLUT1 */
        /* Copy a Texture Look-Up Table from guest RAM into modeled TMEM.
         * The copy happens AT WRITE TIME (the guest may overwrite the RAM
         * source afterwards — TMEM keeps the snapshot, exactly like HW).
         * Source address comes from the last LOADTLUT0 write (<<5, upper
         * bits ignored on GameCube: & 0x01FFFFFF); dest/count from this
         * value: tmem_addr bits 0-9 (<<9 = byte address), tmem_line_count
         * bits 10-20 (x32 bytes). Dolphin static_asserts the max transfer
         * (0x3FF<<9 + 0x7FF*32) fits TMEM_SIZE, so an in-range guest source
         * can never overflow the 1MB array; a source outside MEM1 is
         * clamped and logged instead of read out of bounds. */
        u32 tmem_addr = (value & 0x3FFu) << 9;
        u32 count = ((value >> 10) & 0x7FFu) * 32u;
        u32 src = (gx->bp[GX_BP_LOADTLUT0] << 5) & 0x01FFFFFFu;
        CPUState* cpu = gx->cpu;
        if (cpu && cpu->ram && (u64)src + count <= (u64)cpu->ram_size) {
            memcpy(s_gx_tmem + tmem_addr, cpu->ram + src, count);
        } else {
            fprintf(stderr, "gx: LOADTLUT source out of MEM1 (src 0x%08X count %u) — skipped\n",
                    src, count);
        }
        break;
    }
    case GX_BP_TRIGGER_EFB_COPY:       /* BPStructs.cpp:240-395 */
        /* GCN_GX_STATS bucket 4 (EFB): timed only at this call site, not the
         * BP dispatch around it — the off path below is byte-identical work. */
        if (s_gxstats) {
            u64 t0 = __rdtsc();
            gx_raster_efb_copy(&gx->cpst);
            s_gx_tsc[GX_STAT_EFB] += __rdtsc() - t0;
            s_gx_efbcopies++;
        } else {
            gx_raster_efb_copy(&gx->cpst);
        }
        break;
    default:
        /* All other BP regs: state storage only (that IS their hardware effect
         * until a rasterizer reads them). Already stored above. */
        break;
    }
}

/* ============================================================================
 * Opcode decode + execute — mirrors OpcodeDecoder::detail::RunCommand
 * (OpcodeDecoding.h:125-253). Returns the number of bytes consumed, or 0 if the
 * available bytes do not yet hold a whole command (NotEnoughData). A return of 0
 * makes the caller stop and wait for more FIFO data.
 * ==========================================================================*/
static u32 gx_run(GcnGx* gx, const u8* data, u32 available);   /* fwd (CALL_DL) */

static u32 gx_run_command(GcnGx* gx, const u8* data, u32 available) {
    if (available < 1u)
        return 0;

    const u8 op = data[0];

    switch (op) {
    case GX_OP_NOP: {
        u32 count = 1;
        while (count < available && data[count] == GX_OP_NOP)
            count++;
        if (note_once(&gx->seen_opcode[GX_OP_NOP]))
            fprintf(stderr, "gx: opcode NOP first seen\n");
        return count;
    }

    case GX_OP_LOAD_CP_REG: {
        if (available < 6u) return 0;
        if (note_once(&gx->seen_opcode[op]))
            fprintf(stderr, "gx: opcode LOAD_CP_REG first seen\n");
        gx_on_cp(gx, data[1], rd32(&data[2]));
        return 6;
    }

    case GX_OP_LOAD_XF_REG: {
        if (available < 5u) return 0;
        u32 cmd2 = rd32(&data[1]);
        u32 stream_size_temp = cmd2 >> 16;
        if (stream_size_temp >= 16u) {
            static int warned = 0;
            if (!warned) {
                fprintf(stderr, "gx: LOAD_XF_REG stream size field 0x%X >= 16 "
                                "(cmd2 0x%08X) — masking to 4 bits\n",
                        stream_size_temp, cmd2);
                warned = 1;
            }
        }
        u32 stream_size = (stream_size_temp & 0xFu) + 1u;
        if (available < 5u + stream_size * 4u) return 0;
        if (note_once(&gx->seen_opcode[op]))
            fprintf(stderr, "gx: opcode LOAD_XF_REG first seen\n");
        gx_on_xf(gx, (u16)(cmd2 & 0xFFFFu), (u8)stream_size, &data[5]);
        return 5u + stream_size * 4u;
    }

    case GX_OP_LOAD_INDX_A:
    case GX_OP_LOAD_INDX_B:
    case GX_OP_LOAD_INDX_C:
    case GX_OP_LOAD_INDX_D: {
        if (available < 5u) return 0;
        if (note_once(&gx->seen_opcode[op]))
            fprintf(stderr, "gx: opcode LOAD_INDX_%c UNIMPLEMENTED (indexed XF "
                            "load) — payload consumed, load skipped\n",
                    'A' + (int)((op - GX_OP_LOAD_INDX_A) / 8));
        return 5;
    }

    case GX_OP_CALL_DL: {
        if (available < 9u) return 0;
        if (note_once(&gx->seen_opcode[op]))
            fprintf(stderr, "gx: opcode CALL_DL first seen\n");
        u32 addr = rd32(&data[1]) & ~31u;
        u32 size = rd32(&data[5]) & ~31u;

        /* OnDisplayList (OpcodeDecoding.cpp:143-200): recursion is not allowed —
         * Dolphin warns and skips a nested DL. Run the DL bytes from guest RAM. */
        if (gx->dl_depth > 0) {
            static int warned = 0;
            if (!warned) {
                fprintf(stderr, "gx: recursive display list detected — skipped\n");
                warned = 1;
            }
            return 9;
        }
        CPUState* cpu = gx->cpu;
        u32 phys = addr & 0x1FFFFFFFu;
        if (cpu && cpu->ram && size > 0u &&
            (u64)phys + (u64)size <= (u64)cpu->ram_size) {
            if (s_gxstats) s_gx_dlcalls++;   /* GCN_GX_STATS: DL calls counter */
            gx->dl_depth++;
            gx->cur_dl_addr = addr;
            gx_run(gx, cpu->ram + phys, size);
            gx->cur_dl_addr = 0;
            gx->dl_depth--;
        } else if (size > 0u) {
            static int warned = 0;
            if (!warned) {
                fprintf(stderr, "gx: display list 0x%08X size %u out of MEM1 range "
                                "— skipped\n", addr, size);
                warned = 1;
            }
        }
        return 9;
    }

    case GX_OP_UNKNOWN_METRICS:   /* 0x44: OnUnknown (OpcodeDecoding.cpp:207-212) */
        if (note_once(&gx->seen_opcode[op]))
            fprintf(stderr, "gx: opcode 0x44 (unknown metrics) — no-op\n");
        return 1;

    case GX_OP_INVL_VC:           /* 0x48: invalidate vertex cache — no-op */
        if (note_once(&gx->seen_opcode[op]))
            fprintf(stderr, "gx: opcode INVL_VC (vertex-cache invalidate) — no-op\n");
        return 1;

    case GX_OP_LOAD_BP_REG: {
        if (available < 5u) return 0;
        if (note_once(&gx->seen_opcode[op]))
            fprintf(stderr, "gx: opcode LOAD_BP_REG first seen\n");
        gx_on_bp(gx, data[1], rd24(&data[2]));
        return 5;
    }

    default:
        if (op >= GX_OP_PRIM_START && op <= GX_OP_PRIM_END) {
            if (available < 3u) return 0;
            u32 prim = (op >> 3) & 7u;       /* GX_PRIMITIVE_MASK 0x78 >> 3 */
            u32 vat  = op & 7u;              /* GX_VAT_MASK */
            u32 vsize = gx_vertex_size(&gx->cpst, vat);
            u32 nverts = rd16(&data[1]);

            /* A wrong vertex size desyncs the stream; a nonzero primitive with a
             * zero computed size means VCD/VAT were never set (or are corrupt).
             * ERROR loudly and stop the drain rather than guess (GX_PLAN). */
            if (nverts > 0u && vsize == 0u) {
                static int errored = 0;
                if (!errored) {
                    fprintf(stderr, "gx: PRIMITIVE 0x%02X (prim %u vat %u) has %u "
                                    "vertices but computed vertex size 0 "
                                    "(VCD lo 0x%08X hi 0x%08X) — STOPPING DRAIN "
                                    "to avoid FIFO desync\n",
                            op, prim, vat, nverts,
                            gx->cpst.vtx_desc_lo, gx->cpst.vtx_desc_hi);
                    errored = 1;
                }
                return 0;   /* stop: do not advance past an unknowable payload */
            }

            u32 total = 3u + nverts * vsize;
            if (available < total) return 0;

            if (note_once(&gx->seen_prim[prim]))
                fprintf(stderr, "gx: primitive type %u (opcode 0x%02X) first drawn "
                                "(%u verts x %u bytes, vat %u) pc=0x%08X dl=0x%08X "
                                "vcd_lo=0x%08X vcd_hi=0x%08X vat_g0=0x%08X vat_g1=0x%08X "
                                "vat_g2=0x%08X\n",
                        prim, op, nverts, vsize, vat,
                        gx->cpu ? gx->cpu->pc : 0u, gx->cur_dl_addr,
                        gx->cpst.vtx_desc_lo, gx->cpst.vtx_desc_hi,
                        gx->cpst.vat_g0[vat], gx->cpst.vat_g1[vat], gx->cpst.vat_g2[vat]);
            /* Rasterize (SWVertexLoader -> TransformUnit -> Clipper ->
             * Rasterizer -> Tev). The payload is contiguous in `data`.
             * GCN_GX_STATS bucket 3 (DRAW): timed only at this call site — the
             * off path below is byte-identical work. */
            if (s_gxstats) {
                u64 t0 = __rdtsc();
                gx_raster_draw(&gx->cpst, prim, vat, &data[3], nverts, vsize);
                s_gx_tsc[GX_STAT_DRAW] += __rdtsc() - t0;
                s_gx_draws++;
                s_gx_verts += nverts;
            } else {
                gx_raster_draw(&gx->cpst, prim, vat, &data[3], nverts, vsize);
            }
            return total;
        }

        /* Unknown opcode. HandleUnknownOpcode advances 1 byte (OpcodeDecoding.cpp
         * :219-224). Loud once — an unknown here usually means desync. Dump the
         * DL depth + the next bytes so the culprit command is identifiable. */
        if (note_once(&gx->seen_opcode[op])) {
            u32 ctx = available < 16u ? available : 16u;
            fprintf(stderr, "gx: UNKNOWN opcode 0x%02X (dl_depth=%d, avail=%u) — "
                            "advancing 1 byte (possible FIFO desync); next:",
                    op, gx->dl_depth, available);
            for (u32 i = 0; i < ctx; i++) fprintf(stderr, " %02X", data[i]);
            if (gx->dl_depth == 0 && data >= gx->buf && data < gx->buf + GX_BUF_CAP) {
                u32 back = (u32)(data - gx->buf);
                if (back > 24u) back = 24u;
                fprintf(stderr, " | prev:");
                for (u32 i = back; i > 0; i--) fprintf(stderr, " %02X", data[-(int)i]);
            }
            fprintf(stderr, "\n");
        }
        return 1;
    }
}

/* OpcodeDecoder::Run (OpcodeDecoding.h:267-279): consume whole commands until a
 * partial one (return 0). Returns total bytes consumed. */
static u32 gx_run(GcnGx* gx, const u8* data, u32 available) {
    u32 off = 0;
    while (off < available) {
        u32 sz = gx_run_command(gx, &data[off], available - off);
        if (sz == 0u) break;
        if (s_gxstats) s_gx_commands++;   /* GCN_GX_STATS: commands-decoded counter */
        off += sz;
    }
    return off;
}

/* ============================================================================
 * Public API
 * ==========================================================================*/
void gcn_gx_init(CPUState* cpu, GcnCp* cp, GcnPe* pe) {
    memset(&s_gx, 0, sizeof s_gx);
    s_gx.cpu = cpu;
    s_gx.cp  = cp;
    s_gx.pe  = pe;
    /* Bind the persistent BP/XF register files + guest CPU into the rasterizer
     * (they never move) and clear the EFB model. */
    gx_raster_init(cpu, s_gx.bp, s_gx.xf);
}

void gcn_gx_tick(u32 cycles) {
    (void)cycles;
    /* GCN_GX_STATS=1: resolved once (cached -1 sentinel), then a single
     * `if (s_gxstats)` branch guards every timed site below — see the big
     * comment above s_gx.c's stats statics for the bucket definitions. */
    if (s_gxstats < 0)
        s_gxstats = getenv("GCN_GX_STATS") ? 1 : 0;

    GcnGx* gx = &s_gx;
    if (!gx->cp || !gx->cpu)
        return;
    /* Fifo.cpp RunGpuLoop:317-320 gate: GPReadEnable && distance && !breakpoint. */
    if (!gx->cp->gp_read_enable)
        return;

    /* Unconditional (not gated on s_gxstats): this counter is the shared
     * 2^20-tick print cadence for ALL of "[gx-stats]"/"[gx-draw-stats]"/
     * "[gx-pixel-stats]", and GCN_GX_PIXEL_STATS must be able to print on its
     * own cadence without GCN_GX_STATS also being set (they are independent
     * knobs — see gx_raster.c's s_pixel_stats comment). Cost is one u64
     * increment per tick, not per pixel/command — negligible next to the FIFO
     * drain work this function already does unconditionally every tick. */
    s_gx_ticks++;

    /* G3 pipeline path: identical architectural drain (rptr/rw_dist/breakpoint
     * arithmetic on the CPU-side CP state, same per-tick budget), but the
     * snapshot bytes go to the worker's ring instead of being decoded here.
     * The scanner sizes the stream for PE fences as it pushes. */
    u32 drained = 0;   /* per-tick budget, shared by both paths so the poison
                        * tick still drains at most GCN_GX_DRAIN_BYTES_PER_TICK
                        * in total — byte-identical architectural consumption. */
    if (gx_pipe_on()) {
        s64 fence = 0;
        int poison = 0;
        while (drained < GCN_GX_DRAIN_BYTES_PER_TICK &&
               gcn_cp_fifo_rw_distance(gx->cp) >= GCN_CP_GATHER_PIPE_SIZE &&
               !gcn_cp_at_breakpoint(gx->cp)) {
            u32 rptr = gcn_cp_fifo_read_pointer(gx->cp);
            u32 phys = rptr & 0x1FFFFFFFu;
            if ((u64)phys + GCN_CP_GATHER_PIPE_SIZE > (u64)gx->cpu->ram_size) {
                static int warned = 0;
                if (!warned) {
                    fprintf(stderr, "gx: FIFO read pointer 0x%08X out of MEM1 range — "
                                    "drain halted\n", rptr);
                    warned = 1;
                }
                break;
            }
            const u8* chunk = gx->cpu->ram + phys;
            s64 f = gx_scan_chunk(chunk, 0, &poison);
            if (poison) {
                /* Do NOT push this chunk; the synchronous fall-through takes
                 * over from exactly here (nothing of this chunk was consumed
                 * architecturally yet). Recoverable unless the scanner
                 * itself overflowed (s_pipe_poisoned, a bug guard). */
                gx_pipe_enter_syncmode();
                break;
            }
            gx_pipe_push_chunk(chunk);
            gcn_cp_gpu_consume_chunk(gx->cp);
            drained += GCN_CP_GATHER_PIPE_SIZE;
            if (f) fence = f;
        }
        if (fence)
            gx_pipe_wait_retired(fence);   /* PE signal lands THIS tick, like sync */
        if (!s_pipe_syncmode && !s_pipe_poisoned)
            return;   /* (GCN_GX_STATS' tick-end print is skipped in pipeline
                       * mode — its rdtsc buckets are meaningless split across
                       * threads; use GCN_GX_PIPELINE=0 for GX profiling.) */
        /* sync mode / poisoned: fall through into the synchronous loop below
         * for the remainder of this tick's budget. */
    }

    while (drained < GCN_GX_DRAIN_BYTES_PER_TICK &&
           gcn_cp_fifo_rw_distance(gx->cp) >= GCN_CP_GATHER_PIPE_SIZE &&
           !gcn_cp_at_breakpoint(gx->cp)) {

        /* Read the 32 bytes at the current read pointer (Fifo.cpp ReadDataFromFifo
         * :215-236 copies a GATHER_PIPE_SIZE chunk into the video buffer). */
        u32 rptr = gcn_cp_fifo_read_pointer(gx->cp);
        u32 phys = rptr & 0x1FFFFFFFu;
        if ((u64)phys + GCN_CP_GATHER_PIPE_SIZE > (u64)gx->cpu->ram_size) {
            static int warned = 0;
            if (!warned) {
                fprintf(stderr, "gx: FIFO read pointer 0x%08X out of MEM1 range — "
                                "drain halted\n", rptr);
                warned = 1;
            }
            break;
        }
        if (gx->buf_len + GCN_CP_GATHER_PIPE_SIZE > GX_BUF_CAP) {
            static int warned = 0;
            if (!warned) {
                fprintf(stderr, "gx: staging buffer overflow (%u bytes buffered, a "
                                "single command exceeds %u) — drain halted\n",
                        gx->buf_len, GX_BUF_CAP);
                warned = 1;
            }
            break;
        }

        /* GCN_GX_STATS bucket 1 (FIFO): the chunk copy + CP consume. The off
         * path is byte-identical work, zero timing calls. */
        if (s_gxstats) {
            u64 t0 = __rdtsc();
            memcpy(gx->buf + gx->buf_len, gx->cpu->ram + phys, GCN_CP_GATHER_PIPE_SIZE);
            gx->buf_len += GCN_CP_GATHER_PIPE_SIZE;
            /* Advance the read side (wrap + distance-32 + status/interrupt eval). */
            gcn_cp_gpu_consume_chunk(gx->cp);
            s_gx_tsc[GX_STAT_FIFO] += __rdtsc() - t0;
            s_gx_chunks++;
        } else {
            memcpy(gx->buf + gx->buf_len, gx->cpu->ram + phys, GCN_CP_GATHER_PIPE_SIZE);
            gx->buf_len += GCN_CP_GATHER_PIPE_SIZE;
            gcn_cp_gpu_consume_chunk(gx->cp);
        }
        drained += GCN_CP_GATHER_PIPE_SIZE;

        /* Run whole commands out of the staging buffer; keep the leftover partial
         * command for the next chunk (Fifo.cpp:342-352 advances the read ptr past
         * consumed commands only).
         *
         * GCN_GX_STATS bucket 2 (DECODE): gx_run's own wall time with whatever
         * it spent inside gx_raster_draw/gx_raster_efb_copy (buckets 3/4,
         * timed at their own call sites deeper in gx_run_command/gx_on_bp)
         * subtracted back out — those nested calls happen *inside* this
         * gx_run() call, so without the subtraction DECODE would double-count
         * them. Reading the DRAW/EFB accumulators before and after isolates
         * exactly the delta this call contributed. */
        u32 consumed;
        if (s_gxstats) {
            u64 draw_efb_before = s_gx_tsc[GX_STAT_DRAW] + s_gx_tsc[GX_STAT_EFB];
            u64 t0 = __rdtsc();
            consumed = gx_run(gx, gx->buf, gx->buf_len);
            u64 t1 = __rdtsc();
            u64 draw_efb_after = s_gx_tsc[GX_STAT_DRAW] + s_gx_tsc[GX_STAT_EFB];
            s_gx_tsc[GX_STAT_DECODE] += (t1 - t0) - (draw_efb_after - draw_efb_before);
        } else {
            consumed = gx_run(gx, gx->buf, gx->buf_len);
        }

        if (consumed > 0u) {
            /* GCN_GX_STATS bucket 1 (FIFO) continued: the leftover memmove. */
            if (s_gxstats) {
                u64 t0 = __rdtsc();
                if (consumed < gx->buf_len)
                    memmove(gx->buf, gx->buf + consumed, gx->buf_len - consumed);
                gx->buf_len -= consumed;
                s_gx_tsc[GX_STAT_FIFO] += __rdtsc() - t0;
            } else {
                if (consumed < gx->buf_len)
                    memmove(gx->buf, gx->buf + consumed, gx->buf_len - consumed);
                gx->buf_len -= consumed;
            }
        }
    }

    /* G3: sync-mode tick complete — the unsizable stretch has been executed
     * synchronously; try to hand the stream back to the pipeline. */
    if (s_pipe_on == 1 && s_pipe_syncmode && !s_pipe_poisoned)
        gx_pipe_try_resume();

    /* Summary line every 2^20 ticks (matches GCN_DISPATCH_STATS' cadence) so
     * stderr stays sparse — this is a diagnostic window, not per-tick noise.
     * The cadence gate itself is independent of any specific stats knob (see
     * s_gx_ticks' comment above); each block below is individually guarded on
     * its own knob's data (tot>0 / draw_calls>0 / shaded>0) so GCN_GX_STATS
     * and GCN_GX_PIXEL_STATS print on this shared cadence but fully
     * independently of each other — neither requires the other to be set. */
    if ((s_gx_ticks & 0xFFFFFu) == 0u) {
        u64 tot = s_gx_tsc[GX_STAT_FIFO] + s_gx_tsc[GX_STAT_DECODE] +
                  s_gx_tsc[GX_STAT_DRAW] + s_gx_tsc[GX_STAT_EFB];
        if (s_gxstats && tot > 0u) {
            fprintf(stderr,
                "[gx-stats] ticks=%llu  fifo=%.1f%% decode=%.1f%% draw=%.1f%% efb=%.1f%%"
                "  | chunks=%llu cmds=%llu draws=%llu verts=%llu dl=%llu efbcopy=%llu\n",
                (unsigned long long)s_gx_ticks,
                100.0 * (double)s_gx_tsc[GX_STAT_FIFO]   / (double)tot,
                100.0 * (double)s_gx_tsc[GX_STAT_DECODE] / (double)tot,
                100.0 * (double)s_gx_tsc[GX_STAT_DRAW]   / (double)tot,
                100.0 * (double)s_gx_tsc[GX_STAT_EFB]    / (double)tot,
                (unsigned long long)s_gx_chunks, (unsigned long long)s_gx_commands,
                (unsigned long long)s_gx_draws, (unsigned long long)s_gx_verts,
                (unsigned long long)s_gx_dlcalls, (unsigned long long)s_gx_efbcopies);
            fflush(stderr);
        }

        /* gx_raster.c's further split of the EFB bucket above (copy-encode vs
         * scalar clear-rect) — sizing input for the efb_clear_rect SIMD task.
         * Same cadence/one-print-site pattern as the DRAW-bucket split below. */
        if (s_gxstats && s_gx_tsc[GX_STAT_EFB] > 0u) {
            u64 tsc_efb_clear, efb_clear_calls;
            gx_raster_get_efb_clear_stats(&tsc_efb_clear, &efb_clear_calls);
            u64 efb_tot = s_gx_tsc[GX_STAT_EFB];
            u64 tsc_efb_copy = (efb_tot > tsc_efb_clear) ? (efb_tot - tsc_efb_clear) : 0u;
            fprintf(stderr,
                "[gx-efb-stats] copy=%.1f%% clear=%.1f%%  | clear_calls=%llu\n",
                100.0 * (double)tsc_efb_copy   / (double)efb_tot,
                100.0 * (double)tsc_efb_clear  / (double)efb_tot,
                (unsigned long long)efb_clear_calls);
            fflush(stderr);
        }

        /* gx_raster.c's own further split of the DRAW bucket above (vertex
         * load+transform+clip vs triangle scan/pixel) plus a pixels-shaded
         * counter — same cadence, piggybacked onto this tick so there is only
         * one stats print site to keep in sync (gx_raster_get_draw_stats). */
        u64 tsc_vtx, tsc_tri, pixels_shaded, draw_calls;
        gx_raster_get_draw_stats(&tsc_vtx, &tsc_tri, &pixels_shaded, &draw_calls);
        u64 vtx_tri_tot = tsc_vtx + tsc_tri;
        if (draw_calls > 0u && vtx_tri_tot > 0u) {
            fprintf(stderr,
                "[gx-draw-stats] draws=%llu  vtx=%.1f%% tri=%.1f%%  | pixels_shaded=%llu\n",
                (unsigned long long)draw_calls,
                100.0 * (double)tsc_vtx / (double)vtx_tri_tot,
                100.0 * (double)tsc_tri / (double)vtx_tri_tot,
                (unsigned long long)pixels_shaded);
            fflush(stderr);
        }

        /* GCN_GX_PIXEL_STATS: further split of the "tri" bucket above (triangle
         * scan/pixel) into BLOCK/SLOPE/TEX/COMB/BLEND — see gx_raster.c's big
         * comment above s_pixel_stats for the bucket definitions and the
         * alpha-test/late-Z boundary rationale. Separate knob from
         * GCN_GX_STATS (per-pixel rdtsc pairs are too expensive to leave on by
         * default), same print cadence, piggybacked onto this tick so there is
         * only one stats print site to keep in sync (gx_raster_get_pixel_stats). */
        GxPixelStats ps;
        gx_raster_get_pixel_stats(&ps);
        u64 ps_tot = ps.tsc_block + ps.tsc_slope + ps.tsc_tex + ps.tsc_comb + ps.tsc_blend;
        if (ps.shaded > 0u && ps_tot > 0u) {
            /* texel_cache_hit%: hit rate of the per-draw decode_texel memo
             * (gx_raster.c's "Per-draw texel cache" comment, above tex_sample)
             * — hits+misses covers every decode_texel call tex_sample makes
             * (4 bilinear taps or 1 point tap per sample). */
            u64 tc_tot = ps.texel_cache_hits + ps.texel_cache_misses;
            fprintf(stderr,
                "[gx-pixel-stats] block=%.1f%% slope=%.1f%% tex=%.1f%% comb=%.1f%% blend=%.1f%%"
                "  | tex_calls=%llu linear=%llu point=%llu earlyz_rejected=%llu shaded=%llu"
                " blend_writes=%llu texel_cache_hits=%llu texel_cache_misses=%llu"
                " texel_cache_hit%%=%.1f%%\n",
                100.0 * (double)ps.tsc_block / (double)ps_tot,
                100.0 * (double)ps.tsc_slope / (double)ps_tot,
                100.0 * (double)ps.tsc_tex   / (double)ps_tot,
                100.0 * (double)ps.tsc_comb  / (double)ps_tot,
                100.0 * (double)ps.tsc_blend / (double)ps_tot,
                (unsigned long long)ps.tex_calls, (unsigned long long)ps.tex_linear,
                (unsigned long long)ps.tex_point, (unsigned long long)ps.earlyz_rejected,
                (unsigned long long)ps.shaded, (unsigned long long)ps.blend_writes,
                (unsigned long long)ps.texel_cache_hits, (unsigned long long)ps.texel_cache_misses,
                tc_tot > 0u ? 100.0 * (double)ps.texel_cache_hits / (double)tc_tot : 0.0);
            fflush(stderr);
        }

        /* GCN_GX_TEV_CENSUS: per-config draw/pixel counters (no-op when off —
         * the knob lives in gx_raster.c beside its data). */
        gx_raster_print_census();

        /* GCN_GX_STATS: per-triangle bbox-area histogram (no-op when off —
         * same convention as the census above). */
        gx_raster_print_area_hist();

        /* GCN_GX_MT_STATS: GX-MT fork/join accounting (no-op when off; does
         * not force serial — main-thread-only counters). */
        gx_raster_print_mt_stats();
    }
}
