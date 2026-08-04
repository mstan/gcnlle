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
#include "gx/gx_render.h"
#include "gx/gx_vulkan.h"   /* gx_vulkan_resident_busy — snapshot drain-assert */
#include "gp/gp.h"          /* GcnGp — snapshot drain-assert reads gp->count  */
#include "debug/rings.h"

#include <math.h>        /* fabsf — [gx-xfaudit] matrix-scale classification */
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
#define GX_BP_MASK             0xFEu

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
static u64 s_gx_frames;     /* accepted GXSetDrawDone writes (one per IPL frame) */
static u64 s_xfb_generation; /* completed frame publication sequence */

/* Display-list tear detector (always on; see the CALL_DL handler). The G3
 * pipeline executes CALL_DL on the worker while CP status tells the guest
 * those bytes were already consumed, so the guest may legally rewrite the
 * list under us. Hash the DL bytes before and after execution: a mismatch
 * proves the source was rewritten MID-EXECUTION (a rewrite completing
 * between push and execution still hashes clean — this detector bounds the
 * class from below, it does not clear it). In synchronous GX no guest code
 * runs during the decode, so a mismatch is impossible and the check is
 * free of false positives. */
static u64 s_dl_execs;      /* hashed CALL_DL executions */
static u64 s_dl_tears;      /* pre/post hash mismatches */
static u32 s_dl_max_bytes;  /* largest DL seen (sizing data for a snapshot fix) */

/* Per-frame draw log (companion to gx_raster's coverage-anomaly detector):
 * one entry per PRIMITIVE decoded, RLE-dumped when the frame's coverage is
 * anomalous. Distinguishes "one draw with a runaway vertex count" from
 * "the same draw command repeated" — the two remaining shapes for the IPL
 * flood after vertex DATA was proven sane. Decode-thread only. */
typedef struct { u32 prim, vat, nverts, vsize, dl; } GxDrawLogEntry;
#define GX_DRAWLOG_CAP 8192u
static GxDrawLogEntry s_drawlog[GX_DRAWLOG_CAP];
static u32 s_drawlog_n;
static int s_drawlog_overflow;

static void gx_drawlog_dump(void) {
    static u64 s_dumps;
    if (++s_dumps > 200u) return;
    u32 lines = 0;
    fprintf(stderr, "[gx-drawlog] %u draws%s:\n", s_drawlog_n,
            s_drawlog_overflow ? " (TRUNCATED)" : "");
    for (u32 i = 0; i < s_drawlog_n && lines < 200u; lines++) {
        u32 j = i + 1;
        while (j < s_drawlog_n &&
               !memcmp(&s_drawlog[j], &s_drawlog[i], sizeof s_drawlog[i]))
            j++;
        fprintf(stderr, "  %4ux prim=%u vat=%u nverts=%u vsize=%u dl=%08X\n",
                j - i, s_drawlog[i].prim, s_drawlog[i].vat,
                s_drawlog[i].nverts, s_drawlog[i].vsize, s_drawlog[i].dl);
        i = j;
    }
    fflush(stderr);
}

/* ============================================================================
 * [gx-xfaudit] XF/BP write audit rings (always on; IPL flood investigation).
 *
 * The frame-anomaly instrument proved flood frames draw the cube DL
 * (0x00AF13C0) with cube-scale object vertices but a WALL-scale position
 * matrix (|linear|~22) and the room-wall TEV program — i.e. the per-instance
 * top-level state loads (LOAD_XF slot 0 + BP TEV block between CALL_DLs),
 * provably present with correct values in the pushed stream, did not take
 * effect. get_posmat reads s_xf[] live with no cache, so only three failure
 * shapes remain, and these rings distinguish them at the moment of the bad
 * draw:
 *   A. the LOAD_XF write never executed (decode skipped/mis-resumed it):
 *      the ring shows no top-level slot write between the room DL's interior
 *      write and the bad draw;
 *   B. the write executed but s_xf[] holds something else (write-handler /
 *      aliasing bug): the ring shows the write with a cube-scale payload
 *      while the dump of s_xf[] shows wall scale;
 *   C. the matrix INDEX is wrong (per-vertex PosMatIdx byte or a leaked CP
 *      MATINDEX default selects the wall's slot): the trigger dump's posMtx
 *      != the slot the instance loads target.
 * Recording is decode-thread-only (sync mode: CPU thread; pipeline mode: the
 * GX worker — never both at once), same single-writer model as s_drawlog.
 * The dump trigger lives in gx_raster.c prepare_position_transform (wall-
 * scale linear part applied while inside the cube DL) and calls
 * gcn_gx_state_audit_dump().
 * ==========================================================================*/
typedef struct {
    u64   frame;    /* s_gx_frames at write time (completed frames so far) */
    u32   dl;       /* DL executing the write, 0 = top-level stream */
    u16   addr;     /* XF target; 0xFFA0/0xFFB0 = CP MATINDEX_A/B loads */
    u16   count;    /* words written */
    u32   word0;    /* first payload word (raw) */
    float maxlin;   /* matrix-memory writes (<0x400): max |float| over the
                     * non-translation columns written — wall (~22) vs cube
                     * (<=2.2) classification without decoding word0 */
} GxXfAuditEntry;
#define GX_XFAUDIT_CAP 64u
static GxXfAuditEntry s_xfaudit[GX_XFAUDIT_CAP];
static u64 s_xfaudit_n;

typedef struct { u64 frame; u32 dl; u32 value; u8 cmd; } GxBpAuditEntry;
#define GX_BPAUDIT_CAP 256u
static GxBpAuditEntry s_bpaudit[GX_BPAUDIT_CAP];
static u64 s_bpaudit_n;

static void xfaudit_record(u16 addr, u16 count, u32 word0, float maxlin) {
    GxXfAuditEntry* e = &s_xfaudit[s_xfaudit_n & (GX_XFAUDIT_CAP - 1u)];
    e->frame = s_gx_frames;
    e->dl    = s_gx.cur_dl_addr;
    e->addr  = addr;
    e->count = count;
    e->word0 = word0;
    e->maxlin = maxlin;
    s_xfaudit_n++;
}

/* ============================================================================
 * [gx-fifoprov] staging-byte provenance (always on; IPL flood investigation).
 *
 * The [gx-xfaudit] rings proved the flood's per-instance slot-0 LOAD_XF
 * executes in order with a correct opcode/header/first-word but a garbage
 * payload tail — the corruption is IN THE BYTES the decoder consumed. Two
 * sources remain: the guest genuinely pushed those bytes (CPU-side bug —
 * e.g. interrupt clobbering the matrix computation), or the drain copied
 * bytes the gather pipe never pushed to that FIFO slot (rptr/wptr wrap or
 * accounting bug). The gather-pipe recorder ring (rings.c, always on) holds
 * the pushed truth per 32-byte slot; to compare against it we track, for
 * every chunk appended to the staging buffer by the SYNCHRONOUS drain path,
 * the guest phys address it was copied from:
 *   - chunk k (append order) covers stream bytes [k*32, k*32+32) and came
 *     from s_srcmap_phys[k & mask];
 *   - gx->buf[0] currently holds stream byte s_buf0_stream (advanced by
 *     `consumed` after every gx_run/memmove);
 *   - the command being decoded starts at staging offset s_cur_cmd_buf_off
 *     (set by gx_run only while decoding from the staging buffer).
 * The pipeline worker's pull path does NOT maintain this map (its bytes
 * arrive via the SPSC ring, one copy removed from guest RAM), so the
 * comparison is only attempted with the pipeline fully off — exactly the
 * flood's minimal repro config (GCN_GX_PIPELINE=0). Decode-thread only.
 * ==========================================================================*/
#define GX_SRCMAP_CAP (1u << 14)   /* 16384 chunks = 512 KiB of stream history */
static u32 s_srcmap_phys[GX_SRCMAP_CAP];
static u64 s_srcmap_chunks;        /* chunks appended by the sync drain */
static u64 s_buf0_stream;          /* stream offset of gx->buf[0] */
static u32 s_cur_cmd_buf_off = 0xFFFFFFFFu;   /* staging offset of the command
                                               * being decoded, else ~0 */

void gcn_gx_state_audit_dump(void) {
    u64 nx = s_xfaudit_n;
    u64 fx = nx > GX_XFAUDIT_CAP ? nx - GX_XFAUDIT_CAP : 0u;
    fprintf(stderr, "[gx-xfaudit] XF/MATIDX write ring (%llu total, showing #%llu..#%llu):\n",
            (unsigned long long)nx, (unsigned long long)fx,
            (unsigned long long)(nx ? nx - 1u : 0u));
    for (u64 i = fx; i < nx; i++) {
        const GxXfAuditEntry* e = &s_xfaudit[i & (GX_XFAUDIT_CAP - 1u)];
        float w0f;
        memcpy(&w0f, &e->word0, 4);
        fprintf(stderr,
                "  #%llu f=%llu dl=%08X addr=0x%04X n=%u w0=%08X(%.4g) maxlin=%.4g\n",
                (unsigned long long)i, (unsigned long long)e->frame, e->dl,
                e->addr, e->count, e->word0, (double)w0f, (double)e->maxlin);
    }
    u64 nb = s_bpaudit_n;
    u64 fb = nb > 48u ? nb - 48u : 0u;
    fprintf(stderr, "[gx-xfaudit] BP write ring (%llu total, showing #%llu..#%llu):\n",
            (unsigned long long)nb, (unsigned long long)fb,
            (unsigned long long)(nb ? nb - 1u : 0u));
    for (u64 i = fb; i < nb; i++) {
        const GxBpAuditEntry* e = &s_bpaudit[i & (GX_BPAUDIT_CAP - 1u)];
        fprintf(stderr, "  #%llu f=%llu dl=%08X reg=0x%02X val=0x%06X\n",
                (unsigned long long)i, (unsigned long long)e->frame, e->dl,
                e->cmd, e->value);
    }
    fflush(stderr);
}

static u64 dl_hash(const u8* p, u32 n) {
    u64 h = 1469598103934665603ull;
    while (n >= 8u) {
        u64 v;
        memcpy(&v, p, 8);
        h = (h ^ v) * 1099511628211ull;
        p += 8; n -= 8u;
    }
    while (n--)
        h = (h ^ *p++) * 1099511628211ull;
    return h;
}

/* Log a first-occurrence once; returns 1 the first time a flag is raised. */
static int note_once(u8* flag) {
    if (*flag) return 0;
    *flag = 1;
    return 1;
}

/* ============================================================================
 * G3: CPU/GX pipeline (default on; GCN_GX_PIPELINE=0 disables — see
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
 * on, loudly. The IPL also emits one top-level 4-vertex quad per frame, so
 * this is a recoverable sync-mode handoff: drain, execute that stretch on
 * the CPU, re-seed the scanner from the leftover, then resume pipelining.
 * A future unsizable command gets the same exact fallback, never a guess.
 *
 * Deliberate, gate-arbitrated residual (same class the DSP thread ships
 * with, and the same model Dolphin's dual-core uses): CALL_DL bytes,
 * indexed vertex arrays, textures and TLUT sources are read from guest RAM
 * at WORKER execution time, not producer push time. If the guest mutated
 * that memory between push and execution the result could differ from
 * synchronous consumption — the four golden XFB hashes (repeat-run
 * deterministic) + the oracle value+order diff are the arbiters, exactly
 * as they were for GCN_DSP_THREAD. EFB->XFB copies also land in guest RAM
 * from the worker: GXSetDrawDone's producer fence materializes a completed
 * XFB before the guest can flip VI to it; end-of-run/shutdown, debug RAM and
 * screenshot access, and fifo reset explicitly drain before observing state.
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
static volatile s32 s_pipe_epoch    = 0;   /* changed before every worker wake        */
static volatile s32 s_pipe_quit     = 0;
static volatile s32 s_pipe_failed   = 0;   /* worker cannot preserve command stream  */
static HANDLE       s_pipe_h        = NULL;
static int          s_pipe_on       = -1;  /* default on; GCN_GX_PIPELINE=0 disables */
static int          s_pipe_stats    = -1;  /* GCN_GX_PIPE_STATS=1 transition census */
static int          s_pipe_unsafe_resume = -1; /* GCN_GX_PIPE_UNSAFE_RESUME=1: baseline-
                                            * reproduction instrument only, see gx_pipe_on() */
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
static u64 s_pipe_sync_entries = 0;
static u64 s_pipe_resume_deferrals = 0;
static u64 s_pipe_resumes = 0;
static u64 s_pipe_poison_events = 0;
static u32 s_pipe_max_deferred_carry = 0;

/* The IPL normally renders successive fields into the same guest XFB. The GX
 * worker materializes that buffer row-by-row while VI runs on the CPU thread;
 * protect only those RAM accesses so scanout cannot publish a hybrid frame.
 * This is deliberately narrower than a pipeline drain. */
static SRWLOCK s_xfb_lock = SRWLOCK_INIT;

void gcn_gx_xfb_read_begin(void)  { AcquireSRWLockShared(&s_xfb_lock); }
void gcn_gx_xfb_read_end(void)    { ReleaseSRWLockShared(&s_xfb_lock); }
void gcn_gx_xfb_write_begin(void) { AcquireSRWLockExclusive(&s_xfb_lock); }
void gcn_gx_xfb_write_end(void)   { ReleaseSRWLockExclusive(&s_xfb_lock); }

/* GCN_GX_XFB_HASH=1: chained FNV-1a-64 over every byte ever written to the
 * guest XFB by an EFB->XFB copy, in publication order. This boundary (the
 * same s_xfb_lock-guarded region gcn_gx_xfb_write_begin/end already protect)
 * is the natural choice because:
 *   - it's the single point both the software-raster path (gx_raster.c) and
 *     the Vulkan resident-readback path (gx_vulkan.c) funnel through, so the
 *     hash is identical no matter which backend produced the pixels;
 *   - it hashes the FINAL bytes landing in guest RAM, so fused vs unfused
 *     shader programs and SIMD vs scalar filtering are transparent to it —
 *     they're compared by their output, not their code path;
 *   - it runs once per completed copy (not per draw call or per BP write),
 *     so it's deterministic across runs regardless of internal batching,
 *     thread scheduling, or timing;
 *   - it covers 100% of pixel output: every byte scanout/VI can ever read
 *     back out of the XFB passed through here first.
 * A running (chained) hash rather than a per-publication list because the
 * FNV-1a state itself already folds in every prior publication's bytes --
 * "chain = hash(prev_chain || these_bytes)" is exactly what continuing to
 * feed the same accumulator does. */
static int s_xfb_hash_on = -1;
static u64 s_xfb_hash_chain = 0xcbf29ce484222325ULL; /* FNV-1a-64 offset basis */
static u64 s_xfb_hash_pubs = 0;

static int gx_xfb_hash_on(void) {
    if (s_xfb_hash_on < 0) {
        const char* e = getenv("GCN_GX_XFB_HASH");
        s_xfb_hash_on = (e && e[0] == '1') ? 1 : 0;
    }
    return s_xfb_hash_on;
}

void gcn_gx_xfb_hash_feed(const u8* base, u32 stride, u32 row_bytes, u32 rows) {
    if (!gx_xfb_hash_on())
        return;
    u64 h = s_xfb_hash_chain;
    for (u32 y = 0; y < rows; y++) {
        const u8* row = base + (u64)y * stride;
        for (u32 x = 0; x < row_bytes; x++) {
            h ^= row[x];
            h *= 0x100000001b3ULL; /* FNV-1a-64 prime */
        }
    }
    s_xfb_hash_chain = h;
}

void gcn_gx_xfb_hash_publish_done(void) {
    if (!gx_xfb_hash_on())
        return;
    s_xfb_hash_pubs++;
    /* First publication plus every 256th thereafter: enough breadcrumbs to
     * bisect a divergence to a <=256-publication window without printing a
     * line per frame. */
    if (s_xfb_hash_pubs == 1 || (s_xfb_hash_pubs % 256) == 0)
        fprintf(stderr, "[gx-xfb-hash] publication=%llu chain=%016llx\n",
                (unsigned long long)s_xfb_hash_pubs,
                (unsigned long long)s_xfb_hash_chain);
}
u64 gcn_gx_xfb_generation(void) {
    return __atomic_load_n(&s_xfb_generation, __ATOMIC_ACQUIRE);
}

u64 gcn_gx_frame_count(void) { return s_gx_frames; }

/* Guest address of the display list currently executing (0 = top-level
 * stream). Provenance stamp for gx_raster's coverage-anomaly census. */
u32 gcn_gx_current_dl(void) { return s_gx.cur_dl_addr; }

static DWORD WINAPI gx_pipe_worker(LPVOID param);

/* A worker failure cannot safely fall back to synchronous decode: CP already
 * consumed the queued bytes, and some may remain only in the worker staging
 * buffer. Make the invariant breach loud instead of losing commands or
 * waiting forever on an offset the exited worker can never publish. */
static void gx_pipe_abort_if_failed(void) {
    if (__atomic_load_n(&s_pipe_failed, __ATOMIC_ACQUIRE)) {
        fprintf(stderr, "gx-pipe: fatal worker failure; command stream cannot continue\n");
        fflush(stderr);
        abort();
    }
}

static int gx_pipe_on(void) {
    if (s_pipe_on < 0) {
        const char* stats = getenv("GCN_GX_PIPE_STATS");
        s_pipe_stats = (stats && stats[0] == '1') ? 1 : 0;
        /* GCN_GX_PIPE_UNSAFE_RESUME=1: baseline-reproduction instrument for A/B
         * measurement against the known-buggy pre-fix resume gate (carry_len <=
         * capacity, no gather-size headroom). Never for normal use — it can
         * poison the pipeline, which is the whole point of reproducing it. */
        const char* unsafe = getenv("GCN_GX_PIPE_UNSAFE_RESUME");
        s_pipe_unsafe_resume = (unsafe && unsafe[0] == '1') ? 1 : 0;
        if (s_pipe_unsafe_resume)
            fprintf(stderr, "[gx-pipe] GCN_GX_PIPE_UNSAFE_RESUME=1: "
                            "baseline-reproduction mode, resume gate is the "
                            "known-buggy policy (carry overflow possible)\n");
        /* Default ON after the finalized implementation held all four golden
         * XFB hashes, both oracle counts, repeated-run determinism, and showed
         * a 15.5% average whole-boot win in an interleaved derived A/B. Keep a
         * synchronous escape hatch for diagnostics and future FIFO shapes. */
        const char* e = getenv("GCN_GX_PIPELINE");
        s_pipe_on = (e && e[0] == '0') ? 0 : 1;
        if (s_pipe_on) {
            s_pipe_h = CreateThread(NULL, 0, gx_pipe_worker, NULL, 0, NULL);
            if (!s_pipe_h) {
                fprintf(stderr, "gx-pipe: CreateThread failed; using synchronous GX\n");
                s_pipe_on = 0;
            }
        }
    }
    gx_pipe_abort_if_failed();
    return s_pipe_on && !s_pipe_poisoned && !s_pipe_syncmode;
}

/* Wait until the worker has retired through `target` produced-bytes. */
static void gx_pipe_wait_retired(s64 target) {
    int spins = 0;
    for (;;) {
        gx_pipe_abort_if_failed();
        s64 r = __atomic_load_n(&s_pipe_retired, __ATOMIC_ACQUIRE);
        if (r >= target) return;
        if (++spins < 16384) { _mm_pause(); continue; }
        s64 expect = r;
        __atomic_store_n(&s_pipe_waiting, 1, __ATOMIC_SEQ_CST);
        /* A worker-failure wake does not change s_pipe_retired, so it can race
         * between the failure check and this wait. Bound the sleep and recheck
         * s_pipe_failed rather than relying on an unlatched wake alone. */
        WaitOnAddress((volatile VOID*)&s_pipe_retired, &expect, sizeof expect, 100u);
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
static void gx_pipe_drain_worker(void) {
    if (s_pipe_on != 1) return;
    s64 target = __atomic_load_n(&s_pipe_produced, __ATOMIC_RELAXED);
    int spins = 0;
    for (;;) {
        gx_pipe_abort_if_failed();
        s64 d = __atomic_load_n(&s_pipe_done_upto, __ATOMIC_ACQUIRE);
        if (d >= target) return;
        if (++spins < 16384) { _mm_pause(); continue; }
        s64 expect = d;
        __atomic_store_n(&s_pipe_waiting, 1, __ATOMIC_SEQ_CST);
        WaitOnAddress((volatile VOID*)&s_pipe_done_upto, &expect, sizeof expect, 100u);
        __atomic_store_n(&s_pipe_waiting, 0, __ATOMIC_SEQ_CST);
    }
}

void gcn_gx_pipeline_drain(void) {
    gx_pipe_drain_worker();
    gx_render_flush();
}

/* SNAPSHOT_RESUME (docs/SNAPSHOT_RESUME.md) SAVE-side hard drain-assert — the
 * survey's concrete "fully drained" definition, turned into an actual check
 * rather than the best-effort drain-and-flush gcn_gx_pipeline_drain does.
 * Caller MUST call gcn_gx_pipeline_drain() (which also forces
 * gx_render_flush() -> gx_vulkan_resident_flush() when the resident Vulkan
 * backend is active) immediately before calling this: this function only
 * CHECKS, it never drives anything toward empty, so calling it without a
 * prior drain will spuriously fail on ordinary live-frame state.
 *
 * Returns 1 iff every condition holds (safe to serialize GX state with no
 * live pipeline work outstanding); 0 otherwise, with a human-readable reason
 * written to `why` (best-effort, snprintf-truncated to why_size; `why`/
 * why_size may be 0/NULL to skip the message). */
int gcn_gx_confirm_drained(GcnGp* gp, GcnCp* cp, char* why, size_t why_size) {
    if (gp && gp->count != 0) {
        if (why) snprintf(why, why_size,
            "gather pipe not empty (gp->count=%u)", gp->count);
        return 0;
    }
    if (cp && gcn_cp_fifo_rw_distance(cp) != 0) {
        if (why) snprintf(why, why_size,
            "CP FIFO not empty (rw_distance=%u)", gcn_cp_fifo_rw_distance(cp));
        return 0;
    }
    if (cp && gcn_cp_at_breakpoint(cp)) {
        if (why) snprintf(why, why_size, "CP is stuck at a breakpoint");
        return 0;
    }
    if (s_gx.buf_len != 0) {
        if (why) snprintf(why, why_size,
            "GX consumer staging buffer not empty (buf_len=%u)", s_gx.buf_len);
        return 0;
    }
    if (s_pipe_on == 1) {
        s64 produced  = __atomic_load_n(&s_pipe_produced, __ATOMIC_ACQUIRE);
        s64 done_upto = __atomic_load_n(&s_pipe_done_upto, __ATOMIC_ACQUIRE);
        if (done_upto != produced) {
            if (why) snprintf(why, why_size,
                "GX pipeline worker not caught up (done_upto=%lld produced=%lld)",
                (long long)done_upto, (long long)produced);
            return 0;
        }
        if (s_scan_carry_len != 0) {
            if (why) snprintf(why, why_size,
                "GX scanner carry not empty (%u bytes)", s_scan_carry_len);
            return 0;
        }
    }
    if (gx_vulkan_resident_busy()) {
        if (why) snprintf(why, why_size,
            "Vulkan resident backend has a batch in flight");
        return 0;
    }
    if (why && why_size) why[0] = 0;
    return 1;
}

void gcn_gx_pipeline_shutdown(void) {
    if (s_pipe_on == 1) {
        gcn_gx_pipeline_drain();
        __atomic_store_n(&s_pipe_quit, 1, __ATOMIC_RELEASE);
        __atomic_add_fetch(&s_pipe_epoch, 1, __ATOMIC_RELEASE);
        WakeByAddressAll((PVOID)&s_pipe_epoch);
        if (s_pipe_h) {
            WaitForSingleObject(s_pipe_h, INFINITE);
            CloseHandle(s_pipe_h);
            s_pipe_h = NULL;
        }
        s_pipe_on = 0;
    }
    if (s_gxstats == 1) {
        u64 total = s_gx_tsc[GX_STAT_FIFO] + s_gx_tsc[GX_STAT_DECODE] +
                    s_gx_tsc[GX_STAT_DRAW] + s_gx_tsc[GX_STAT_EFB];
        if (total)
            fprintf(stderr,
                    "[gx-final-stats] fifo=%.1f%% decode=%.1f%% draw=%.1f%% "
                    "efb=%.1f%% draws=%llu efbcopy=%llu\n",
                    100.0 * (double)s_gx_tsc[GX_STAT_FIFO] / (double)total,
                    100.0 * (double)s_gx_tsc[GX_STAT_DECODE] / (double)total,
                    100.0 * (double)s_gx_tsc[GX_STAT_DRAW] / (double)total,
                    100.0 * (double)s_gx_tsc[GX_STAT_EFB] / (double)total,
                    (unsigned long long)s_gx_draws,
                    (unsigned long long)s_gx_efbcopies);
        u64 vtx = 0, tri = 0, pixels = 0, draw_calls = 0;
        gx_raster_get_draw_stats(&vtx, &tri, &pixels, &draw_calls);
        if (vtx + tri)
            fprintf(stderr,
                    "[gx-final-draw-stats] draws=%llu vtx=%.1f%% tri=%.1f%% "
                    "pixels=%llu\n",
                    (unsigned long long)draw_calls,
                    100.0 * (double)vtx / (double)(vtx + tri),
                    100.0 * (double)tri / (double)(vtx + tri),
                    (unsigned long long)pixels);
        u64 cfg_hits = 0, cfg_misses = 0;
        gx_raster_get_config_cache_stats(&cfg_hits, &cfg_misses);
        fprintf(stderr, "[gx-config-cache] hit=%llu miss=%llu (%.1f%%)\n",
                (unsigned long long)cfg_hits,
                (unsigned long long)cfg_misses,
                cfg_hits + cfg_misses ?
                    100.0 * (double)cfg_hits / (double)(cfg_hits + cfg_misses) : 0.0);
        gx_raster_print_config_cache_detail();
        gx_raster_print_draw_shape_stats();
    }
    if (s_pipe_stats == 1)
        fprintf(stderr,
                "[gx-pipe-stats] sync=%llu defer=%llu resume=%llu poison=%llu "
                "max-deferred-carry=%u\n",
                (unsigned long long)s_pipe_sync_entries,
                (unsigned long long)s_pipe_resume_deferrals,
                (unsigned long long)s_pipe_resumes,
                (unsigned long long)s_pipe_poison_events,
                s_pipe_max_deferred_carry);
    gx_raster_print_cfg_verify_summary();
    fprintf(stderr, "gx: completed %llu IPL frames (GXSetDrawDone)\n",
            (unsigned long long)s_gx_frames);
    fprintf(stderr, "gx: DL tear census: %llu tears / %llu hashed CALL_DL "
                    "executions (max DL %u bytes)\n",
            (unsigned long long)s_dl_tears, (unsigned long long)s_dl_execs,
            s_dl_max_bytes);
    if (gx_xfb_hash_on())
        fprintf(stderr, "[gx-xfb-hash] publications=%llu chain=%016llx\n",
                (unsigned long long)s_xfb_hash_pubs,
                (unsigned long long)s_xfb_hash_chain);
    gx_render_shutdown();
}

/* GCN_GX_PIPE_LOCKSTEP=1: drain the worker after every pushed chunk. All
 * pipeline machinery still runs (scanner, fences, sync-mode handoffs,
 * re-seed) but producer/worker lag is pinned at zero, so every
 * late-guest-RAM-read tear window is closed while every bookkeeping path
 * still executes. Bisects "worker reads torn data" from "handoff/offset
 * bookkeeping bug" for pipeline-only rendering corruption. */
static int s_pipe_lockstep = -1;

static void gx_pipe_push_chunk(const u8* src) {
    gx_pipe_abort_if_failed();
    /* Ring-full backpressure (worker a full MiB behind): wait for space.
     * retired <= pulled, so gating space on retired is conservative. */
    s64 produced = s_pipe_produced;
    gx_pipe_wait_retired(produced + GCN_CP_GATHER_PIPE_SIZE - (s64)GX_PIPE_CAP);
    memcpy(s_pipe_ring + (produced & (GX_PIPE_CAP - 1)), src, GCN_CP_GATHER_PIPE_SIZE);
    __atomic_store_n(&s_pipe_produced, produced + GCN_CP_GATHER_PIPE_SIZE,
                     __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&s_pipe_epoch, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&s_pipe_parked, __ATOMIC_SEQ_CST))
        WakeByAddressAll((PVOID)&s_pipe_epoch);
    if (s_pipe_lockstep == -1) {
        const char* e = getenv("GCN_GX_PIPE_LOCKSTEP");
        s_pipe_lockstep = (e && e[0] == '1') ? 1 : 0;
        if (s_pipe_lockstep)
            fprintf(stderr, "gx-pipe: LOCKSTEP probe on — draining worker "
                            "after every chunk (zero-lag pipeline)\n");
    }
    if (s_pipe_lockstep)
        gx_pipe_drain_worker();
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
        s_pipe_poison_events++;
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
    /* This producer shares the rendered path's real-time audio deadline with
     * the emulation thread. Keep both above ordinary desktop/capture work;
     * the window consumer remains below-normal and coalesces frames. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    GcnGx* gx = &s_gx;
    s64 pulled = 0;
    for (;;) {
        s64 produced = __atomic_load_n(&s_pipe_produced, __ATOMIC_ACQUIRE);
        if (produced == pulled) {
            if (__atomic_load_n(&s_pipe_quit, __ATOMIC_ACQUIRE)) return 0;
            s32 epoch = __atomic_load_n(&s_pipe_epoch, __ATOMIC_ACQUIRE);
            int spins = 0;
            for (;;) {
                produced = __atomic_load_n(&s_pipe_produced, __ATOMIC_ACQUIRE);
                if (produced != pulled ||
                    __atomic_load_n(&s_pipe_quit, __ATOMIC_ACQUIRE)) break;
                if (++spins < 16384) { _mm_pause(); continue; }
                s32 expect = epoch;
                __atomic_store_n(&s_pipe_parked, 1, __ATOMIC_SEQ_CST);
                WaitOnAddress((volatile VOID*)&s_pipe_epoch, &expect,
                              sizeof expect, INFINITE);
                __atomic_store_n(&s_pipe_parked, 0, __ATOMIC_SEQ_CST);
                epoch = __atomic_load_n(&s_pipe_epoch, __ATOMIC_ACQUIRE);
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
            fflush(stderr);
            __atomic_store_n(&s_pipe_failed, 1, __ATOMIC_RELEASE);
            /* Published offsets intentionally do not advance. Wake every CPU
             * wait site so it observes s_pipe_failed and aborts loudly. */
            WakeByAddressAll((PVOID)&s_pipe_retired);
            WakeByAddressAll((PVOID)&s_pipe_done_upto);
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
    gx_pipe_drain_worker();
    s_pipe_sync_entries++;
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
    /* GCN_GX_PIPE_UNSAFE_RESUME=1 substitutes the old, known-buggy gate
     * (resume whenever the carry merely fits the buffer, with no headroom
     * reserved for the next gather) for baseline A/B reproduction only. */
    int can_resume = s_pipe_unsafe_resume
        ? (gx->buf_len <= sizeof s_scan_carry)
        : gcn_gx_pipeline_carry_can_resume(
              gx->buf_len, (u32)sizeof s_scan_carry,
              GCN_CP_GATHER_PIPE_SIZE);
    if (!can_resume) {
        s_pipe_resume_deferrals++;
        if (gx->buf_len > s_pipe_max_deferred_carry)
            s_pipe_max_deferred_carry = gx->buf_len;
        if (s_pipe_stats == 1 && s_pipe_resume_deferrals == 1u)
            fprintf(stderr,
                    "[gx-pipe-stats] first resume deferral carry=%u "
                    "next-gather=%u capacity=%u\n",
                    gx->buf_len, GCN_CP_GATHER_PIPE_SIZE,
                    (u32)sizeof s_scan_carry);
        return;
    }
    memcpy(s_scan_carry, gx->buf, gx->buf_len);
    s_scan_carry_len = gx->buf_len;
    s_scan_pos = __atomic_load_n(&s_pipe_produced, __ATOMIC_RELAXED) - (s64)gx->buf_len;
    s_pipe_resumes++;
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
    case GX_CP_MATINDEX_A:
        s->matrix_index_a = value;
        xfaudit_record(0xFFA0u, 1u, value, 0.0f);   /* [gx-xfaudit] index leak? */
        break;
    case GX_CP_MATINDEX_B:
        s->matrix_index_b = value;
        xfaudit_record(0xFFB0u, 1u, value, 0.0f);
        break;
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
/* [gx-fifoprov] Fired by gx_on_xf on a corrupt-looking top-level matrix load
 * (see the block comment at the srcmap statics). Prints the exact command
 * bytes the decoder consumed, then for every 32-byte FIFO slot the command
 * spanned, the bytes the gather pipe pushed to that slot (from the always-on
 * burst recorder) with the pushing pc/block — byte-for-byte verdict per
 * chunk — plus the recent interrupt/DMA event ring for timing correlation. */
static void gx_fifo_provenance_dump(GcnGx* gx, u16 address, u8 count,
                                    const u8* data, float maxlin) {
    static u32 s_dumps;
    static u64 s_hits;
    s_hits++;
    if (s_dumps >= 6u) {
        if ((s_hits & 63u) == 0u)
            fprintf(stderr, "[gx-fifoprov] %llu corrupt-payload hits total "
                            "(dumps capped)\n", (unsigned long long)s_hits);
        return;
    }
    s_dumps++;

    const u8* cmd = data - 5;        /* opcode + 32-bit header precede payload */
    u32 cmdlen = 5u + (u32)count * 4u;
    fprintf(stderr,
            "[gx-fifoprov] HIT #%llu: top-level LOAD_XF addr=0x%04X n=%u "
            "maxlin=%.4g frame=%llu rptr=%08X rw_dist=%u buf_off=%u\n",
            (unsigned long long)s_hits, address, count, (double)maxlin,
            (unsigned long long)s_gx_frames,
            gcn_cp_fifo_read_pointer(gx->cp), gcn_cp_fifo_rw_distance(gx->cp),
            s_cur_cmd_buf_off);
    fprintf(stderr, "[gx-fifoprov]   decoded:");
    for (u32 i = 0; i < cmdlen; i++)
        fprintf(stderr, "%s%02X", (i & 15u) == 0u ? "\n[gx-fifoprov]     " : " ",
                cmd[i]);
    fprintf(stderr, "\n");

    if (s_cur_cmd_buf_off == 0xFFFFFFFFu || s_pipe_on != 0) {
        fprintf(stderr, "[gx-fifoprov]   no source map (DL bytes or pipeline "
                        "mode) — cannot compare against pushed bursts\n");
    } else if (s_buf0_stream + gx->buf_len != s_srcmap_chunks * 32u) {
        fprintf(stderr, "[gx-fifoprov]   srcmap invariant broken "
                        "(buf0_stream=%llu buf_len=%u chunks=%llu) — skipped\n",
                (unsigned long long)s_buf0_stream, gx->buf_len,
                (unsigned long long)s_srcmap_chunks);
    } else {
        u64 s0 = s_buf0_stream + s_cur_cmd_buf_off;
        u64 s1 = s0 + cmdlen;
        for (u64 c = s0 & ~31ull; c < s1; c += 32u) {
            u64 k = c >> 5;
            if (k >= s_srcmap_chunks || s_srcmap_chunks - k > GX_SRCMAP_CAP) {
                fprintf(stderr, "[gx-fifoprov]   chunk stream=%llu: source "
                                "evicted from srcmap\n", (unsigned long long)c);
                continue;
            }
            u32 phys = s_srcmap_phys[k & (GX_SRCMAP_CAP - 1u)];
            u64 lo = c > s_buf0_stream ? c : s_buf0_stream;
            u64 hi = c + 32u;   /* staging holds through buf_len; hi <= s1 span */
            if (hi > s_buf0_stream + gx->buf_len) hi = s_buf0_stream + gx->buf_len;
            u8 pushed[32];
            u64 seq = 0, block = 0;
            u32 pc = 0;
            int found = gcn_ring_fifo_find(phys, &seq, &pc, &block, pushed);
            fprintf(stderr, "[gx-fifoprov]   chunk stream=%llu phys=%08X "
                            "(cmp bytes %llu..%llu):\n",
                    (unsigned long long)c, phys,
                    (unsigned long long)(lo - c), (unsigned long long)(hi - c));
            fprintf(stderr, "[gx-fifoprov]     staged:");
            for (u64 p = c; p < c + 32u; p++) {
                if (p < lo || p >= hi) fprintf(stderr, " ..");
                else fprintf(stderr, " %02X", gx->buf[p - s_buf0_stream]);
            }
            fprintf(stderr, "\n");
            if (!found) {
                fprintf(stderr, "[gx-fifoprov]     pushed: NO BURST RECORDED "
                                "for phys %08X\n", phys);
                continue;
            }
            fprintf(stderr, "[gx-fifoprov]     pushed:");
            for (u32 i = 0; i < 32u; i++) fprintf(stderr, " %02X", pushed[i]);
            int mismatch = 0;
            for (u64 p = lo; p < hi; p++)
                if (gx->buf[p - s_buf0_stream] != pushed[p - c]) mismatch = 1;
            fprintf(stderr, "\n[gx-fifoprov]     burst seq=%llu pc=%08X "
                            "blk=%llu -> %s\n",
                    (unsigned long long)seq, pc, (unsigned long long)block,
                    mismatch ? "MISMATCH (drain-side corruption)"
                             : "MATCH (guest pushed these bytes)");
        }
    }
    gcn_ring_event_dump_stderr(24);
    /* The corrupt payload was pushed by the IPL's psq upload loop at most a
     * tick before this decode (rw_dist is ~0 at every hit) — the last ~120
     * psq ops cover the corrupt matrix's load->store pairs plus context. */
    gcn_ring_psq_dump_stderr(120);
    /* Producer chain: every resident psq op that touched any of the corrupt
     * payload's exact bit patterns — the earliest ST of a corrupt word names
     * the guest routine that computed it. */
    {
        u32 words[16];
        u32 nw = count < 16u ? count : 16u;
        for (u32 i = 0; i < nw; i++) words[i] = rd32(&data[i * 4u]);
        gcn_ring_psq_value_trace(words, (int)nw, 80);
    }
    /* Write-watch ring: with GCN_WATCH armed on the corrupt source buffer,
     * the newest entries here are the LAST stores into it before this
     * corrupt upload — the writer of the corrupt lanes, by pc. */
    gcn_ring_watch_dump_stderr(700);
    /* The producer writes via TWO routines (watch-proven): the PSMTXCopy
     * loop at 0x81339F14..F40 (its LD eas name the corrupt SOURCE matrix)
     * and PSMTXConcat at 0x81339F44..0x8133A010 (its LDs are the a/b
     * operands — recompute d by hand to name the wrong ps op). 240 entries
     * reaches ~130 ops past the upload back through the corrupt copy. */
    gcn_ring_psq_dump_pc_range(0x81339F00u, 0x8133A014u, 240);
    /* Pinpoint: the newest psq store into the watched buffer +/- 30 ops —
     * the PSMTXCopy call that wrote the corrupt payload, WITH its paired
     * loads (= the corrupt source matrix address and values). */
    gcn_ring_psq_dump_around_watched_store(30);
}

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

    /* [gx-xfaudit] always-on write audit: payload word0 + (matrix mem only)
     * the max |float| across the non-translation columns being written, so
     * the ring itself classifies wall-scale vs cube-scale uploads. */
    {
        float maxlin = 0.0f;
        if (address < 0x400u) {
            for (u8 i = 0; i < count; i++) {
                if ((((u32)address + i) & 3u) == 3u)
                    continue;               /* translation column of a 3x4 row */
                u32 v = rd32(&data[i * 4u]);
                float f;
                memcpy(&f, &v, 4);
                float af = fabsf(f);
                if (af > maxlin) maxlin = af;
            }
        }
        xfaudit_record(address, count, count ? rd32(&data[0]) : 0u, maxlin);
        /* [gx-fifoprov] corrupt-payload detector: every clean top-level
         * matrix upload in the IPL menus stays below |0.7| in its
         * non-translation columns; the flood's corrupted uploads carry
         * screen-scale junk (hundreds..thousands). Threshold shared with the
         * [gx-xfaudit] draw-side trigger. */
        if (address < 0x100u && count >= 8u && gx->cur_dl_addr == 0u &&
            maxlin > 3.0f)
            gx_fifo_provenance_dump(gx, address, count, data, maxlin);
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

/* SNAPSHOT_RESUME (docs/SNAPSHOT_RESUME.md) SAVE-side accessors: the BP/XF
 * register files backing s_gx, read-only, for runtime/src/snapshot.c. Sizes
 * match cp.h/pe.h's own reg[]-exposure precedent (this struct just isn't a
 * GcnCp/GcnPe, it's the GX consumer's own mirror — see gx.c's GcnGx doc). */
const u32* gcn_gx_bp(void) { return s_gx.bp; }
const u32* gcn_gx_xf(void) { return s_gx.xf; }
u32 gcn_gx_xf_words(void) { return GX_XF_MEM_WORDS; }

/* SNAPSHOT_RESUME pass B (restore side): load-mirrors of the three
 * accessors above. Caller must have already confirmed the pipeline is
 * drained (same precondition gcn_gx_confirm_drained checks for a capture —
 * a restore happens before any dispatch runs, so this is trivially true,
 * but the ordering still matters relative to any lazily-created worker
 * thread: call these before gcn_gx_tick/gcn_gx_pipeline_drain are ever
 * invoked again). */
void gcn_gx_set_bp(const u32* bp) {
    if (bp) memcpy(s_gx.bp, bp, sizeof s_gx.bp);
}
void gcn_gx_set_xf(const u32* xf, u32 words) {
    if (!xf) return;
    if (words > GX_XF_MEM_WORDS) words = GX_XF_MEM_WORDS;
    memcpy(s_gx.xf, xf, (size_t)words * sizeof(u32));
}
void gcn_gx_set_tmem(const u8* data, u32 len) {
    if (!data) return;
    if (len > sizeof s_gx_tmem) len = (u32)sizeof s_gx_tmem;
    memcpy(s_gx_tmem, data, len);
}

static void gx_on_bp(GcnGx* gx, u8 cmd, u32 value) {
    /* Flipper BPMEM_BP_MASK is a one-shot write mask, not an ordinary state
     * register. Merge the next BP write with the old destination, then reset
     * the mask to all 24 payload bits. J3D relies on this for PE state: its
     * material DL writes 0x001FE7 before BLENDMODE specifically so the global
     * GXSetColorUpdate/GXSetAlphaUpdate bits survive the material update.
     * Treating the following payload as an unmasked replacement turns both
     * writes off and makes correctly shaded sky geometry write no EFB color. */
    {
        const u32 mask = gx->bp[GX_BP_MASK] & 0x00FFFFFFu;
        const u32 old = gx->bp[cmd] & 0x00FFFFFFu;
        value = ((old & ~mask) | (value & mask)) & 0x00FFFFFFu;
        if (cmd != GX_BP_MASK)
            gx->bp[GX_BP_MASK] = 0x00FFFFFFu;
    }

    if (note_once(&gx->seen_bp[cmd]))
        fprintf(stderr, "gx: BP reg 0x%02X first written (val 0x%06X)\n", cmd, value);

    /* [gx-xfaudit] always-on write audit (records identical-value rewrites
     * too — the notify-on-change gate below is itself under audit). */
    {
        GxBpAuditEntry* e = &s_bpaudit[s_bpaudit_n & (GX_BPAUDIT_CAP - 1u)];
        e->frame = s_gx_frames;
        e->dl    = gx->cur_dl_addr;
        e->value = value;
        e->cmd   = cmd;
        s_bpaudit_n++;
    }

    if (gx->bp[cmd] != value)
        gx_raster_notify_bp_write(cmd);
    gx->bp[cmd] = value;

    switch (cmd) {
    case GX_BP_SETDRAWDONE:            /* BPStructs.cpp:180-201 */
        if ((value & 0xFFu) == 0x02u) {
            gx_render_flush();
            /* All preceding EFB copies are now materialized in MEM1. Publish
             * this generation before PE finish lets VI present only complete
             * guest frames, never an intermediate copy from the same frame. */
            __atomic_add_fetch(&s_xfb_generation, 1u, __ATOMIC_RELEASE);
            gcn_pe_set_finish(gx->pe);
            ++s_gx_frames;
            /* Draw-log dumps ride the same opt-in as the frame-anomaly
             * printer (GCN_GX_FRAMEANOM=1): detection/rings stay always-on,
             * but menu transitions legitimately trip the extreme band
             * (their zoom IS a >3x coverage sweep), so default-on dumps
             * spam ordinary navigation now that the flood bug is fixed. */
            static int s_fa_dumps = -1;
            if (s_fa_dumps < 0) s_fa_dumps = getenv("GCN_GX_FRAMEANOM") ? 1 : 0;
            if (gx_raster_frame_anomaly_mark(s_gx_frames)) {
                if (s_fa_dumps) gx_drawlog_dump();
            } else if (s_fa_dumps && (s_gx_frames & 1023u) == 0u) {
                /* Periodic clean-frame reference dump: the anomaly dumps are
                 * only interpretable against a known-good frame's draw
                 * composition, which no anomaly-gated dump ever captures. */
                fprintf(stderr, "[gx-drawlog] CLEAN reference frame %llu:\n",
                        (unsigned long long)s_gx_frames);
                gx_drawlog_dump();
            }
            s_drawlog_n = 0;
            s_drawlog_overflow = 0;
        } else {
            fprintf(stderr, "gx: GXSetDrawDone ??? (val 0x%04X)\n", value & 0xFFFFu);
        }
        break;
    case GX_BP_PE_TOKEN_ID:            /* BPStructs.cpp:202-217 (no interrupt) */
        gx_render_flush();
        gcn_pe_set_token(gx->pe, (u16)(value & 0xFFFFu), 0);
        break;
    case GX_BP_PE_TOKEN_INT_ID:        /* BPStructs.cpp:218-233 (interrupt) */
        gx_render_flush();
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
            gx_render_efb_copy(&gx->cpst);
            s_gx_tsc[GX_STAT_EFB] += __rdtsc() - t0;
            s_gx_efbcopies++;
        } else {
            gx_render_efb_copy(&gx->cpst);
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
            u64 pre = dl_hash(cpu->ram + phys, size);
            if (size > s_dl_max_bytes) s_dl_max_bytes = size;
            gx_run(gx, cpu->ram + phys, size);
            s_dl_execs++;
            if (dl_hash(cpu->ram + phys, size) != pre) {
                s_dl_tears++;
                if (s_dl_tears <= 8u || (s_dl_tears & 1023u) == 0u)
                    fprintf(stderr,
                            "gx: DL TEAR #%llu — display list 0x%08X (%u bytes) "
                            "rewritten by the guest during execution "
                            "(frame %llu)\n",
                            (unsigned long long)s_dl_tears, addr, size,
                            (unsigned long long)s_gx_frames);
            }
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
            if (s_drawlog_n < GX_DRAWLOG_CAP) {
                GxDrawLogEntry* e = &s_drawlog[s_drawlog_n++];
                e->prim = prim; e->vat = vat; e->nverts = nverts;
                e->vsize = vsize; e->dl = gx->cur_dl_addr;
            } else {
                s_drawlog_overflow = 1;
            }
            /* Rasterize (SWVertexLoader -> TransformUnit -> Clipper ->
             * Rasterizer -> Tev). The payload is contiguous in `data`.
             * GCN_GX_STATS bucket 3 (DRAW): timed only at this call site — the
             * off path below is byte-identical work. */
            if (s_gxstats) {
                u64 t0 = __rdtsc();
                gx_render_draw(&gx->cpst, prim, vat, &data[3], nverts, vsize);
                s_gx_tsc[GX_STAT_DRAW] += __rdtsc() - t0;
                s_gx_draws++;
                s_gx_verts += nverts;
            } else {
                gx_render_draw(&gx->cpst, prim, vat, &data[3], nverts, vsize);
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
        /* [gx-fifoprov] staging offset of the command about to decode (valid
         * only for the top-level staging buffer; DL bytes have no FIFO slot).
         * A nested CALL_DL gx_run clears it; the next loop iteration here
         * re-derives it, so it is always correct at gx_run_command entry. */
        s_cur_cmd_buf_off = (data == gx->buf) ? off : 0xFFFFFFFFu;
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
    /* Hardware reset value: all 24 BP payload bits writable. A zero reset
     * would mask every BP command until the guest explicitly touched 0xFE. */
    s_gx.bp[GX_BP_MASK] = 0x00FFFFFFu;
    s_gx_frames = 0;
    __atomic_store_n(&s_xfb_generation, 0u, __ATOMIC_RELAXED);
    s_gx.cpu = cpu;
    s_gx.cp  = cp;
    s_gx.pe  = pe;
    /* Bind the persistent BP/XF register files + guest CPU into the rasterizer
     * (they never move) and clear the EFB model. */
    gx_render_init(cpu, s_gx.bp, s_gx.xf);
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
        /* [gx-fifoprov] source map: only meaningful with the pipeline fully
         * off (the worker's pull path appends without recording, which would
         * desync the stream/chunk invariant — see the srcmap block comment). */
        if (s_pipe_on == 0) {
            s_srcmap_phys[s_srcmap_chunks & (GX_SRCMAP_CAP - 1u)] = phys;
            s_srcmap_chunks++;
        }

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
            s_buf0_stream += consumed;   /* [gx-fifoprov] buf[0]'s stream pos */
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
