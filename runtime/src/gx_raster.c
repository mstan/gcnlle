/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GX rasterizer (impl). Scoped transcription of Dolphin's Software video
 * backend. See include/gx/gx_raster.h for the transcription map and scope.
 *
 * Everything the IPL menu frame does not exercise traps loudly (one-time
 * stderr) rather than silently guessing (PRINCIPLES: transcribe, never invent;
 * make misses loud).
 */
#include "gx/gx_raster.h"
#include "gx/gx.h"       /* gcn_gx_tmem() — TLUT reads for paletted textures */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>      /* getenv — GCN_GX_STATS cached read, see below */
#include <string.h>
#include <x86intrin.h>   /* __rdtsc — GCN_GX_STATS attribution only */
#include <emmintrin.h>   /* SSE2 — EFB-copy scanline encode (GCN_GX_NO_SIMD knob) */
#include <immintrin.h>   /* AVX2 — efb_clear_rect / EFB-copy scanline encode 8-wide
                          * widening (GCN_GX_NO_AVX2 knob). This TU is compiled
                          * WITHOUT -mavx2 (see build files); every function that
                          * touches an __m256i/AVX2 intrinsic below carries its own
                          * __attribute__((target("avx2"))), so no AVX2 instruction
                          * can leak into code that might run on a pre-AVX2 CPU. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>     /* worker pool: CreateThread + WaitOnAddress/WakeByAddressAll
                          * (tile-parallel rasterizer — see the GX-MT block comment
                          * above gx_mt_worker) */

/* ============================================================================
 * Tile-parallel rasterizer (GX-MT) — compile-time worker ceiling. The actual
 * count is resolved once per process from GCN_GX_THREADS (see gx_mt_resolve):
 *   unset/0 -> auto (logical cores / 2, capped at 8)
 *   1       -> serial (the same-binary A/B knob: identical pixel math, one
 *              thread, byte-identical XFB by construction)
 *   N       -> N threads (main participates; N-1 spawned workers), <= this cap.
 * Every piece of per-pixel mutable state is per-worker (s_tev_w / s_rb_w /
 * s_texel_cache_w below); everything else the pixel path touches during a
 * forked scan is either read-only for the duration of the draw (s_cfg, s_bp,
 * s_xf, slopes, guest RAM textures — the CPU is stalled inside the dispatch
 * loop while gx_raster_draw runs) or per-pixel-location EFB state whose rows
 * are owned by exactly one worker (interleaved block rows, see
 * scan_block_rows). Fork/join is per TRIANGLE, so cross-triangle write order
 * at any EFB location is the serial order by construction.
 * ==========================================================================*/
#define GX_MT_MAX 16

/* GCN_GX_NO_SIMD=1: force the EFB-copy scanline encode (gx_raster_efb_copy)
 * to always take its scalar path, even when the SIMD preconditions hold.
 * Own lazy -1-sentinel getenv, same pattern as s_no_fused (see its comment).
 * This is the knob the SIMD task's same-binary A/B exactness proof toggles:
 * SIMD-on and GCN_GX_NO_SIMD=1 must produce the identical golden XFB hash. */
static int s_no_simd = -1;

/* GCN_GX_NO_AVX2=1: force the AVX2 8-wide widenings below (efb_clear_rect's
 * color/depth clear passes; gx_raster_efb_copy's vertical-filter+YUV-encode,
 * chroma-smooth, and YUYV-pack passes) to fall back to the existing SSE2
 * 4-wide paths, even on a CPU that supports AVX2. Own lazy -1-sentinel
 * getenv, same pattern as s_no_simd above — this is the knob the AVX2
 * widening task's same-binary A/B exactness proof toggles (AVX2 vs
 * GCN_GX_NO_AVX2=1, same executable, golden XFB hash must match either way —
 * and must ALSO match GCN_GX_NO_SIMD=1's fully-scalar hash, a three-way A/B).
 * Independent of GCN_GX_NO_SIMD: NO_SIMD=1 is checked first at every call
 * site and forces fully scalar regardless of this knob; NO_AVX2=1 alone
 * still takes the SSE2 SIMD path, just not the wider one.
 *
 * s_cpu_avx2: one-time runtime CPUID check (gcc __builtin_cpu_supports),
 * cached the same -1-sentinel way, so an older CPU (no AVX2) auto-falls back
 * to the SSE2 path with no env var needed. Every AVX2 instruction in this
 * file lives ONLY inside functions carrying __attribute__((target("avx2")))
 * — this whole translation unit is built WITHOUT -mavx2 — so a pre-AVX2 CPU
 * simply never calls into them; gx_avx2_available() is the single gate every
 * call site below checks before doing so. efb_clear_rect and
 * gx_raster_efb_copy both run on the main thread only (FIFO-serialized BP
 * command dispatch, not a GX-MT per-triangle worker path — see the "Per-draw
 * config cache" comment above), so these lazy-init sentinels need no
 * synchronization, same reasoning as s_no_simd/s_no_fused. */
static int s_no_avx2 = -1;
static int s_cpu_avx2 = -1;
static inline int gx_avx2_available(void) {
    if (s_no_avx2 < 0) s_no_avx2 = getenv("GCN_GX_NO_AVX2") ? 1 : 0;
    if (s_cpu_avx2 < 0) {
        __builtin_cpu_init();
        s_cpu_avx2 = __builtin_cpu_supports("avx2") ? 1 : 0;
    }
    return !s_no_avx2 && s_cpu_avx2;
}

/* EFB geometry (VideoCommon/VideoCommon.h:15-16). */
#define EFB_WIDTH   640u
#define EFB_HEIGHT  528u

/* ============================================================================
 * GCN_GX_STATS=1 (same knob gx.c reads — see its GX_STAT_DRAW bucket comment):
 * a further split of THAT bucket's own time between (a) vertex load+transform+
 * clip (SWVertexLoader -> TransformUnit -> Clipper's trivial-reject/cull/clip,
 * everything in gx_raster_draw except actual scan conversion) and (b) triangle
 * scan/pixel (Rasterizer.cpp's edge-function loop + per-pixel Tev::Draw),
 * plus a pixels_shaded counter incremented wherever tev_draw() actually runs.
 * gx.c reads these via gx_raster_get_draw_stats() and prints them as a
 * "[gx-draw-stats]" line at the same 2^20-tick cadence as its own summary.
 *
 * Own cached getenv (own translation unit, same lazy -1 sentinel pattern as
 * gx.c's s_gxstats) — this file has no visibility into gx.c's static. Zero
 * cost when off: every timed site below is a single untaken
 * `if (s_draw_stats)` branch, no rdtsc, no counter writes, identical
 * rasterizer behavior either way. */
static int s_draw_stats = -1;
static u64 s_tsc_vtx;          /* vertex load+transform+clip (per gx_raster_draw call) */
static u64 s_tsc_tri;          /* triangle scan/pixel (draw_triangle, incl. all its pixels) */
static u64 s_pixels_shaded;    /* tev_draw() invocations (one per shaded pixel candidate) */
static u64 s_draw_calls_stat;  /* gx_raster_draw calls counted (vtx bucket samples) */

/* Further split of gx.c's own GX_STAT_EFB bucket (timed as a whole around
 * the gx_raster_efb_copy call, which does copy-encode THEN `if (clear)
 * efb_clear_rect()` — see that function's tail): how much of the EFB
 * bucket's wall is the scanline copy/YUV-encode vs the scalar clear-rect
 * fill. Sizing input for the efb_clear_rect SIMD task (residuals sweep) —
 * same "measure before implementing" discipline as GCN_GX_TEV_CENSUS was for
 * the fused-pixel task. Own accumulator, gated on the SAME s_draw_stats
 * knob/env var gx.c's GX_STAT_EFB timing already uses (GCN_GX_STATS), timed
 * only around the efb_clear_rect() call itself — the copy-encode share is
 * then just (EFB bucket total, already tracked in gx.c) minus this. Zero
 * cost when off: one untaken `if (s_draw_stats)` branch at the one call
 * site, no rdtsc, no counter writes — same contract as every other stat
 * knob in this file. */
static u64 s_tsc_efb_clear;
static u64 s_efb_clear_calls;

/* Per-triangle scissored-bbox-area histogram (same GCN_GX_STATS knob): bucket
 * k holds triangles whose post-scissor bounding-box area is in [2^(k-1), 2^k)
 * pixels (bucket 0 = degenerate/empty-bbox early returns), with each bucket's
 * triangle count, shaded-pixel count, and rdtsc wall. This is the sizing
 * input for the tile-parallel rasterizer's fork threshold: it answers "what
 * fraction of scan/pixel WALL lives in triangles big enough to be worth a
 * fork/join" for any candidate area cutoff, not just one probe value —
 * observability that works for every threshold question, per CLAUDE.md.
 * EFB is 640x528 < 2^19, so 20 buckets cover every possible bbox. */
#define GX_AREA_HIST_BUCKETS 20
static u64 s_hist_tris[GX_AREA_HIST_BUCKETS];
static u64 s_hist_pixels[GX_AREA_HIST_BUCKETS];
static u64 s_hist_tsc[GX_AREA_HIST_BUCKETS];
static u32 s_last_tri_area;    /* draw_triangle_impl -> draw_triangle wrapper */

/* ============================================================================
 * GCN_GX_PIXEL_STATS=1: a SEPARATE knob from GCN_GX_STATS above, with its own
 * cached -1-sentinel getenv (s_pixel_stats) and its own accumulators. This is
 * deliberate, not an oversight: per-pixel rdtsc pairs (one per tex_sample call,
 * one per tev_draw call, ...) add ~30-50% overhead inside the hottest loop in
 * the whole runtime (triangle scan is 96.9% of the GX draw bucket), which would
 * silently distort every GCN_GX_STATS share if the two knobs shared a branch.
 * Enable one or the other, never both in the same run.
 *
 * Splits GCN_GX_STATS' own "tri" bucket (draw_triangle / triangle scan+pixel)
 * five ways:
 *   BLOCK  build_block(): per-2x2-block setup (UV/interp/lod prep), timed as a
 *          whole at its single call site in draw_triangle_impl.
 *   SLOPE  raster_pixel's work strictly BEFORE tev_draw: z slope eval,
 *          early-Z test, color slope evals, UV fixed-point conversion. Timed
 *          over the whole raster_pixel_prep() call (whether it early-returns
 *          on an early-Z reject or falls through to tev_draw) so the bucket
 *          reflects real wall time regardless of outcome.
 *   TEX    tex_sample() total, timed at its one call site inside tev_draw's
 *          stage loop (covers decode_texel/wrap_coord/bilinear filter).
 *   COMB   the rest of tev_draw: per-stage konst/ras/combiner math (color_arg,
 *          alpha_arg, draw_color_regular/compare, draw_alpha_regular/compare)
 *          through alpha_test(). Computed
 *          as tev_draw's own wall time minus the nested TEX delta — same
 *          before/after accumulator-subtraction technique gx.c's DECODE bucket
 *          uses to isolate itself from DRAW/EFB.
 *   BLEND  everything from the moment alpha_test() PASSES onward: late-Z
 *          (EmulatedZ::Late — it lives here, after alpha test, not in SLOPE)
 *          and BlendTev. Timed as a whole around the blend_stage() call, which
 *          holds exactly that segment (its own early-return on a late-Z reject
 *          still gets timed, mirroring the SLOPE approach for early-Z).
 * Boundary is deliberately at alpha_test()'s pass/fail branch: alpha test
 * itself is COMB (it's still shading math on the combiner's output), late-Z is
 * BLEND (it's a per-pixel EFB read gated on the shaded result, same family as
 * BlendTev's own EFB read-modify-write).
 *
 * Counters: tex_sample call count split bilinear(linear)/point, early-Z
 * rejected pixels (raster_pixel_prep returned "rejected"), shaded pixels
 * (tev_draw entries — own counter, mirrors s_pixels_shaded above but gated on
 * this knob instead of GCN_GX_STATS so the two never cross-contaminate), and
 * blend writes (blend_stage actually reached BlendTev, i.e. passed alpha test
 * AND late-Z).
 *
 * Zero cost when off: every timed site is a single untaken `if (s_pixel_stats)`
 * branch, no rdtsc, no counter writes, identical rasterizer behavior either
 * way — same contract as s_draw_stats above. gx.c reads these via
 * gx_raster_get_pixel_stats() and prints them as a "[gx-pixel-stats]" line at
 * the same 2^20-tick cadence as "[gx-stats]"/"[gx-draw-stats]".
 * ==========================================================================*/
static int s_pixel_stats = -1;
static u64 s_tsc_block;          /* build_block (per 2x2 block) */
static u64 s_tsc_slope;          /* raster_pixel pre-tev_draw work */
static u64 s_tsc_tex;            /* tex_sample total (nested inside COMB's wall) */
static u64 s_tsc_comb;           /* tev_draw minus nested TEX and BLEND deltas */
static u64 s_tsc_blend;          /* late-Z (if present) + BlendTev */
static u64 s_ps_tex_calls;       /* tex_sample invocations */
static u64 s_ps_tex_linear;      /* ...of which bilinear (TextureLinear[stage]) */
static u64 s_ps_tex_point;       /* ...of which point-sampled */
static u64 s_ps_earlyz_rejected; /* raster_pixel_prep: early-Z rejected */
static u64 s_ps_shaded;          /* tev_draw entries (own counter, see above) */
static u64 s_ps_blend_writes;    /* blend_stage reached BlendTev */

/* ============================================================================
 * GCN_GX_TEV_CENSUS=1: census of distinct per-draw shading configurations.
 * Decides whether a fused specialized pixel path is worth building: if a
 * handful of configs cover ~all shaded pixels, hand-fusing them (identical
 * math, per-pixel selector switches folded to direct loads) is a large exact
 * win; if configs are diverse, SIMD batching is the better attack. The
 * signature hashes every field that determines the PIXEL-PATH CODE SHAPE
 * (stage combiner words + enables + channel selection, alpha-test word,
 * z/blend/dest-alpha state, counts) — NOT geometry or texture addresses,
 * which vary per draw without changing the code path. A new config dumps its
 * full field set once (see build_draw_cfg's census tail); per-config draw and
 * shaded-pixel counters print on the shared cadence via
 * gx_raster_print_census. Diagnostic only; default off; one untaken branch
 * per draw (and one per pixel) when off. `fused_pixels` (added for the
 * fused-pixel-path perf task) counts, per bucket, how many of that bucket's
 * `pixels` were shaded via a fused_pixel_A/B/C specialization instead of the
 * general tev_draw() — the coverage number the task's VERIFY step asks for,
 * gated on this same knob so it costs nothing when census is off. */
#define GX_CENSUS_MAX 128
typedef struct { int used; u32 hash, program_id; u64 draws, pixels, fused_pixels; } GxCensusEntry;
static int s_tev_census = -1;
static GxCensusEntry s_census[GX_CENSUS_MAX];
static int s_census_cur = -1;   /* index of the current draw's config, else -1 */

/* Table-full accounting: when a new shape shows up after all GX_CENSUS_MAX
 * slots are taken, the draw has nowhere to be tallied. Rather than silently
 * dropping it (the old behavior), count the draws and the distinct unseen
 * shapes so gx_raster_print_census can flag an insufficient table instead of
 * quietly under-reporting coverage. Table size stays GX_CENSUS_MAX; this is
 * a separate, generously-sized dedup set for shapes that didn't fit. */
#define GX_CENSUS_OVERFLOW_MAX 256
static u32 s_census_overflow_hashes[GX_CENSUS_OVERFLOW_MAX];
static int s_census_overflow_shapes = 0;  /* distinct unseen shapes (capped) */
static u64 s_census_overflow_draws = 0;   /* draws whose shape didn't fit */
static u64 s_ps_texel_cache_hits;   /* per-draw texel cache: decode_texel calls it satisfied */

/* GCN_GX_NO_FUSED=1: force every draw's fused-pixel-path selection (see
 * build_draw_cfg's tail, near fused_pixel_A/B/C) to stay off, i.e. always
 * fall back to the general tev_draw(). Own lazy -1-sentinel getenv, same
 * pattern as s_tev_census/s_draw_stats/s_pixel_stats above — this is the
 * knob the task's same-binary A/B exactness proof toggles (fused vs general,
 * same executable, XFB hash must match either way). Independent of
 * GCN_GX_TEV_CENSUS: fused selection runs on every draw regardless of whether
 * the census is being collected. */
static int s_no_fused = -1;
static u64 s_ps_texel_cache_misses; /* ...calls that had to fall through to a real decode */

/* ---- one-time trap logging ------------------------------------------------ */
/* Atomic exchange rather than plain load/store: trap sites are reachable from
 * GX-MT worker threads (per-pixel/per-sample paths), and two workers hitting
 * the same first-ever trap concurrently must still print exactly once. */
static int trap_once(int* flag, const char* what) {
    if (__atomic_exchange_n(flag, 1, __ATOMIC_RELAXED)) return 0;
    fprintf(stderr, "gx_raster: UNIMPLEMENTED/out-of-scope: %s — trapped once\n",
            what);
    return 1;
}
#define TRAP(field, msg) do { static int field = 0; trap_once(&field, msg); } while (0)

/* Same one-time gate as TRAP, but logs the FULL parameters (register values,
 * VCD/VAT, draw context) that led to the trap, not just the label — so a
 * single capture run tells us exactly what to add next (PRINCIPLES: make
 * misses loud, transcribe never guess). */
#define TRAPF(field, fmt, ...) do { \
        static int field = 0; \
        if (!__atomic_exchange_n(&field, 1, __ATOMIC_RELAXED)) { \
            fprintf(stderr, "gx_raster: UNIMPLEMENTED/out-of-scope: " fmt \
                    " — trapped once\n", __VA_ARGS__); \
        } \
    } while (0)

/* Draw-call context for trap logs (set once per gx_raster_draw call so the
 * per-vertex/per-pixel trap sites below can report which draw triggered
 * them without threading extra parameters through every callee). */
static const GxCpState* s_trap_cp;
static u32 s_trap_vat, s_trap_prim;

/* ---- bound state ---------------------------------------------------------- */
static CPUState* s_cpu;
static const u32* s_bp;    /* u32[256] BP register file (raw written values) */
static const u32* s_xf;    /* u32[0x1058] XF memory (rd32 host-order words)  */

/* ============================================================================
 * EFB model. Dolphin keeps 3 color bytes + 3 depth bytes per pixel
 * (SWEfbInterface.cpp:24-38); we keep the packed color word Dolphin stores
 * (per pixel_format) and a 24-bit depth separately — the pixel-format pack/
 * unpack semantics are transcribed exactly (SWEfbInterface.cpp:40-228).
 * ==========================================================================*/
static u32 s_efb_color[EFB_WIDTH * EFB_HEIGHT];
static u32 s_efb_depth[EFB_WIDTH * EFB_HEIGHT];
static GxRasterTriangleSink s_triangle_sink;
static void* s_triangle_sink_user;

void gx_raster_efb_data(const u32** color, const u32** depth,
                        u32* width, u32* height) {
    if (color) *color = s_efb_color;
    if (depth) *depth = s_efb_depth;
    if (width) *width = EFB_WIDTH;
    if (height) *height = EFB_HEIGHT;
}

void gx_raster_efb_data_mutable(u32** color, u32** depth,
                                u32* width, u32* height) {
    if (color) *color = s_efb_color;
    if (depth) *depth = s_efb_depth;
    if (width) *width = EFB_WIDTH;
    if (height) *height = EFB_HEIGHT;
}

void gx_raster_set_triangle_sink(GxRasterTriangleSink sink, void* user) {
    s_triangle_sink = sink;
    s_triangle_sink_user = sink ? user : NULL;
}

/* Tev / EfbInterface component ordering (Tev.h:225-231, matches EfbInterface). */
enum { ALP_C = 0, BLU_C = 1, GRN_C = 2, RED_C = 3 };

/* ---- bitfield helpers ----------------------------------------------------- */
static inline u32 bits(u32 v, u32 pos, u32 n) { return (v >> pos) & ((n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1u)); }
static inline s32 sext(u32 v, u32 n) {          /* sign-extend n-bit field */
    u32 m = 1u << (n - 1);
    return (s32)((v ^ m) - m);
}
static inline float xf_f(u32 addr) { float f; u32 v = s_xf[addr]; memcpy(&f, &v, 4); return f; }

/* big-endian guest-RAM readers */
static inline float be_f32(const u8* p) {
    u32 v = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
    float f; memcpy(&f, &v, 4); return f;
}
static inline s16 be_s16(const u8* p) { return (s16)(((u16)p[0] << 8) | p[1]); }
static inline u16 be_u16(const u8* p) { return (u16)(((u16)p[0] << 8) | p[1]); }

/* PEControl.pixel_format (BPMemory.h:1734-1741 @ bp 0x43). */
enum { PF_RGB8_Z24 = 0, PF_RGBA6_Z24 = 1, PF_RGB565_Z16 = 2, PF_Z24 = 3 };
static u32 pixel_format(void) { return bits(s_bp[0x43], 0, 3); }

static inline u8 Convert6To8(u8 v) { return (u8)((v << 2) | (v >> 4)); }
/* LookUpTables.h: same N->8 bit replication for the other widths the vertex
 * color formats need. */
static inline u8 Convert3To8(u8 v) { return (u8)((v << 5) | (v << 2) | (v >> 1)); }
static inline u8 Convert4To8(u8 v) { return (u8)((v << 4) | v); }
static inline u8 Convert5To8(u8 v) { return (u8)((v << 3) | (v >> 2)); }

/* TEV register value (Tev.h TevRegister). Declared up here (ahead of its
 * original home beside the rest of the Tev state, further down) purely so the
 * per-draw config cache immediately below — which needs this type for its
 * pre-resolved per-stage StageKonst — can sit ahead of every pixel-path
 * function that reads the cache (GetPixelColor, ZCompare, BlendTev, ...). */
typedef struct { s16 r, g, b, a; } TColor;

/* Forward declaration only (full definition lives with the rest of the Tev
 * state, further down) so DrawCfg below can hold a function pointer typed
 * over it — see DrawCfg::fused (GCN_GX_TEV fused-pixel-path perf task). A
 * pointer to an incomplete struct type is legal in C; nothing here needs
 * Tev's layout, only its address. */
typedef struct Tev Tev;

/* ============================================================================
 * Per-draw config cache (perf). BP loads are separate FIFO commands that never
 * interleave with a draw's vertex payload — gx.c hands gx_raster_draw one
 * complete, contiguous vertex block per call, and gx_raster_efb_copy is its own
 * separate call — so every BP-derived value below is provably constant for the
 * whole call. Dolphin's own per-pixel code (Tev.cpp, Rasterizer.cpp,
 * TextureSampler.cpp) re-reads the BP words on every pixel because its
 * BPMemory is a live register file it must treat as volatile in the general
 * case; we run single-threaded with FIFO-serialized BP writes, so decoding
 * once at the top of each entry point and reusing the result for every pixel/
 * stage/sample it produces is exact, not an approximation (see gx_raster.h
 * scope note). Populated by build_draw_cfg() (gx_raster_draw) or the smaller
 * build_efb_cfg() (gx_raster_efb_copy); never touched mid-call after that.
 *
 * PURE DECODE MOTION ONLY: every field here is the same bits()/register read
 * the pixel path already did, just performed once instead of once per pixel.
 * The TRAP-guard conditions that depend only on these same BP-genmode fields
 * (indirect stages / z-texture / fog) move their *check* here too — same
 * trap-once semantics, evaluated once per draw instead of once per pixel
 * (CLAUDE.md gx-raster perf task). Traps whose firing still depends on
 * per-pixel data, or that weren't called out for hoisting, keep their
 * trap_once() call at the original per-pixel/per-sample site, just reading
 * the cached field instead of re-decoding it (paletted/unknown texture
 * format, texture-OOB, mipmap-filter, ras-color-channel, active-indirect).
 * ==========================================================================*/
typedef struct {
    /* order (bp 0x28+s2) fields, pre-split per stage — no more odd/even math
     * or s_bp reads at TEV-draw time (Tev.cpp SetupTextures/SetRasColor). */
    u32 texcoordSel, texmap, enable, colorchan;
    u32 tevind;           /* raw BP 0x10+stage indirect-stage descriptor */

    u32 cc, ac;           /* raw TevStageCombiner color/alpha words */

    /* color_arg/alpha_arg selectors (Tev.h GetColorInput/GetAlphaInput). */
    u32 argA, argB, argC, argD;      /* cc bits 12/8/4/0, 4 each */
    u32 aargA, aargB, aargC, aargD;  /* ac bits 13/10/7/4, 3 each */

    /* draw_color_{regular,compare} / draw_alpha_{regular,compare} fields.
     * Same physical bit ranges serve both regular and compare-mode encodings
     * (bias==3 in the c16/a16 field is Dolphin's compare-mode sentinel), so a
     * single set of cached extracts covers both call targets. */
    u32 c16, c18, c19, c20, c22;   /* cc: bias|n/a, op|cmp, clamp, scale|mode, dest */
    u32 a16, a18, a19, a20, a22;   /* ac: same layout for alpha */

    u32 tswap_id, rswap_id;   /* ac bits 2,2 / 0,2 -> index into s_cfg.swaptab */

    /* StageKonst is itself draw-invariant, not just its kcsel/kasel selector:
     * konst_lookup() only ever reads t->Konst[] (loaded once by
     * tev_load_registers before build_draw_cfg runs and never mutated again
     * during the draw) or fixed tables, so the resolved (r,g,b,a) can be
     * precomputed once per stage instead of re-run through konst_lookup()
     * every pixel (Tev.h AllTevKSels konst LUT). */
    TColor stage_konst;
} TevStageCfg;

typedef struct {
    /* tex_sample/decode_texel/wrap_coord/calc_lod fields (TextureSampler.cpp,
     * TextureDecoder_Common.cpp, Rasterizer.cpp calc_lod). `valid` false means
     * every sample from this unit is forced to (0,0,0,0) — the trap for it
     * still fires (once) at tex_sample's original per-sample call site. */
    u32 fmt;
    int w1, h1;                 /* width-1, height-1 (TX_SETIMAGE0) */
    u32 wrap_s, wrap_t;         /* TX_SETMODE0 bits 0-2, 2-2 */
    u32 magf, minf;             /* TX_SETMODE0 bits 4 (mag), 7 (min) */
    u32 mipmap_filter;          /* TX_SETMODE0 bits 5-2: none/point/linear */
    int lod_edge;               /* TX_SETMODE0 bit 8 */
    s32 lod_bias_half;          /* sext(TX_SETMODE0[16:9], 8) >> 1, precomputed */
    u32 minlod, maxlod;         /* TX_SETMODE1 bits 0-8 / 8-8 */
    u32 image0_raw, image3_raw; /* raw TX_SETIMAGE0/3 (for trap-log messages) */
    const u8* tlut;             /* palette base inside modeled TMEM (TX_SETTLUT
                                  * tmem_offset<<9); only meaningful for
                                  * C4/C8/C14X2 — stale-but-harmless otherwise */
    u32 tlutfmt;                /* TX_SETTLUT format: 0 IA8, 1 RGB565, 2 RGB5A3 */
    u32 img_base;               /* (image3_raw & 0xFFFFFF) << 5, UNMASKED (matches
                                  * the original inline trap-log expression) */
    u32 phys;                   /* img_base & 0x1FFFFFFF (the actual MEM1 offset) */
    int valid;                  /* s_cpu && s_cpu->ram && phys < ram_size */
    const u8* src;               /* s_cpu->ram + phys, only meaningful if valid */
    u32 src_len;                 /* ram_size - phys, only meaningful if valid   */
} TexUnitCfg;

typedef struct {
    /* GenMode (bp 0x00). */
    u32 numtexgens, numcolchans, numtevstages, cullmode;

    /* ZMode (bp 0x40) + PEControl.early_ztest (bp 0x43 bit 6). Draw-only (the
     * EFB-copy path never z-tests), unlike the shared quad below. */
    int zt_enable, zt_early;
    u32 zt_func;

    /* BlendMode (bp 0x41) fields BlendTev/BlendColor/LogicBlend need. Also
     * draw-only — color_update/alpha_update are shared (see s_bm_cu/s_bm_au
     * below; efb_clear_rect needs those too). */
    int bm_blend_enable, bm_logic_enable, bm_dither, bm_subtract;
    u32 bm_dst_factor, bm_src_factor, bm_logic_mode;

    /* ConstantAlpha / dstalpha (bp 0x42). */
    int da_enable;
    u8  da_alpha;

    u32 swaptab[4][4];   /* AllTevKSels swap tables (bp 0xF6+id*2 / +1), ids 0..3,
                           * pre-built once instead of rebuilt per pixel per stage. */

    TevStageCfg stage[16];   /* index = TEV stage number, 0..numtevstages       */
    TexUnitCfg  tex[8];      /* index = texture unit / texmap, always all 8     */

    /* Fused specialized per-pixel TEV path (perf; see the big comment above
     * fused_pixel_A/B/C, near tev_draw). NULL unless build_draw_cfg's tail
     * matched this draw's FULL shading config against one of those functions'
     * exact signatures bit-for-bit; raster_pixel() calls this instead of
     * tev_draw() when set. GCN_GX_NO_FUSED=1 forces this to always stay NULL
     * (same-binary A/B against the general path — see build_draw_cfg). */
    void (*fused)(Tev* t);

    /* 1 iff the selected `fused` function reads RasColor.r/g/b (Color[0][0..2])
     * -- true only for fused_pixel_D so far; fused_pixel_A/B/C's fused_core_C
     * only ever reads Color[0][3] (RasColor.a), which is why
     * raster_pixel_prep_fused's RGB-slope skip was safe for them (see its big
     * comment). fused_pixel_D's color fold consumes RasColor.rgb directly, so
     * raster_pixel_prep_fused must evaluate those 3 slopes too when this is
     * set -- gated per-draw instead of unconditionally, so A/B/C draws keep
     * the dead-work elision. Meaningless when `fused` is NULL. */
    int fused_needs_ras_rgb;

    /* Exact A--S renderer program selected from this same BP-derived config.
     * Cached with the config so thousands of unchanged tiny menu draws do not
     * re-walk every program signature. */
    u32 program_id;

    /* GX-MT: 1 iff this draw's pixel program is provably free of pixel-to-
     * pixel Tev-state carry, i.e. its pixels may be partitioned across
     * workers in any order with byte-identical results. Computed once per
     * draw by draw_parallel_ok() (see its proof comment); draws that fail
     * scan serially on worker 0 exactly as before. */
    int parallel_ok;
} DrawCfg;

static DrawCfg s_cfg;
static u64 s_bp_generation = 1;
static u64 s_cfg_bp_generation = 0;
static u64 s_cfg_cache_hits;
static u64 s_cfg_cache_misses;
static u64 s_draw_shapes[8][17]; /* nverts 0..15, 16=16+ */

/* Last observed draw for every shading-census bucket. This is deliberately a
 * fixed, append-only-by-bucket diagnostic surface: a late TCP query can still
 * inspect the full-screen movie draw even after later menu draws have replaced
 * live BP state. Texture bytes themselves remain in guest RAM and may be
 * reused, so capture their hash and a short sample at draw time as well as the
 * address. */
typedef struct {
    u32 mode0, mode1, image0, image3, tlut;
    u32 fmt, width, height, phys, bytes;
    u64 hash;
    u8 sample[32];
    u32 sample_len;
    int active, valid;
} GxDebugTexture;

typedef struct {
    int valid;
    u64 sequence, frame;
    u32 cpu_pc, dl, prim, vat, nverts, vstride;
    u32 vertex_bytes;
    u64 vertex_hash;
    u8 vertex_head[32], vertex_tail[32];
    u32 vertex_head_len, vertex_tail_len;
    u32 census_hash, program_id;
    u32 genmode, alpha_test;
    u32 zmode, blendmode, dstalpha, pecontrol;
    u32 numtexgens, numcolchans, numtevstages;
    u32 bp_tev_ra[4], bp_tev_bg[4];
    s16 tev_reg[4][4], tev_konst[4][4];
    u64 pixels;
    u64 alpha_tested, alpha_rejected, alpha_sum, last_tex_alpha_sum;
    u32 alpha_min, alpha_max, last_tex_alpha_min, last_tex_alpha_max;
    u64 blend_inputs, z_rejected, color_writes;
    u64 output_rgba_sum[4], efb_rgba_sum[4];
    u32 triangles_submitted, triangles_trivial_rejected, triangles_culled;
    u32 triangles_clipped, triangles_rasterized;
    u64 bbox_area_sum;
    int bbox_valid;
    s32 bbox_minx, bbox_miny, bbox_maxx, bbox_maxy;
    u32 largest_triangle_area;
    struct {
        float obj[3], mv[3], clip[4], screen[3];
        float normal[3][3], texcoord[8][3];
        u8 color[2][4];
        u8 pos_mtx;
    } largest_triangle[3];
    struct {
        u32 order, texcoord, texmap, enable, colorchan, cc, ac, tevind;
        u32 ksel, kcsel, kasel;
        s16 konst[4];
    } stage[16];
    GxDebugTexture tex[8];
} GxDebugDraw;

static GxDebugDraw s_debug_draw[GX_CENSUS_MAX];
static GxDebugDraw s_debug_pending;
static int s_debug_pending_index = -1;
static u64 s_debug_pending_pixels_before;
static u64 s_debug_draw_sequence;

#define GX_DEBUG_RECENT_MAX 2048u
#define GX_DEBUG_LARGE_MAX 4096u
typedef struct {
    u64 sequence, frame;
    u32 cpu_pc, dl, census_hash, prim, nverts, vstride;
    u64 vertex_hash, pixels, alpha_tested, alpha_rejected, alpha_sum;
    u32 alpha_min, alpha_max;
    u64 blend_inputs, z_rejected, color_writes;
    u32 triangles_submitted, triangles_trivial_rejected, triangles_culled;
    u32 triangles_clipped, triangles_rasterized;
    u64 bbox_area_sum;
    int bbox_valid;
    s32 bbox_minx, bbox_miny, bbox_maxx, bbox_maxy;
    u32 largest_triangle_area;
    float largest_screen[3][3];
    u32 alpha_test, zmode, blendmode, pecontrol;
} GxDebugRecent;
static GxDebugRecent s_debug_recent[GX_DEBUG_RECENT_MAX];
static u32 s_debug_recent_head, s_debug_recent_count;
static u64 s_debug_recent_sequence, s_debug_recent_latest_frame;
/* Dedicated chronological journal for scene-sized draws.  The general recent
 * ring intentionally also keeps tiny zero-pixel draws so rejected geometry can
 * be diagnosed, but Wind Waker can submit enough of those to evict an entire
 * title frame.  Keep large draws in their own ring so the base ocean pass and
 * later wave/composite passes survive until a TCP query. */
static GxDebugRecent s_debug_large[GX_DEBUG_LARGE_MAX];
static u32 s_debug_large_head, s_debug_large_count;
static u64 s_debug_large_latest_frame;

void gx_raster_notify_bp_write(void) {
    if (++s_bp_generation == 0) {
        s_bp_generation = 1;
        s_cfg_bp_generation = 0;
    }
}

/* Shared with gx_raster_efb_copy (which rebuilds this same subset for its own
 * call via build_efb_cfg() — see below): PEControl.pixel_format and
 * BlendMode.{color,alpha}_update / ZMode.update_enable back the low-level EFB
 * pixel helpers (GetPixelColor family, ZCompare, efb_clear_rect) that BOTH the
 * draw and EFB-copy paths call. A single cache slot updated at whichever entry
 * point currently owns it is correct: gx_raster_draw and gx_raster_efb_copy
 * never run concurrently or re-enter each other, and each decodes this subset
 * fresh at its own entry before any pixel work reads it. */
static u32 s_pf;       /* pixel_format() */
static int s_zt_upd;   /* zm_update_enable() */
static int s_bm_cu;    /* bm_color_update() */
static int s_bm_au;    /* bm_alpha_update() */

/* ============================================================================
 * EFB pixel access (SWEfbInterface.cpp). RGB8_Z24, RGBA6_Z24 and RGB565_Z16
 * (menu-observed: a transient PEControl switch during the boot animation) are
 * handled; Z24-only (depth-only rendering) stays a trap — never observed.
 * ==========================================================================*/
/* The two GetPixelColor formulas, factored out to plain per-format functions
 * (u32 off -> u32 color) so gx_raster_efb_copy's inner loop can pick one of
 * them ONCE PER COPY (its s_pf is per-copy-constant, see build_efb_cfg) instead
 * of re-switching on s_pf on every one of its GetPixelColor calls — see
 * get_efb_color/gx_raster_efb_copy below. Pure extraction: GetPixelColor's own
 * switch below is unchanged bit-for-bit, it just calls these instead of
 * inlining their bodies. */
static inline u32 get_pixel_color_direct(u32 off) {
    /* RGB8_Z24 / Z24 / RGB565_Z16 (SWEfbInterface.cpp:164-166 — Dolphin itself
     * treats RGB565_Z16 identically to RGB8_Z24, "not supported correctly
     * yet"; transcribed as-is so we match the oracle exactly). */
    return 0xffu | ((s_efb_color[off] & 0x00ffffffu) << 8);
}
static inline u32 get_pixel_color_rgba6(u32 off) {
    u32 src = s_efb_color[off];
    return (u32)Convert6To8(src & 0x3f) |
           ((u32)Convert6To8((src >> 6) & 0x3f) << 8) |
           ((u32)Convert6To8((src >> 12) & 0x3f) << 16) |
           ((u32)Convert6To8((src >> 18) & 0x3f) << 24);
}
static u32 GetPixelColor(u32 off) {
    switch (s_pf) {
    case PF_RGB8_Z24:
    case PF_Z24:
    case PF_RGB565_Z16:
        return get_pixel_color_direct(off);
    case PF_RGBA6_Z24:
        return get_pixel_color_rgba6(off);
    default:
        TRAPF(pf_getcolor, "EFB GetPixelColor pixel_format %u (only RGB8_Z24=0/RGBA6_Z24=1 "
              "in scope; raw ZCOMPARE/PEControl bp[0x43]=0x%06X)",
              s_pf, s_bp[0x43]);
        return get_pixel_color_direct(off);
    }
}
/* Same factoring as get_pixel_color_direct/get_pixel_color_rgba6 above, this
 * time for the three EFB pixel SETTERS — pure extraction, each switch's own
 * case body unchanged bit-for-bit, just calling these instead of inlining
 * them. Lets efb_clear_rect (below) pick one specialized formula per setter
 * ONCE PER CLEAR RECT (s_pf is a per-clear constant, same as it is per-copy)
 * instead of re-switching on s_pf on every one of the rect's pixels. */
static inline void set_pixel_color_only_direct(u32 off, const u8* rgb) {
    u32 src; memcpy(&src, rgb, 4);
    s_efb_color[off] = (s_efb_color[off] & 0xff000000u) | (src >> 8);
}
static inline void set_pixel_color_only_rgba6(u32 off, const u8* rgb) {
    u32 src; memcpy(&src, rgb, 4);
    u32 val = s_efb_color[off] & 0xff00003fu;
    val |= (src >> 4) & 0x00000fc0u;   /* blue  */
    val |= (src >> 6) & 0x0003f000u;   /* green */
    val |= (src >> 8) & 0x00fc0000u;   /* red   */
    s_efb_color[off] = val;
}
static void SetPixelColorOnly(u32 off, const u8* rgb) {
    switch (s_pf) {
    case PF_RGB8_Z24: case PF_Z24: case PF_RGB565_Z16:
        set_pixel_color_only_direct(off, rgb);
        break;
    case PF_RGBA6_Z24:
        set_pixel_color_only_rgba6(off, rgb);
        break;
    default:
        TRAPF(pf_setcolor, "EFB SetPixelColorOnly pixel_format %u (raw bp[0x43]=0x%06X)",
              s_pf, s_bp[0x43]);
        break;
    }
}
static inline void set_pixel_alpha_color_direct(u32 off, const u8* color) {
    u32 src; memcpy(&src, color, 4);
    s_efb_color[off] = (s_efb_color[off] & 0xff000000u) | (src >> 8);
}
static inline void set_pixel_alpha_color_rgba6(u32 off, const u8* color) {
    u32 src; memcpy(&src, color, 4);
    u32 val = s_efb_color[off] & 0xff000000u;
    val |= (src >> 2) & 0x0000003fu;   /* alpha */
    val |= (src >> 4) & 0x00000fc0u;   /* blue  */
    val |= (src >> 6) & 0x0003f000u;   /* green */
    val |= (src >> 8) & 0x00fc0000u;   /* red   */
    s_efb_color[off] = val;
}
static void SetPixelAlphaColor(u32 off, const u8* color) {
    switch (s_pf) {
    case PF_RGB8_Z24: case PF_Z24: case PF_RGB565_Z16:
        set_pixel_alpha_color_direct(off, color);
        break;
    case PF_RGBA6_Z24:
        set_pixel_alpha_color_rgba6(off, color);
        break;
    default:
        TRAPF(pf_setalpha, "EFB SetPixelAlphaColor pixel_format %u (raw bp[0x43]=0x%06X)",
              s_pf, s_bp[0x43]);
        break;
    }
}
/* RGB8/Z24/RGB565 have no alpha plane — nothing to do (no "direct" formula
 * exists for this setter; the direct-format branch is a real, deliberate
 * no-op, not an omission). */
static inline void set_pixel_alpha_only_rgba6(u32 off, u8 a) {
    u32 val = s_efb_color[off] & 0xffffffc0u;
    val |= (a >> 2) & 0x3f;
    s_efb_color[off] = val;
}
static void SetPixelAlphaOnly(u32 off, u8 a) {
    if (s_pf == PF_RGBA6_Z24) {
        set_pixel_alpha_only_rgba6(off, a);
    }
    /* RGB8/Z24/RGB565 have no alpha plane — nothing to do. */
}
static void SetPixelDepth(u32 off, u32 depth) { s_efb_depth[off] = depth & 0x00ffffffu; }
static u32  GetPixelDepth(u32 off)            { return s_efb_depth[off] & 0x00ffffffu; }

/* VideoCommon/VideoCommon.h CompressZ16. Flipper's RGB565_Z16 mode exposes
 * one of four 16-bit encodings through GXPeekZ; the other EFB formats return
 * the stored 24-bit value directly. */
static u32 compress_z16(u32 z24, u32 format) {
    if (format == 0u || format > 3u)
        return z24 >> 8;

    u32 shifted = z24 << 8;
    u32 inverted = ~shifted;
    u32 leading = inverted ? (u32)__builtin_clz(inverted) : 32u;
    u32 exp_bits;
    int next_one = 0;
    if (format == 1u) {
        exp_bits = 2u;
        if (leading >= 3u) { leading = 3u; next_one = 1; }
    } else if (format == 2u) {
        exp_bits = 3u;
        if (leading >= 7u) { leading = 7u; next_one = 1; }
    } else {
        exp_bits = 4u;
        if (leading >= 12u) { leading = 12u; next_one = 1; }
    }

    u32 mantissa_bits = 16u - exp_bits;
    u32 top = 24u - leading;
    if (top < mantissa_bits) top = mantissa_bits;
    if (!next_one) top--;
    u32 bottom = top - mantissa_bits;
    u32 mantissa = (z24 >> bottom) & ((1u << mantissa_bits) - 1u);
    return (leading << mantissa_bits) | mantissa;
}

int gx_raster_efb_cpu_read(u32 address, u32* value) {
    if (!value || !s_bp)
        return 0;

    /* Core/PowerPC/MMU.cpp EFB_Read: the low address bits encode one 32-bit
     * pixel per x and one 4 KiB row per y. The same decode works for physical
     * 0x08xxxxxx and the SDK's effective 0xC8xxxxxx mapping. */
    u32 x = (address & 0xFFFu) >> 2;
    u32 y = (address >> 12) & 0x3FFu;
    if (x >= EFB_WIDTH || y >= EFB_HEIGHT) {
        *value = 0;
        return 1;
    }
    if (address & 0x00800000u) {
        TRAP(efb_cpu_zcolor, "combined Z+color CPU EFB read");
        return 0;
    }
    if (address & 0x00400000u) {
        u32 depth = GetPixelDepth(y * EFB_WIDTH + x);
        if (pixel_format() == PF_RGB565_Z16)
            depth = compress_z16(depth, bits(s_bp[0x43], 3, 3));
        *value = depth;
        return 1;
    }

    TRAP(efb_cpu_color, "CPU EFB color read");
    return 0;
}

/* BlendMode (bp 0x41) accessors. */
static u32 bm(void) { return s_bp[0x41]; }
static int bm_blend_enable(void)  { return bits(bm(), 0, 1); }
static int bm_logic_enable(void)  { return bits(bm(), 1, 1); }
static int bm_dither(void)        { return bits(bm(), 2, 1); }
static int bm_color_update(void)  { return bits(bm(), 3, 1); }
static int bm_alpha_update(void)  { return bits(bm(), 4, 1); }
static u32 bm_dst_factor(void)    { return bits(bm(), 5, 3); }
static u32 bm_src_factor(void)    { return bits(bm(), 8, 3); }
static int bm_subtract(void)      { return bits(bm(), 11, 1); }
static u32 bm_logic_mode(void)    { return bits(bm(), 12, 4); }

/* ZMode (bp 0x40). */
static int zm_test_enable(void)   { return bits(s_bp[0x40], 0, 1); }
static u32 zm_func(void)          { return bits(s_bp[0x40], 1, 3); }
static int zm_update_enable(void) { return bits(s_bp[0x40], 4, 1); }
/* ConstantAlpha / dstalpha (bp 0x42). */
static int da_enable(void)        { return bits(s_bp[0x42], 8, 1); }
static u8  da_alpha(void)         { return (u8)bits(s_bp[0x42], 0, 8); }
/* early_ztest (bp 0x43 PEControl). */
static int early_ztest(void)      { return bits(s_bp[0x43], 6, 1); }

enum { CMP_NEVER, CMP_LESS, CMP_EQUAL, CMP_LEQUAL, CMP_GREATER, CMP_NEQUAL, CMP_GEQUAL, CMP_ALWAYS };

static int ZCompare(u16 x, u16 y, u32 z) {
    u32 off = (u32)x + (u32)y * EFB_WIDTH;
    u32 depth = GetPixelDepth(off);
    int pass;
    switch (s_cfg.zt_func) {
    case CMP_NEVER:   pass = 0;        break;
    case CMP_LESS:    pass = z < depth;  break;
    case CMP_EQUAL:   pass = z == depth; break;
    case CMP_LEQUAL:  pass = z <= depth; break;
    case CMP_GREATER: pass = z > depth;  break;
    case CMP_NEQUAL:  pass = z != depth; break;
    case CMP_GEQUAL:  pass = z >= depth; break;
    case CMP_ALWAYS:  pass = 1;        break;
    default:          pass = 0;        break;
    }
    if (pass && s_zt_upd) SetPixelDepth(off, z);
    return pass;
}

/* Blend factor helpers (SWEfbInterface.cpp:230-310). Color arrays are [A,B,G,R]. */
static u32 src_factor(const u8* s, const u8* d, u32 mode) {
    u8 a;
    switch (mode) {
    case 0: return 0;                              /* Zero */
    case 1: return 0xffffffffu;                    /* One  */
    case 2: { u32 v; memcpy(&v, d, 4); return v; } /* DstClr */
    case 3: { u32 v; memcpy(&v, d, 4); return 0xffffffffu - v; }
    case 4: a = s[ALP_C]; break;                   /* SrcAlpha */
    case 5: a = 0xff - s[ALP_C]; break;            /* InvSrcAlpha */
    case 6: a = d[ALP_C]; break;                   /* DstAlpha */
    case 7: a = 0xff - d[ALP_C]; break;            /* InvDstAlpha */
    default: return 0;
    }
    return ((u32)a << 24) | ((u32)a << 16) | ((u32)a << 8) | a;
}
static u32 dst_factor(const u8* s, const u8* d, u32 mode) {
    u8 a;
    switch (mode) {
    case 0: return 0;
    case 1: return 0xffffffffu;
    case 2: { u32 v; memcpy(&v, s, 4); return v; }          /* SrcClr */
    case 3: { u32 v; memcpy(&v, s, 4); return 0xffffffffu - v; }
    case 4: a = s[ALP_C]; break;
    case 5: a = 0xff - s[ALP_C]; break;
    case 6: a = d[ALP_C]; break;
    case 7: a = 0xff - d[ALP_C]; break;
    default: return 0;
    }
    return ((u32)a << 24) | ((u32)a << 16) | ((u32)a << 8) | a;
}
static void BlendColor(const u8* src, u8* dst) {
    u32 sf = src_factor(src, dst, s_cfg.bm_src_factor);
    u32 df = dst_factor(src, dst, s_cfg.bm_dst_factor);
    for (int i = 0; i < 4; i++) {
        u32 s = sf & 0xff; s += s >> 7;
        u32 d = df & 0xff; d += d >> 7;
        u32 c = (src[i] * s + dst[i] * d) >> 8;
        dst[i] = (c > 255) ? 255 : (u8)c;
        df >>= 8; sf >>= 8;
    }
}
static void SubtractBlend(const u8* src, u8* dst) {
    for (int i = 0; i < 4; i++) {
        int c = (int)dst[i] - (int)src[i];
        dst[i] = (c < 0) ? 0 : (u8)c;
    }
}
static void LogicBlend(u32 s, u32* d, u32 op) {
    switch (op) {
    case 0:  *d = 0; break;
    case 1:  *d = s & *d; break;
    case 2:  *d = s & (~*d); break;
    case 3:  *d = s; break;
    case 4:  *d = (~s) & *d; break;
    case 5:  break;
    case 6:  *d = s ^ *d; break;
    case 7:  *d = s | *d; break;
    case 8:  *d = ~(s | *d); break;
    case 9:  *d = ~(s ^ *d); break;
    case 10: *d = ~*d; break;
    case 11: *d = s | (~*d); break;
    case 12: *d = ~s; break;
    case 13: *d = (~s) | *d; break;
    case 14: *d = ~(s & *d); break;
    case 15: *d = 0xffffffffu; break;
    }
}
static void Dither(u16 x, u16 y, u8* color) {
    if (!s_cfg.bm_dither || s_pf != PF_RGBA6_Z24) return;
    static const u8 dth[2][2] = { {0, 2}, {3, 1} };
    for (int i = BLU_C; i <= RED_C; i++)
        color[i] = (u8)(((color[i] - (color[i] >> 6)) + dth[y & 1][x & 1]) & 0xfc);
}
/* BlendTev (SWEfbInterface.cpp:412-450). color is [A,B,G,R]. */
static void BlendTev(u16 x, u16 y, u8* color) {
    u32 off = (u32)x + (u32)y * EFB_WIDTH;
    u32 dstClr = GetPixelColor(off);
    u8* dstPtr = (u8*)&dstClr;

    if (s_cfg.bm_blend_enable) {
        if (s_cfg.bm_subtract) SubtractBlend(color, dstPtr);
        else                   BlendColor(color, dstPtr);
    } else if (s_cfg.bm_logic_enable) {
        u32 s; memcpy(&s, color, 4);
        LogicBlend(s, &dstClr, s_cfg.bm_logic_mode);
    } else {
        dstPtr = color;
    }

    if (s_cfg.da_enable) dstPtr[ALP_C] = s_cfg.da_alpha;

    if (s_bm_cu) {
        Dither(x, y, dstPtr);
        if (s_bm_au) SetPixelAlphaColor(off, dstPtr);
        else         SetPixelColorOnly(off, dstPtr);
    } else if (s_bm_au) {
        SetPixelAlphaOnly(off, dstPtr[ALP_C]);
    }
}

/* ============================================================================
 * Texture sampler — unit 0, from MEM1 (TextureSampler.cpp + per-format decode
 * TextureDecoder_Common.cpp:300-640). Formats the menu-frame inventory shows:
 * I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/CMPR. Paletted C4/C8/C14X2 need a TLUT
 * model we haven't built (never observed) and trap. No TMEM cache (we always
 * read straight from MEM1, matching Dolphin's non-"cache_manually_managed"
 * path), no mipmaps beyond LOD selection (mip levels themselves trap).
 * ==========================================================================*/
enum { TEXFMT_I4 = 0x0, TEXFMT_I8 = 0x1, TEXFMT_IA4 = 0x2, TEXFMT_IA8 = 0x3,
       TEXFMT_RGB565 = 0x4, TEXFMT_RGB5A3 = 0x5, TEXFMT_RGBA8 = 0x6,
       TEXFMT_C4 = 0x8, TEXFMT_C8 = 0x9, TEXFMT_C14X2 = 0xA, TEXFMT_CMPR = 0xE };
enum { WRAP_CLAMP = 0, WRAP_REPEAT = 1, WRAP_MIRROR = 2 };

/* GX texture-register addressing is split into two banks. Units 0..3 use
 * BP 0x80..0x9b, while units 4..7 use 0xa0..0xbb. Within either bank the
 * low two unit bits select a column and the register kind advances by four
 * words (TexUnitAddress::ComputeOffset in Dolphin's BPMemory model). */
static u32 tx_unit_base(u32 unit) {
    return (unit & 3u) | ((unit & 4u) << 3);
}
static u32 tx_mode0(u32 unit) { return s_bp[0x80 + tx_unit_base(unit)]; }
static u32 tx_mode1(u32 unit) { return s_bp[0x84 + tx_unit_base(unit)]; }
static u32 tx_image0(u32 unit){ return s_bp[0x88 + tx_unit_base(unit)]; }
static u32 tx_image3(u32 unit){ return s_bp[0x94 + tx_unit_base(unit)]; }

static int wrap_coord(int coord, u32 wrap, int size) {
    switch (wrap) {
    case WRAP_CLAMP:
        if (coord < 0) coord = 0; else if (coord > size - 1) coord = size - 1;
        return coord;
    case WRAP_REPEAT:
        return coord & (size - 1);
    case WRAP_MIRROR:
        if ((coord & size) != 0) coord = ~coord;
        return coord & (size - 1);
    default:
        TRAP(wrapmode, "texture wrap mode 3");
        if (coord < 0) coord = 0; else if (coord > size - 1) coord = size - 1;
        return coord;
    }
}

static inline u8 safe_u8(const u8* src, u32 len, u32 off) { return (off < len) ? src[off] : 0; }

/* DecodePixel_Paletted (TextureDecoder_Common.cpp): one 16-bit TLUT entry ->
 * RGBA. `entry` points at the entry's 2 bytes inside modeled TMEM. IA8 reads
 * the raw transmission bytes ([A, I] — Dolphin's DecodePixel_IA8 takes the
 * UNSWAPPED little-endian read, same convention as the direct IA8 texture
 * case below); RGB565/RGB5A3 swap to the true big-endian value first
 * (be_u16) and then share the exact bit decode of their direct-format twins.
 * An out-of-range tlut format (3) returns 0, as Dolphin's default: does. */
static void decode_tlut_pixel(const u8* entry, u32 tlutfmt, u8 out[4]) {
    switch (tlutfmt) {
    case 0: {                                     /* IA8 */
        out[0] = out[1] = out[2] = entry[1]; out[3] = entry[0];
        break;
    }
    case 1: {                                     /* RGB565 */
        u16 val = be_u16(entry);
        out[0] = Convert5To8((u8)((val >> 11) & 0x1Fu));
        out[1] = Convert6To8((u8)((val >> 5) & 0x3Fu));
        out[2] = Convert5To8((u8)(val & 0x1Fu));
        out[3] = 0xFFu;
        break;
    }
    case 2: {                                     /* RGB5A3 */
        u16 val = be_u16(entry);
        if (val & 0x8000u) {
            out[0] = Convert5To8((u8)((val >> 10) & 0x1Fu));
            out[1] = Convert5To8((u8)((val >> 5) & 0x1Fu));
            out[2] = Convert5To8((u8)(val & 0x1Fu));
            out[3] = 0xFFu;
        } else {
            out[3] = Convert3To8((u8)((val >> 12) & 0x7u));
            out[0] = Convert4To8((u8)((val >> 8) & 0xFu));
            out[1] = Convert4To8((u8)((val >> 4) & 0xFu));
            out[2] = Convert4To8((u8)(val & 0xFu));
        }
        break;
    }
    default: out[0] = out[1] = out[2] = out[3] = 0; break;
    }
}

/* Per-texel decode (TextureDecoder_Common.cpp TexDecoder_DecodeTexel:361-639,
 * transcribed exactly: the 7 direct formats the menu frame uses plus the
 * paletted C4/C8/C14X2 family, which index a TLUT inside modeled TMEM
 * (`tlut` = entry 0 of this texture's palette, `tlutfmt` its entry format —
 * both from TX_SETTLUT via build_draw_cfg; ignored by direct formats). The
 * TLUT pointer is always within TMEM: max reachable byte (tmem_offset
 * 0x3FF<<9 + C14X2 index 16383*2+1) < 1MB, the same bound Dolphin
 * static_asserts, so entry reads need no per-access length check. `out` is
 * RGBA (matches the rest of the TEV pipeline's texel[4] convention).
 * Endianness note: RGB565/RGB5A3/CMPR color1/color2 explicitly
 * Common::swap16 the raw 16-bit read before decoding (so we reconstruct the
 * true big-endian value via be_u16); IA8 does NOT swap (DecodePixel_IA8
 * takes the raw unswapped read), so on Dolphin's little-endian host alpha
 * ends up as the FIRST transmitted byte and intensity the second —
 * reproduced here with direct byte indexing rather than be_u16, to match
 * the oracle bit-for-bit rather than the "IA" name. */
static void decode_texel(u32 fmt, const u8* src, u32 src_len, int s, int t,
                         int image_w_minus_1, const u8* tlut, u32 tlutfmt,
                         u8 out[4]) {
    u32 ss = (u32)s, tt = (u32)t, iw1 = (u32)image_w_minus_1;
    switch (fmt) {
    case TEXFMT_C4: {   /* 8x8 blocks, 4bpp — same tiling as I4, value is a TLUT index */
        u32 sBlk = ss >> 3, tBlk = tt >> 3, widthBlks = (iw1 >> 3) + 1;
        u32 base = (tBlk * widthBlks + sBlk) << 5;
        u32 blkOff = ((tt & 7) << 3) + (ss & 7);
        int rs = (blkOff & 1) ? 0 : 4;
        u8 val = (u8)((safe_u8(src, src_len, base + (blkOff >> 1)) >> rs) & 0xFu);
        decode_tlut_pixel(tlut + (u32)val * 2u, tlutfmt, out);
        break;
    }
    case TEXFMT_C8: {   /* 8x4 blocks, 8bpp — same tiling as I8 */
        u32 sBlk = ss >> 3, tBlk = tt >> 2, widthBlks = (iw1 >> 3) + 1;
        u32 base = (tBlk * widthBlks + sBlk) << 5;
        u32 blkOff = ((tt & 3) << 3) + (ss & 7);
        u8 val = safe_u8(src, src_len, base + blkOff);
        decode_tlut_pixel(tlut + (u32)val * 2u, tlutfmt, out);
        break;
    }
    case TEXFMT_C14X2: { /* 4x4 blocks, 16bpp — big-endian read, low 14 bits index */
        u32 sBlk = ss >> 2, tBlk = tt >> 2, widthBlks = (iw1 >> 2) + 1;
        u32 base = (tBlk * widthBlks + sBlk) << 4;
        u32 blkOff = ((tt & 3) << 2) + (ss & 3);
        u32 offset = (base + blkOff) << 1;
        u16 val = (offset + 1 < src_len) ? (u16)(be_u16(&src[offset]) & 0x3FFFu) : 0;
        decode_tlut_pixel(tlut + (u32)val * 2u, tlutfmt, out);
        break;
    }
    case TEXFMT_I4: {
        u32 sBlk = ss >> 3, tBlk = tt >> 3, widthBlks = (iw1 >> 3) + 1;
        u32 base = (tBlk * widthBlks + sBlk) << 5;
        u32 blkOff = ((tt & 7) << 3) + (ss & 7);
        int rs = (blkOff & 1) ? 0 : 4;
        u8 raw = safe_u8(src, src_len, base + (blkOff >> 1));
        u8 val = Convert4To8((u8)((raw >> rs) & 0xFu));
        out[0] = out[1] = out[2] = out[3] = val;
        break;
    }
    case TEXFMT_I8: {
        u32 sBlk = ss >> 3, tBlk = tt >> 2, widthBlks = (iw1 >> 3) + 1;
        u32 base = (tBlk * widthBlks + sBlk) << 5;
        u32 blkOff = ((tt & 3) << 3) + (ss & 7);
        u8 val = safe_u8(src, src_len, base + blkOff);
        out[0] = out[1] = out[2] = out[3] = val;
        break;
    }
    case TEXFMT_IA4: {
        u32 sBlk = ss >> 3, tBlk = tt >> 2, widthBlks = (iw1 >> 3) + 1;
        u32 base = (tBlk * widthBlks + sBlk) << 5;
        u32 blkOff = ((tt & 3) << 3) + (ss & 7);
        u8 raw = safe_u8(src, src_len, base + blkOff);
        u8 a = Convert4To8((u8)(raw >> 4)), l = Convert4To8((u8)(raw & 0xFu));
        out[0] = l; out[1] = l; out[2] = l; out[3] = a;
        break;
    }
    case TEXFMT_IA8: {
        u32 sBlk = ss >> 2, tBlk = tt >> 2, widthBlks = (iw1 >> 2) + 1;
        u32 base = (tBlk * widthBlks + sBlk) << 4;
        u32 blkOff = ((tt & 3) << 2) + (ss & 3);
        u32 offset = (base + blkOff) << 1;
        u8 a = safe_u8(src, src_len, offset), i = safe_u8(src, src_len, offset + 1);
        out[0] = i; out[1] = i; out[2] = i; out[3] = a;
        break;
    }
    case TEXFMT_RGB565: {
        u32 sBlk = ss >> 2, tBlk = tt >> 2, widthBlks = (iw1 >> 2) + 1;
        u32 base = (tBlk * widthBlks + sBlk) << 4;
        u32 blkOff = ((tt & 3) << 2) + (ss & 3);
        u32 offset = (base + blkOff) << 1;
        u16 val = (offset + 1 < src_len) ? be_u16(&src[offset]) : 0;
        out[0] = Convert5To8((u8)((val >> 11) & 0x1Fu));
        out[1] = Convert6To8((u8)((val >> 5) & 0x3Fu));
        out[2] = Convert5To8((u8)(val & 0x1Fu));
        out[3] = 0xFFu;
        break;
    }
    case TEXFMT_RGB5A3: {
        u32 sBlk = ss >> 2, tBlk = tt >> 2, widthBlks = (iw1 >> 2) + 1;
        u32 base = (tBlk * widthBlks + sBlk) << 4;
        u32 blkOff = ((tt & 3) << 2) + (ss & 3);
        u32 offset = (base + blkOff) << 1;
        u16 val = (offset + 1 < src_len) ? be_u16(&src[offset]) : 0;
        if (val & 0x8000u) {
            out[0] = Convert5To8((u8)((val >> 10) & 0x1Fu));
            out[1] = Convert5To8((u8)((val >> 5) & 0x1Fu));
            out[2] = Convert5To8((u8)(val & 0x1Fu));
            out[3] = 0xFFu;
        } else {
            out[3] = Convert3To8((u8)((val >> 12) & 0x7u));
            out[0] = Convert4To8((u8)((val >> 8) & 0xFu));
            out[1] = Convert4To8((u8)((val >> 4) & 0xFu));
            out[2] = Convert4To8((u8)(val & 0xFu));
        }
        break;
    }
    case TEXFMT_RGBA8: {
        u32 sBlk = ss >> 2, tBlk = tt >> 2, widthBlks = (iw1 >> 2) + 1;
        u32 base = (tBlk * widthBlks + sBlk) << 5;   /* shift by 5 is correct (AR+GB planes) */
        u32 blkOff = ((tt & 3) << 2) + (ss & 3);
        u32 offset = (base + blkOff) << 1;
        out[3] = safe_u8(src, src_len, offset);       /* A */
        out[0] = safe_u8(src, src_len, offset + 1);   /* R */
        out[1] = safe_u8(src, src_len, offset + 32);  /* G */
        out[2] = safe_u8(src, src_len, offset + 33);  /* B */
        break;
    }
    case TEXFMT_CMPR: {
        u32 sDxt = ss >> 2, tDxt = tt >> 2;
        u32 sBlk = sDxt >> 1, tBlk = tDxt >> 1, widthBlks = (iw1 >> 3) + 1;
        u32 base = (tBlk * widthBlks + sBlk) << 2;
        u32 blkOff = ((tDxt & 1) << 1) + (sDxt & 1);
        u32 offset = (base + blkOff) << 3;   /* 8 bytes/DXTBlock: c1(2) c2(2) lines(4) */
        u16 c1 = (offset + 1 < src_len) ? be_u16(&src[offset]) : 0;
        u16 c2 = (offset + 3 < src_len) ? be_u16(&src[offset + 2]) : 0;
        u8 line = safe_u8(src, src_len, offset + 4 + (tt & 3));
        int blue1 = Convert5To8((u8)(c1 & 0x1Fu)),  blue2 = Convert5To8((u8)(c2 & 0x1Fu));
        int green1 = Convert6To8((u8)((c1 >> 5) & 0x3Fu)), green2 = Convert6To8((u8)((c2 >> 5) & 0x3Fu));
        int red1 = Convert5To8((u8)((c1 >> 11) & 0x1Fu)), red2 = Convert5To8((u8)((c2 >> 11) & 0x1Fu));
        int rs = 6 - (int)((ss & 3) << 1);
        int colorSel = (line >> rs) & 3;
        colorSel |= (c1 > c2) ? 0 : 4;
        switch (colorSel) {
        case 0: case 4:
            out[0] = (u8)red1; out[1] = (u8)green1; out[2] = (u8)blue1; out[3] = 0xFFu; break;
        case 1: case 5:
            out[0] = (u8)red2; out[1] = (u8)green2; out[2] = (u8)blue2; out[3] = 0xFFu; break;
        case 2:
            out[0] = (u8)((red2 * 3 + red1 * 5) >> 3);
            out[1] = (u8)((green2 * 3 + green1 * 5) >> 3);
            out[2] = (u8)((blue2 * 3 + blue1 * 5) >> 3);
            out[3] = 0xFFu; break;
        case 3:
            out[0] = (u8)((red1 * 3 + red2 * 5) >> 3);
            out[1] = (u8)((green1 * 3 + green2 * 5) >> 3);
            out[2] = (u8)((blue1 * 3 + blue2 * 5) >> 3);
            out[3] = 0xFFu; break;
        case 6:
            out[0] = (u8)((red1 + red2) / 2); out[1] = (u8)((green1 + green2) / 2);
            out[2] = (u8)((blue1 + blue2) / 2); out[3] = 0xFFu; break;
        case 7:
            out[0] = (u8)((red1 + red2) / 2); out[1] = (u8)((green1 + green2) / 2);
            out[2] = (u8)((blue1 + blue2) / 2); out[3] = 0x00u; break;
        default:
            out[0] = out[1] = out[2] = out[3] = 0; break;
        }
        break;
    }
    default:
        out[0] = out[1] = out[2] = out[3] = 0;   /* unreachable: caller trapped */
        break;
    }
}

/* ============================================================================
 * Per-draw texel cache (perf; GCN_GX_PIXEL_STATS showed tex_sample at 25% of
 * the triangle-scan wall, with EVERY sample bilinear — 4 decode_texel calls
 * each — and adjacent samples' bilinear footprints overlapping heavily (a
 * given texel is typically re-decoded ~4x by one sample's own taps and again
 * by neighboring pixels' taps). decode_texel does GameCube tiled-format
 * address math + format conversion per call; none of that depends on
 * anything but (fmt, src, src_len, iS, iT, w1).
 *
 * EXACTNESS: this is a correctness-preserving memo, not a heuristic. Within
 * one gx_raster_draw call, tc = &s_cfg.tex[texmap] (build_draw_cfg, populated
 * once at draw entry) freezes fmt/src/src_len/w1 for that texmap — BP loads
 * are separate FIFO commands that never interleave with a draw's vertex/
 * pixel payload (see the "Per-draw config cache" comment above), so they
 * cannot change mid-draw. h1 does not participate at all: decode_texel's
 * signature never takes it (only w1 drives the tiled block-row stride; the
 * per-sample T wrapping that *does* use h1 happens in tex_sample before
 * decode_texel is ever called). So for a fixed draw, (texmap, iS, iT) alone
 * determines every argument decode_texel would receive, which means it also
 * determines the output byte-for-byte. Caching keyed on (texmap, iS, iT) and
 * invalidated at draw granularity is therefore exact: a hit returns the exact
 * out[4] the replaced decode_texel call would have produced, never an
 * approximation.
 *
 * GENERATION TAGGING (no per-draw memset): a global u32 generation counter
 * bumped once per draw (build_draw_cfg). Each slot stores the generation it
 * was written in; a slot is live only if its stored generation equals the
 * current one, so starting a new draw invalidates the whole table for free
 * (one integer increment) instead of paying to clear ~thousands of entries
 * every draw regardless of how few of them the new draw will touch.
 * Wraparound (u32 generation overflowing back to 0 after ~4 billion draws)
 * is handled by treating stored-generation 0 as a reserved "never valid"
 * sentinel: build_draw_cfg() skips 0 when it would otherwise land there, and
 * pays for exactly one full-table memset at that moment (see build_draw_cfg
 * for the code — this is the only clear the cache ever does in its entire
 * lifetime, amortized over 4 billion draws, i.e. effectively never).
 *
 * SIZE / LAYOUT: direct-mapped, power-of-two entry count so the index is a
 * plain AND-mask (no modulo). Entry is deliberately kept as two u32s + 4
 * bytes (key, gen, rgba) rather than folding gen into the tag word: menu
 * textures are small, but texmap (3 bits) + mip (4 bits) + iS + iT (up to
 * 10 bits each, GX's max texture dimension is 1024) want 27 bits for a collision-
 * free key, leaving too few spare bits to also carry a generation counter
 * that wouldn't wrap (and re-trigger the full-clear path) many times a
 * second. Two separate u32s cost 4 more bytes per entry but keep both fields
 * exact and the wraparound vanishingly rare.
 *
 * KEY: texmap<<24 | mip<<20 | iT<<10 | iS — exact and collision-free
 * (iS/iT < 1024, mip < 16, texmap < 8), used only for tag comparison.
 *
 * INDEX MIX: (iS ^ (iT<<5) ^ (mip<<9) ^ (texmap<<11)) & MASK. iS lands in the index's
 * low bits untouched, so horizontally-adjacent texels in the same scanline
 * (iS, iS+1, iS+2, ...) get distinct, sequential index slots instead of all
 * colliding on the same one; the <<5 on iT and <<11 on texmap spread rows
 * and texture units across the table so a vertically-adjacent or different-
 * texmap request doesn't systematically alias iS's own bit pattern either.
 * Size starts at 4096 (12-bit mask); measured hit rate is reported in the
 * task's VERIFY output (bumped to 8192 if 4096 fell short of ~80%).
 * ==========================================================================*/
#define TEXEL_CACHE_BITS  12u
#define TEXEL_CACHE_SIZE  (1u << TEXEL_CACHE_BITS)
#define TEXEL_CACHE_MASK  (TEXEL_CACHE_SIZE - 1u)

typedef struct {
    u32 key;      /* texmap<<24 | mip<<20 | iT<<10 | iS — exact tag */
    u32 gen;      /* draw generation this slot was last written in */
    u8  rgba[4];  /* decode_texel's exact out[4] for that (texmap,iS,iT) */
} TexelCacheEntry;

/* One private cache per GX-MT worker (row = worker id; serial mode only ever
 * touches row 0, which is byte-for-byte the old single cache). The cache is a
 * pure memo over the deterministic decode_texel, so per-worker instances
 * return identical bytes to the shared one — they exist only so workers never
 * write shared lines. Each 48KB row is a multiple of 64B, so rows never share
 * a cache line either. */
static TexelCacheEntry s_texel_cache_w[GX_MT_MAX][TEXEL_CACHE_SIZE]
        __attribute__((aligned(64)));

/* 0 is reserved as the "slot never written since last full clear" sentinel
 * (see the wraparound handling in build_draw_cfg) — so the very first real
 * draw must not use generation 0 either; build_draw_cfg's pre-increment
 * (0 -> 1 on the first call) already guarantees that. The generation is
 * SHARED across workers: only the main thread ever writes it (once per draw,
 * while no fork is in flight), workers just compare against it. */
static u32 s_texel_cache_gen;

static inline u32 texel_cache_key(u32 texmap, u32 mip, u32 iS, u32 iT) {
    return (texmap << 24) | (mip << 20) | (iT << 10) | iS;
}
static inline u32 texel_cache_index(u32 texmap, u32 mip, u32 iS, u32 iT) {
    return (iS ^ (iT << 5) ^ (mip << 9) ^ (texmap << 11)) & TEXEL_CACHE_MASK;
}

/* Cache-checked decode_texel wrapper. Hooked INSIDE tex_sample (this is its
 * only caller — decode_texel itself is left untouched and still callable
 * directly, though a repo-wide grep found no other caller) so callers of
 * decode_texel that might not share tex_sample's per-draw-frozen-texmap
 * invariant are unaffected by construction, not just by accident. */
static inline void decode_texel_cached(int wid, u32 texmap, u32 mip, u32 fmt,
                                        const u8* src, u32 src_len,
                                        int iS, int iT, int w1, const u8* tlut, u32 tlutfmt,
                                        u8 out[4]) {
    u32 key = texel_cache_key(texmap, mip, (u32)iS, (u32)iT);
    u32 idx = texel_cache_index(texmap, mip, (u32)iS, (u32)iT);
    TexelCacheEntry* e = &s_texel_cache_w[wid][idx];

    if (e->gen == s_texel_cache_gen && e->key == key) {
        /* Hit: byte-identical to the decode_texel call this replaces (see the
         * EXACTNESS argument above) — no recompute, just copy the 4 bytes.
         * TLUT contents are frozen for the whole draw (LOADTLUT is a BP
         * command, FIFO-serialized like every other per-draw-constant this
         * cache already relies on), so a paletted texel is as cacheable as a
         * direct one. */
        out[0] = e->rgba[0]; out[1] = e->rgba[1]; out[2] = e->rgba[2]; out[3] = e->rgba[3];
        if (s_pixel_stats) s_ps_texel_cache_hits++;
        return;
    }

    decode_texel(fmt, src, src_len, iS, iT, w1, tlut, tlutfmt, out);
    e->key = key;
    e->gen = s_texel_cache_gen;
    e->rgba[0] = out[0]; e->rgba[1] = out[1]; e->rgba[2] = out[2]; e->rgba[3] = out[3];
    if (s_pixel_stats) s_ps_texel_cache_misses++;
}

/* TextureDecoder block geometry used to advance through packed mip levels in
 * MEM1. Depth is in nibbles per texel. */
static void tex_format_layout(u32 fmt, int* block_w, int* block_h,
                              int* depth_nibbles) {
    switch (fmt) {
    case TEXFMT_I4: case TEXFMT_C4: case TEXFMT_CMPR:
        *block_w = 8; *block_h = 8; *depth_nibbles = 1; break;
    case TEXFMT_I8: case TEXFMT_IA4: case TEXFMT_C8:
        *block_w = 8; *block_h = 4; *depth_nibbles = 2; break;
    case TEXFMT_IA8: case TEXFMT_RGB565: case TEXFMT_RGB5A3: case TEXFMT_C14X2:
        *block_w = 4; *block_h = 4; *depth_nibbles = 4; break;
    case TEXFMT_RGBA8:
        *block_w = 4; *block_h = 4; *depth_nibbles = 8; break;
    default:
        *block_w = 4; *block_h = 4; *depth_nibbles = 8; break;
    }
}

static void tex_sample_mip(int wid, u32 texmap, s32 s, s32 t, u32 mip,
                           int linear, u8* out /*RGBA*/) {
    /* tx_mode0/mode1/image0/image3 field extraction is per-draw-constant (BP
     * loads never interleave with a draw's vertex payload) — decoded once by
     * build_draw_cfg() into s_cfg.tex[texmap] instead of every sample. Trap
     * firing (paletted/unknown format, texture-OOB, mipmap filter) stays at
     * this original per-sample call site, just reading the cached fields
     * instead of re-decoding them each time. */
    const TexUnitCfg* tc = &s_cfg.tex[texmap];
    u32 fmt = tc->fmt;
    int w1 = tc->w1, h1 = tc->h1;   /* width - 1, height - 1 */
    switch (fmt) {
    case TEXFMT_I4: case TEXFMT_I8: case TEXFMT_IA4: case TEXFMT_IA8:
    case TEXFMT_RGB565: case TEXFMT_RGB5A3: case TEXFMT_RGBA8: case TEXFMT_CMPR:
        break;   /* supported (direct) */
    case TEXFMT_C4: case TEXFMT_C8: case TEXFMT_C14X2:
        break;   /* supported (paletted — TLUT in modeled TMEM, see decode_texel) */
    default:
        TRAPF(unknowntexfmt, "unknown/unsupported texture format %u (texmap %u, %ux%u, "
              "TX_SETIMAGE0=0x%06X TX_SETIMAGE3=0x%06X src_addr=0x%08X)",
              fmt, texmap, w1 + 1, h1 + 1, tc->image0_raw, tc->image3_raw,
              tc->img_base);
        memset(out, 0, 4); return;
    }

    if (!tc->valid) {
        TRAP(texoob, "texture source out of MEM1"); memset(out, 0, 4); return;
    }
    const u8* src = tc->src;
    u32 src_len = tc->src_len;
    u32 wrap_s = tc->wrap_s, wrap_t = tc->wrap_t;

    /* TextureSampler::SampleMip: reduce sample coordinates and dimensions,
     * and skip the complete tiled images preceding the requested level. */
    if (mip) {
        int mip_w = w1 + 1, mip_h = h1 + 1;
        int block_w, block_h, depth_nibbles;
        tex_format_layout(fmt, &block_w, &block_h, &depth_nibbles);
        w1 >>= mip;
        h1 >>= mip;
        s >>= mip;
        t >>= mip;
        for (u32 level = 0; level < mip; level++) {
            int stored_w = mip_w > block_w ? mip_w : block_w;
            int stored_h = mip_h > block_h ? mip_h : block_h;
            u64 level_size = ((u64)stored_w * (u64)stored_h *
                              (u64)depth_nibbles) >> 1;
            u32 advance = level_size > src_len ? src_len : (u32)level_size;
            src += advance;
            src_len -= advance;
            mip_w >>= 1;
            mip_h >>= 1;
        }
    }

    if (linear) {
        s -= 64; t -= 64;
        int iS = s >> 7, iT = t >> 7;
        int iS1 = iS + 1, iT1 = iT + 1;
        int fS = s & 0x7f, fT = t & 0x7f;
        iS  = wrap_coord(iS,  wrap_s, w1 + 1);
        iT  = wrap_coord(iT,  wrap_t, h1 + 1);
        iS1 = wrap_coord(iS1, wrap_s, w1 + 1);
        iT1 = wrap_coord(iT1, wrap_t, h1 + 1);
        u8 v00[4], v10[4], v01[4], v11[4];
        /* Cache-checked (see "Per-draw texel cache" comment above): adjacent
         * pixels in a scanline and adjacent samples' own 4 taps repeat the
         * same (texmap,iS,iT) constantly, this is exactly what the cache
         * memoizes. */
        decode_texel_cached(wid, texmap, mip, fmt, src, src_len, iS,  iT,  w1, tc->tlut, tc->tlutfmt, v00);
        decode_texel_cached(wid, texmap, mip, fmt, src, src_len, iS1, iT,  w1, tc->tlut, tc->tlutfmt, v10);
        decode_texel_cached(wid, texmap, mip, fmt, src, src_len, iS,  iT1, w1, tc->tlut, tc->tlutfmt, v01);
        decode_texel_cached(wid, texmap, mip, fmt, src, src_len, iS1, iT1, w1, tc->tlut, tc->tlutfmt, v11);
        for (int c = 0; c < 4; c++) {
            u32 acc = v00[c] * (u32)((128 - fS) * (128 - fT)) +
                      v10[c] * (u32)((fS)       * (128 - fT)) +
                      v01[c] * (u32)((128 - fS) * (fT)) +
                      v11[c] * (u32)((fS)       * (fT));
            out[c] = (u8)(acc >> 14);
        }
    } else {
        int iS = wrap_coord(s >> 7, wrap_s, w1 + 1);
        int iT = wrap_coord(t >> 7, wrap_t, h1 + 1);
        decode_texel_cached(wid, texmap, mip, fmt, src, src_len,
                            iS, iT, w1, tc->tlut, tc->tlutfmt, out);
    }
}

/* TextureSampler::Sample: choose mip(s) from the s28.4 LOD and TX_SETMODE0's
 * mip filter. Point mode rounds at 0.5; linear mode blends adjacent levels. */
static void tex_sample(int wid, u32 texmap, s32 s, s32 t, s32 lod,
                       int linear, u8 out[4]) {
    const TexUnitCfg* tc = &s_cfg.tex[texmap];
    u32 base_mip = 0;
    int mip_linear = 0;
    u32 lod_fract = (u32)lod & 0xFu;

    if (tc->mipmap_filter == 3u)
        TRAP(mipmapfilter, "invalid texture mipmap filter 3");
    if (lod > 0 && tc->mipmap_filter != 0u) {
        base_mip = (u32)lod >> 4;
        mip_linear = lod_fract != 0u && tc->mipmap_filter == 2u;
        if (tc->mipmap_filter == 1u && lod_fract >= 8u)
            base_mip++;
    }

    if (mip_linear) {
        u8 a[4], b[4];
        tex_sample_mip(wid, texmap, s, t, base_mip, linear, a);
        tex_sample_mip(wid, texmap, s, t, base_mip + 1u, linear, b);
        for (int c = 0; c < 4; c++)
            out[c] = (u8)(((u32)a[c] * (16u - lod_fract) +
                           (u32)b[c] * lod_fract) >> 4);
    } else {
        tex_sample_mip(wid, texmap, s, t, base_mip, linear, out);
    }
}

/* FixedLog2 + LOD (Rasterizer.cpp:122-256). */
static s32 fixed_log2(float f) {
    u32 x; memcpy(&x, &f, 4);
    s32 logInt = ((x & 0x7F800000) >> 19) - 2032;
    s32 logFract = (x & 0x007fffff) >> 19;
    return logInt + logFract;
}

/* ============================================================================
 * TEV (Tev.cpp + Tev.h). Single-/multi-stage regular combiner + alpha test +
 * blend. Indirect textures, z-texture and fog trap loudly if enabled.
 * ==========================================================================*/
struct Tev {   /* tagged (not anonymous) so the DrawCfg::fused forward
                * declaration above can name it before this definition */
    TColor Reg[4];          /* Prev, Color0, Color1, Color2 */
    TColor Konst[4];        /* KonstantColors */
    TColor TexColor, RasColor, StageKonst, RawTexColor;
    s32    Position[3];
    u8     Color[2][4];     /* [chan][R,G,B,A] */
    s32    UvS[8], UvT[8];  /* s17.7 */
    s32    TexCoordS, TexCoordT;
    u8     AlphaBump;
    s32    TextureLod[16];  int TextureLinear[16];
    s32    IndirectLod[4];  int IndirectLinear[4];
    int    wid;             /* GX-MT worker id owning this instance — indexes
                             * s_texel_cache_w/s_rb_w so the whole per-pixel
                             * path stays parameterized by the one Tev* it
                             * already threads everywhere. 0 in serial mode. */
} __attribute__((aligned(64)));   /* sizeof rounds up to a 64B multiple, so
                * adjacent s_tev_w[] workers never share a cache line */
   /* Tev already named by the forward `typedef struct Tev Tev;` above */

static const s16 s_Bias[4]   = { 0, 128, -128, 0 };
static const u8  s_LShift[4] = { 0, 1, 2, 0 };
static const u8  s_RShift[4] = { 0, 0, 0, 1 };

static s16 clamp255(s16 v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static s16 clamp1024(s16 v){ return v < -1024 ? -1024 : v > 1023 ? 1023 : v; }

/* GenMode (bp 0x00). */
static u32 gm_numtexgens(void)  { return bits(s_bp[0x00], 0, 4); }
static u32 gm_numcolchans(void) { return bits(s_bp[0x00], 4, 3); }
static u32 gm_numtevstages(void){ return bits(s_bp[0x00], 10, 4); }
static u32 gm_cull_mode(void)   { return bits(s_bp[0x00], 14, 2); }
static u32 gm_numindstages(void){ return bits(s_bp[0x00], 16, 3); }

/* Konst LUT (Tev.h:158-195). Returns r,g,b,a for a KonstSel. */
static void konst_lookup(const Tev* t, u32 sel, s16* r, s16* g, s16* b, s16* a) {
    static const s16 fixed[8] = { 255, 223, 191, 159, 128, 96, 64, 32 };
    if (sel < 8) { *r = *g = *b = *a = fixed[sel]; return; }
    if (sel < 12) { *r = *g = *b = *a = 0; return; }
    if (sel < 16) { const TColor* k = &t->Konst[sel - 12]; *r = k->r; *g = k->g; *b = k->b; *a = 0; return; }
    if (sel < 20) { s16 v = t->Konst[sel - 16].r; *r = *g = *b = *a = v; return; }
    if (sel < 24) { s16 v = t->Konst[sel - 20].g; *r = *g = *b = *a = v; return; }
    if (sel < 28) { s16 v = t->Konst[sel - 24].b; *r = *g = *b = *a = v; return; }
    { s16 v = t->Konst[sel - 28].a; *r = *g = *b = *a = v; return; }
}

/* Color input LUT (Tev.h:130-147): resolve TevColorArg -> (r,g,b). */
static void color_arg(const Tev* t, u32 arg, s16* r, s16* g, s16* b) {
    switch (arg) {
    case 0:  *r = t->Reg[0].r; *g = t->Reg[0].g; *b = t->Reg[0].b; break;
    case 1:  *r = *g = *b = t->Reg[0].a; break;
    case 2:  *r = t->Reg[1].r; *g = t->Reg[1].g; *b = t->Reg[1].b; break;
    case 3:  *r = *g = *b = t->Reg[1].a; break;
    case 4:  *r = t->Reg[2].r; *g = t->Reg[2].g; *b = t->Reg[2].b; break;
    case 5:  *r = *g = *b = t->Reg[2].a; break;
    case 6:  *r = t->Reg[3].r; *g = t->Reg[3].g; *b = t->Reg[3].b; break;
    case 7:  *r = *g = *b = t->Reg[3].a; break;
    case 8:  *r = t->TexColor.r; *g = t->TexColor.g; *b = t->TexColor.b; break;
    case 9:  *r = *g = *b = t->TexColor.a; break;
    case 10: *r = t->RasColor.r; *g = t->RasColor.g; *b = t->RasColor.b; break;
    case 11: *r = *g = *b = t->RasColor.a; break;
    case 12: *r = *g = *b = 255; break;
    case 13: *r = *g = *b = 128; break;
    case 14: *r = t->StageKonst.r; *g = t->StageKonst.g; *b = t->StageKonst.b; break;
    default: *r = *g = *b = 0; break;
    }
}
static s16 alpha_arg(const Tev* t, u32 arg) {
    switch (arg) {
    case 0: return t->Reg[0].a;
    case 1: return t->Reg[1].a;
    case 2: return t->Reg[2].a;
    case 3: return t->Reg[3].a;
    case 4: return t->TexColor.a;
    case 5: return t->RasColor.a;
    case 6: return t->StageKonst.a;
    default: return 0;
    }
}

/* Swap table (BPMemory.h AllTevKSels::GetSwapTable). Returns 4 channel indices. */
static void swap_table(u32 id, u32 out[4]) {
    u32 rg = s_bp[0xF6 + (id << 1)];
    u32 ba = s_bp[0xF6 + (id << 1) + 1];
    out[0] = bits(rg, 0, 2);  /* red  <- */
    out[1] = bits(rg, 2, 2);  /* green<- */
    out[2] = bits(ba, 0, 2);  /* blue <- */
    out[3] = bits(ba, 2, 2);  /* alpha<- */
}

/* Load TEV / konst registers from bp 0xE0-0xE7 (BPStructs.cpp:667-703). */
static void tev_load_registers(Tev* t) {
    for (int num = 0; num < 4; num++) {
        u32 ra = s_bp[0xE0 + num * 2];
        u32 bg = s_bp[0xE1 + num * 2];
        s16 red = (s16)sext(bits(ra, 0, 11), 11);
        s16 alp = (s16)sext(bits(ra, 12, 11), 11);
        s16 blu = (s16)sext(bits(bg, 0, 11), 11);
        s16 grn = (s16)sext(bits(bg, 12, 11), 11);
        int ra_const = bits(ra, 23, 1);
        int bg_const = bits(bg, 23, 1);
        if (ra_const) { t->Konst[num].r = red; t->Konst[num].a = alp; }
        else          { t->Reg[num].r   = red; t->Reg[num].a   = alp; }
        if (bg_const) { t->Konst[num].g = grn; t->Konst[num].b = blu; }
        else          { t->Reg[num].g   = grn; t->Reg[num].b   = blu; }
    }
}

/* draw_color_{regular,compare} / draw_alpha_{regular,compare} take the
 * per-stage cache instead of the raw cc/ac words: the cc/ac bit fields they
 * read (bias|n/a @16, op|cmp @18, clamp @19, scale|mode @20, dest @22) are
 * per-draw-constant, decoded once into TevStageCfg by build_draw_cfg() rather
 * than re-extracted via bits() on every pixel/stage (perf; PURE DECODE
 * MOTION — same fields, same values, just read once instead of N times). */
static void draw_color_regular(Tev* t, const TevStageCfg* sc, const s32 inA[4], const s32 inB[4],
                               const s32 inC[4], const s32 inD[4]) {
    u32 bias = sc->c16, op = sc->c18;
    u32 scale = sc->c20, dest = sc->c22;
    for (int i = BLU_C; i <= RED_C; i++) {
        u32 c = (u32)inC[i] + ((u32)inC[i] >> 7);
        s32 temp = inA[i] * (256 - (s32)c) + inB[i] * (s32)c;
        temp <<= s_LShift[scale];
        temp += (scale == 3) ? 0 : (op ? 127 : 128);
        temp >>= 8;
        temp = op ? -temp : temp;
        s32 result = ((inD[i] + s_Bias[bias]) << s_LShift[scale]) + temp;
        result >>= s_RShift[scale];
        if (i == BLU_C) t->Reg[dest].b = (s16)result;
        else if (i == GRN_C) t->Reg[dest].g = (s16)result;
        else t->Reg[dest].r = (s16)result;
    }
    if (sc->c19) {
        t->Reg[dest].r = clamp255(t->Reg[dest].r);
        t->Reg[dest].g = clamp255(t->Reg[dest].g);
        t->Reg[dest].b = clamp255(t->Reg[dest].b);
    } else {
        t->Reg[dest].r = clamp1024(t->Reg[dest].r);
        t->Reg[dest].g = clamp1024(t->Reg[dest].g);
        t->Reg[dest].b = clamp1024(t->Reg[dest].b);
    }
}
static void draw_color_compare(Tev* t, const TevStageCfg* sc, const s32 inA[4], const s32 inB[4],
                               const s32 inC[4], const s32 inD[4]) {
    u32 mode = sc->c20, cmp = sc->c18, dest = sc->c22;
    for (int i = BLU_C; i <= RED_C; i++) {
        u32 a, b;
        switch (mode) {
        case 0: a = inA[RED_C]; b = inB[RED_C]; break;
        case 1: a = (inA[GRN_C] << 8) | inA[RED_C]; b = (inB[GRN_C] << 8) | inB[RED_C]; break;
        case 2: a = (inA[BLU_C] << 16) | (inA[GRN_C] << 8) | inA[RED_C];
                b = (inB[BLU_C] << 16) | (inB[GRN_C] << 8) | inB[RED_C]; break;
        default: a = inA[i]; b = inB[i]; break;
        }
        s32 add = cmp ? (a == b ? inC[i] : 0) : (a > b ? inC[i] : 0);
        s16 res = (s16)(inD[i] + add);
        if (i == BLU_C) t->Reg[dest].b = res;
        else if (i == GRN_C) t->Reg[dest].g = res;
        else t->Reg[dest].r = res;
    }
    if (sc->c19) {
        t->Reg[dest].r = clamp255(t->Reg[dest].r);
        t->Reg[dest].g = clamp255(t->Reg[dest].g);
        t->Reg[dest].b = clamp255(t->Reg[dest].b);
    }
}
static void draw_alpha_regular(Tev* t, const TevStageCfg* sc, s32 a, s32 b, s32 c, s32 d) {
    u32 bias = sc->a16, op = sc->a18;
    u32 scale = sc->a20, dest = sc->a22;
    u32 cc = (u32)c + ((u32)c >> 7);
    s32 temp = a * (256 - (s32)cc) + b * (s32)cc;
    temp <<= s_LShift[scale];
    temp += (scale == 3) ? 0 : (op ? 127 : 128);
    temp = op ? (-temp >> 8) : (temp >> 8);
    s32 result = ((d + s_Bias[bias]) << s_LShift[scale]) + temp;
    result >>= s_RShift[scale];
    t->Reg[dest].a = (s16)result;
    t->Reg[dest].a = sc->a19 ? clamp255(t->Reg[dest].a) : clamp1024(t->Reg[dest].a);
}
static void draw_alpha_compare(Tev* t, const TevStageCfg* sc, const s32 inAa[4], const s32 inBa[4], s32 c, s32 d) {
    u32 mode = sc->a20, cmp = sc->a18, dest = sc->a22;
    u32 a, b;
    switch (mode) {
    case 0: a = inAa[RED_C]; b = inBa[RED_C]; break;
    case 1: a = (inAa[GRN_C] << 8) | inAa[RED_C]; b = (inBa[GRN_C] << 8) | inBa[RED_C]; break;
    case 2: a = (inAa[BLU_C] << 16) | (inAa[GRN_C] << 8) | inAa[RED_C];
            b = (inBa[BLU_C] << 16) | (inBa[GRN_C] << 8) | inBa[RED_C]; break;
    default: a = inAa[ALP_C]; b = inBa[ALP_C]; break;
    }
    s32 add = cmp ? (a == b ? c : 0) : (a > b ? c : 0);
    t->Reg[dest].a = (s16)(d + add);
    t->Reg[dest].a = sc->a19 ? clamp255(t->Reg[dest].a) : clamp1024(t->Reg[dest].a);
}

/* AlphaTest (bp 0xF3, Tev.cpp:193-238). */
static int alpha_cmp(int alpha, int ref, u32 comp) {
    switch (comp) {
    case CMP_ALWAYS:  return 1;
    case CMP_NEVER:   return 0;
    case CMP_LEQUAL:  return alpha <= ref;
    case CMP_LESS:    return alpha < ref;
    case CMP_GEQUAL:  return alpha >= ref;
    case CMP_GREATER: return alpha > ref;
    case CMP_EQUAL:   return alpha == ref;
    case CMP_NEQUAL:  return alpha != ref;
    default:          return 1;
    }
}
static int alpha_test(int alpha) {
    u32 at = s_bp[0xF3];
    int c0 = alpha_cmp(alpha, (int)bits(at, 0, 8), bits(at, 16, 3));
    int c1 = alpha_cmp(alpha, (int)bits(at, 8, 8), bits(at, 19, 3));
    switch (bits(at, 22, 2)) {
    case 0: return c0 && c1;
    case 1: return c0 || c1;
    case 2: return c0 ^ c1;
    default: return !(c0 ^ c1);
    }
}

static int iround(float x);
static float vp_wd(void);

static float fog_float(u32 word) {
    u32 raw = (((word >> 19) & 1u) << 31) |
              (((word >> 11) & 0xffu) << 23) |
              ((word & 0x7ffu) << 12);
    float f;
    memcpy(&f, &raw, sizeof f);
    return f;
}

static u32 ztexture_depth(const Tev* t) {
    u32 ztex2 = s_bp[0xF5];
    u32 op = bits(ztex2, 2, 2);
    if (op == 0u) return (u32)t->Position[2] & 0x00ffffffu;
    if (op > 2u) {
        TRAPF(ztexop, "invalid z-texture operation %u", op);
        return (u32)t->Position[2] & 0x00ffffffu;
    }

    u32 type = bits(ztex2, 0, 2);
    u32 tex;
    if (type == 0u) {
        tex = (u32)(u8)t->RawTexColor.a;
    } else if (type == 1u) {
        tex = (u32)(u8)t->RawTexColor.r |
              ((u32)(u8)t->RawTexColor.a << 8);
    } else if (type == 2u) {
        tex = ((u32)(u8)t->RawTexColor.r << 16) |
              ((u32)(u8)t->RawTexColor.g << 8) |
              (u32)(u8)t->RawTexColor.b;
    } else {
        TRAP(ztextype, "invalid z-texture format");
        tex = 0;
    }
    tex += s_bp[0xF4] & 0x00ffffffu;
    if (op == 1u) tex += (u32)t->Position[2];
    return tex & 0x00ffffffu;
}

/* GX fog is applied to the TEV result before blending. */
static void apply_fog(const Tev* t, u8 output[4]) {
    u32 f3 = s_bp[0xF1];
    u32 fsel = bits(f3, 21, 3);
    if (fsel == 0u) return;
    if (fsel != 2u && fsel < 4u) {
        TRAPF(fogtype, "invalid fog type %u", fsel);
        return;
    }

    u32 a_word = s_bp[0xEE], c_word = f3;
    float a = fog_float(a_word), c = fog_float(c_word);
    if (((a_word >> 11) & 0xffu) == 0xffu &&
        ((c_word >> 11) & 0xffu) == 0xffu) {
        a = 0.0f;
        c = (!(a_word & (1u << 19)) && !(c_word & (1u << 19)))
            ? -INFINITY : INFINITY;
    }

    u32 z = ztexture_depth(t);
    float ze;
    if (bits(f3, 20, 1) == 0u) {
        u32 shift = s_bp[0xF0] & 31u;
        s32 denom = (s32)(s_bp[0xEF] & 0x00ffffffu) - (s32)(z >> shift);
        ze = denom ? (a * 16777216.0f) / (float)denom
                   : (a < 0.0f ? -INFINITY : INFINITY);
    } else {
        ze = a * (float)z * (1.0f / 16777216.0f);
    }

    u32 range_base = s_bp[0xE8];
    if (bits(range_base, 10, 1)) {
        float vp_width = fabsf(vp_wd() * 2.0f);
        if (vp_width > 0.0f) {
            float center = (float)((s32)bits(range_base, 0, 10) - 342);
            float screen_center = center / vp_width * 2.0f - 1.0f;
            float offset = 2.0f * ((float)t->Position[0] / vp_width) -
                           1.0f - screen_center;
            float findex = 9.0f - fabsf(offset) * 9.0f;
            if (findex < 0.0f) findex = 0.0f;
            if (findex > 9.0f) findex = 9.0f;
            u32 lo = (u32)findex, hi = lo < 9u ? lo + 1u : 9u;
            u32 lo_word = s_bp[0xE9 + lo / 2u];
            u32 hi_word = s_bp[0xE9 + hi / 2u];
            float klo = (float)bits(lo_word, (lo & 1u) ? 0 : 12, 12) / 64.0f;
            float khi = (float)bits(hi_word, (hi & 1u) ? 0 : 12, 12) / 64.0f;
            float frac = findex - (float)lo;
            float k = klo + (khi - klo) * frac;
            if (k != 0.0f) ze *= sqrtf(offset * offset + k * k) / k;
        }
    }

    float fog = ze - c;
    if (fog < 0.0f) fog = 0.0f;
    if (fog > 1.0f) fog = 1.0f;
    switch (fsel) {
    case 4: fog = 1.0f - exp2f(-8.0f * fog); break;
    case 5: fog = 1.0f - exp2f(-8.0f * fog * fog); break;
    case 6: fog = exp2f(-8.0f * (1.0f - fog)); break;
    case 7: fog = 1.0f - fog; fog = exp2f(-8.0f * fog * fog); break;
    default: break;
    }
    int ifog = iround(fog * 256.0f);
    u32 fog_color = s_bp[0xF2];
    u8 fb = (u8)bits(fog_color, 0, 8);
    u8 fg = (u8)bits(fog_color, 8, 8);
    u8 fr = (u8)bits(fog_color, 16, 8);
    output[BLU_C] = (u8)((output[BLU_C] * (256 - ifog) + fb * ifog) >> 8);
    output[GRN_C] = (u8)((output[GRN_C] * (256 - ifog) + fg * ifog) >> 8);
    output[RED_C] = (u8)((output[RED_C] * (256 - ifog) + fr * ifog) >> 8);
}

/* GCN_GX_PIXEL_STATS BLEND bucket: everything from the moment alpha_test()
 * PASSES onward — late-Z (EmulatedZ::Late; it lives here, gated on the shaded
 * result, not in SLOPE which only ever runs early-Z) and BlendTev. Factored
 * out purely so the timed wrapper in tev_draw has one call to time regardless
 * of whether late-Z rejects (early return) or the pixel reaches BlendTev —
 * same technique as raster_pixel_prep. Behavior is unchanged from the inline
 * version this replaces. */
static void blend_stage(Tev* t, u8* output) {
    if (s_debug_pending_index >= 0) {
        s_debug_pending.blend_inputs++;
        s_debug_pending.output_rgba_sum[0] += output[RED_C];
        s_debug_pending.output_rgba_sum[1] += output[GRN_C];
        s_debug_pending.output_rgba_sum[2] += output[BLU_C];
        s_debug_pending.output_rgba_sum[3] += output[ALP_C];
    }
    if (s_cfg.zt_enable && !s_cfg.zt_early) {
        if (!ZCompare((u16)t->Position[0], (u16)t->Position[1], ztexture_depth(t))) {
            if (s_debug_pending_index >= 0)
                s_debug_pending.z_rejected++;
            return;
        }
    }
    BlendTev((u16)t->Position[0], (u16)t->Position[1], output);
    if (s_debug_pending_index >= 0 && s_bm_cu) {
        u32 off = (u32)(u16)t->Position[0] +
                  (u32)(u16)t->Position[1] * EFB_WIDTH;
        u32 efb = GetPixelColor(off);
        const u8* p = (const u8*)&efb;
        s_debug_pending.color_writes++;
        s_debug_pending.efb_rgba_sum[0] += p[RED_C];
        s_debug_pending.efb_rgba_sum[1] += p[GRN_C];
        s_debug_pending.efb_rgba_sum[2] += p[BLU_C];
        s_debug_pending.efb_rgba_sum[3] += p[ALP_C];
    }
    if (s_pixel_stats) s_ps_blend_writes++;   /* GCN_GX_PIXEL_STATS: blend_writes counter */
}

/* ============================================================================
 * Shared per-pixel shading bookkeeping (GCN_GX_STATS / GCN_GX_PIXEL_STATS /
 * GCN_GX_TEV_CENSUS side effects). Factored out of tev_draw's body so the
 * fused_pixel_A/B/C specializations below — which bypass tev_draw's stage
 * loop entirely — can reuse the exact same instrumentation instead of
 * silently going dark on every stats/census knob whenever a fused path is
 * selected. Pure extraction, no behavior change versus the code this
 * replaces in tev_draw.
 * ==========================================================================*/

/* Top-of-shading counters (was tev_draw's first 3 lines). `fused` (new, for
 * the fused-pixel-path task) additionally bumps the census bucket's
 * fused_pixels counter so gx_raster_print_census can report fused coverage —
 * gated on the same s_tev_census knob as the rest of the census, so it costs
 * nothing when the census is off. */
static inline void tev_stats_enter(int fused) {
    if (s_draw_stats) s_pixels_shaded++;    /* GCN_GX_STATS: pixels_shaded counter */
    if (s_pixel_stats) s_ps_shaded++;       /* GCN_GX_PIXEL_STATS: own shaded counter */
    if (s_census_cur >= 0) {
        s_census[s_census_cur].pixels++;               /* GCN_GX_TEV_CENSUS */
        if (fused) s_census[s_census_cur].fused_pixels++;
    }
}

/* GCN_GX_PIXEL_STATS TEX bucket: tex_sample total, timed only around the call
 * itself (was tev_draw's inline if/else). Used by tev_draw's stage loop AND
 * every fused function's one tex_sample call (config A/B/C all sample
 * exactly once per pixel — see the fused derivation comments). */
static inline void tev_sample_stat(int wid, u32 texmap, s32 texS, s32 texT,
                                   s32 lod, int linear, u8* texel) {
    if (s_pixel_stats) {
        s_ps_tex_calls++;
        if (linear) s_ps_tex_linear++; else s_ps_tex_point++;
        u64 tex_t0 = __rdtsc();
        tex_sample(wid, texmap, texS, texT, lod, linear, texel);
        s_tsc_tex += __rdtsc() - tex_t0;
    } else {
        tex_sample(wid, texmap, texS, texT, lod, linear, texel);
    }
}

static inline s32 tev_s24(s32 v) {
    return sext((u32)v & 0x00ffffffu, 24);
}

static inline s32 tev_ind_wrap(s32 coord, u32 mode) {
    if (mode == 0) return coord;
    if (mode < 6) return coord & (s32)(0xfffeu >> mode);
    return 0;
}

/* Apply one BP TevStageIndirect descriptor to the stage's regular S17.7
 * coordinate. This follows the low-level BP register model directly:
 * RAS1_IREF selects the indirect texcoord/texmap, TEXSCALE scales the lookup,
 * IND_MTXA/B/C supplies the 2x3 offset matrix, and the descriptor controls
 * format/bias/wrap/previous-coordinate carry. */
static void tev_indirect_coord(Tev* t, const TevStageCfg* sc,
                               s32 fixed_s, s32 fixed_t) {
    const u32 tevind = sc->tevind;
    if (tevind == 0) {
        t->TexCoordS = fixed_s;
        t->TexCoordT = fixed_t;
        return;
    }

    const u32 bt = bits(tevind, 0, 2);
    const u32 fmt = bits(tevind, 2, 2);
    const u32 bias = bits(tevind, 4, 3);
    const u32 bs = bits(tevind, 7, 2);
    const u32 matrix_index = bits(tevind, 9, 2);
    const u32 matrix_id = bits(tevind, 11, 2);
    s32 trans_s = 0, trans_t = 0;

    /* A lookup only affects the result when bump alpha or a matrix consumes
     * it. When bt names a disabled indirect stage the lookup result is
     * undefined on hardware; leave the translation at zero while preserving
     * wrapping/addprev below. */
    if ((bs != 0 || matrix_index != 0) && bt < gm_numindstages()) {
        const u32 iref = s_bp[0x27];
        const u32 ind_texmap = bits(iref, bt * 6, 3);
        u32 ind_texcoord = bits(iref, bt * 6 + 3, 3);
        if (ind_texcoord >= s_cfg.numtexgens) ind_texcoord = 0;

        const u32 scale_word = s_bp[0x25 + (bt >> 1)];
        const u32 scale_shift = (bt & 1) ? 8 : 0;
        const u32 scale_s = bits(scale_word, scale_shift, 4);
        const u32 scale_t = bits(scale_word, scale_shift + 4, 4);
        const s32 lookup_s = t->UvS[ind_texcoord] >> scale_s;
        const s32 lookup_t = t->UvT[ind_texcoord] >> scale_t;

        u8 sample[4];
        /* Indirect lookups use the selected texture unit's ordinary LOD and
         * filter, computed for this indirect stage by build_block. */
        tev_sample_stat(t->wid, ind_texmap, lookup_s, lookup_t,
                        t->IndirectLod[bt], t->IndirectLinear[bt], sample);

        /* GX indirect data is sampled as A, B, G (S, T, U). */
        s32 ind[3] = { sample[3], sample[2], sample[1] };
        if (bs != 0) t->AlphaBump = (u8)ind[bs - 1];

        if (fmt == 0) {
            for (u32 i = 0; i < 3; i++)
                if (bias & (1u << i)) ind[i] -= 128;
            t->AlphaBump &= 0xf8;
        } else {
            const u32 shift = fmt == 1 ? 3 : fmt == 2 ? 4 : 5;
            for (u32 i = 0; i < 3; i++)
                ind[i] = (ind[i] >> shift) + ((bias >> i) & 1u);
            t->AlphaBump = (u8)(t->AlphaBump << (8 - shift));
        }

        if (matrix_index != 0) {
            const u32 base = 0x06 + (matrix_index - 1) * 3;
            const u32 ma_word = s_bp[base + 0];
            const u32 mc_word = s_bp[base + 1];
            const u32 me_word = s_bp[base + 2];
            const s32 ma = sext(bits(ma_word, 0, 11), 11);
            const s32 mb = sext(bits(ma_word, 11, 11), 11);
            const s32 mc = sext(bits(mc_word, 0, 11), 11);
            const s32 md = sext(bits(mc_word, 11, 11), 11);
            const s32 me = sext(bits(me_word, 0, 11), 11);
            const s32 mf = sext(bits(me_word, 11, 11), 11);
            const u32 scale = bits(ma_word, 22, 2) |
                              (bits(mc_word, 22, 2) << 2) |
                              (bits(me_word, 22, 1) << 4);
            const s32 matrix_shift = 17 - (s32)scale;

            if (matrix_id == 0) {
                trans_s = (s32)(((s64)ma * ind[0] + (s64)mc * ind[1] +
                                  (s64)me * ind[2]) >> 3);
                trans_t = (s32)(((s64)mb * ind[0] + (s64)md * ind[1] +
                                  (s64)mf * ind[2]) >> 3);
            } else if (matrix_id == 1) {
                trans_s = (s32)(((s64)fixed_s * ind[0]) >> 8);
                trans_t = (s32)(((s64)fixed_t * ind[0]) >> 8);
            } else if (matrix_id == 2) {
                trans_s = (s32)(((s64)fixed_s * ind[1]) >> 8);
                trans_t = (s32)(((s64)fixed_t * ind[1]) >> 8);
            }

            if (matrix_shift >= 0) {
                trans_s >>= matrix_shift;
                trans_t >>= matrix_shift;
            } else {
                const u32 left = (u32)(-matrix_shift) & 31;
                trans_s = (s32)((u32)trans_s << left);
                trans_t = (s32)((u32)trans_t << left);
            }
        }
    }

    const s32 wrapped_s = tev_ind_wrap(fixed_s, bits(tevind, 13, 3));
    const s32 wrapped_t = tev_ind_wrap(fixed_t, bits(tevind, 16, 3));
    if (bits(tevind, 20, 1)) {
        t->TexCoordS = tev_s24(t->TexCoordS + wrapped_s + trans_s);
        t->TexCoordT = tev_s24(t->TexCoordT + wrapped_t + trans_t);
    } else {
        t->TexCoordS = tev_s24(wrapped_s + trans_s);
        t->TexCoordT = tev_s24(wrapped_t + trans_t);
    }
}

/* COMB-bucket-open (was tev_draw's `u64 comb_t0=0,tex_before=0; if(...)`). */
static inline u64 tev_comb_begin(u64* tex_before) {
    if (!s_pixel_stats) { *tex_before = 0; return 0; }
    *tex_before = s_tsc_tex;
    return __rdtsc();
}

/* COMB-bucket-close (pass exit) + blend dispatch (was tev_draw's final
 * if/else). blend_fn is blend_stage() for the general path or
 * fused_blend_stage() for a specialized config (see fused_pixel_A/B/C) — both
 * have identical signatures, so one shared timed-call site covers either. */
static inline void tev_comb_finish(Tev* t, u8* output, u64 comb_t0, u64 tex_before,
                                    void (*blend_fn)(Tev*, u8*)) {
    apply_fog(t, output);
    if (s_pixel_stats) {
        s_tsc_comb += (__rdtsc() - comb_t0) - (s_tsc_tex - tex_before);
        u64 blend_t0 = __rdtsc();
        blend_fn(t, output);
        s_tsc_blend += __rdtsc() - blend_t0;
    } else {
        blend_fn(t, output);
    }
}

/* Full Tev::Draw (Tev.cpp:387-683), scoped: no indirect/ztex/fog. The
 * indstages/ztex/fog guard depends only on BP genmode/ztex2/fogparam3 words —
 * none of it is per-pixel data — so it has moved to build_draw_cfg() (draw
 * entry) and fires once per draw instead of once per pixel; every other trap
 * below (active-indirect-stage, ras-color-channel) keeps its original
 * per-pixel/per-stage call site, just reading the pre-decoded TevStageCfg
 * field instead of re-extracting it from s_bp each time.
 *
 * GCN_GX_PIXEL_STATS COMB/TEX split: comb_t0/tex_before bracket this whole
 * function body (stage loop through alpha_test); TEX is timed at tex_sample's
 * call site below and subtracted back out at both exit points (alpha-test-fail
 * return and the fall-through into BLEND) — same before/after
 * accumulator-subtraction technique gx.c's DECODE bucket uses against its own
 * nested DRAW/EFB buckets. See the big s_pixel_stats comment near the top of
 * the file for the full bucket definitions and the alpha-test/late-Z boundary
 * rationale.
 *
 * This is the GENERAL path: every draw whose full shading config didn't match
 * one of fused_pixel_A/B/C's exact signatures (see build_draw_cfg's tail)
 * still runs this, unchanged, stage-loop selector switches and all — the
 * fused paths are a specialization ON TOP of this, never a replacement for
 * it (CLAUDE.md: "fold only constant decisions", requirement 5: general path
 * stays fully intact). */
static void tev_draw(Tev* t) {
    tev_stats_enter(0);

    u64 tex_before, comb_t0 = tev_comb_begin(&tex_before);

    u32 numstages = s_cfg.numtevstages;  /* actual count is +1 */
    u32 numtexgens = s_cfg.numtexgens;
    t->TexCoordS = 0;
    t->TexCoordT = 0;
    t->AlphaBump = 0;

    for (u32 stage = 0; stage <= numstages; stage++) {
        const TevStageCfg* sc = &s_cfg.stage[stage];
        u32 texcoordSel = sc->texcoordSel;
        u32 texmap      = sc->texmap;
        u32 enable      = sc->enable;
        u32 colorchan   = sc->colorchan;

        tev_indirect_coord(t, sc, t->UvS[texcoordSel], t->UvT[texcoordSel]);

        if (enable) {
            u8 texel[4];
            if (numtexgens > 0) {
                tev_sample_stat(t->wid, texmap, t->TexCoordS, t->TexCoordT,
                                 t->TextureLod[stage],
                                 t->TextureLinear[stage], texel);
            } else {
                memset(texel, 0, 4);
            }
            t->RawTexColor.r = texel[0]; t->RawTexColor.g = texel[1];
            t->RawTexColor.b = texel[2]; t->RawTexColor.a = texel[3];
            const u32* sw = s_cfg.swaptab[sc->tswap_id];  /* ac.tswap */
            t->TexColor.r = texel[sw[0]]; t->TexColor.g = texel[sw[1]];
            t->TexColor.b = texel[sw[2]]; t->TexColor.a = texel[sw[3]];
        }

        /* konst for this stage (kcsel/kasel, BPMemory AllTevKSels) — StageKonst
         * itself is draw-invariant (see TevStageCfg comment), so it is just
         * copied out of the cache instead of re-run through konst_lookup(). */
        t->StageKonst = sc->stage_konst;

        /* ras color (SetRasColor, Tev.cpp:34-78). */
        {
            const u32* rsw = s_cfg.swaptab[sc->rswap_id];  /* ac.rswap */
            const u8* col = NULL;
            if (colorchan == 0) col = t->Color[0];
            else if (colorchan == 1) col = t->Color[1];
            if (col) {
                t->RasColor.r = col[rsw[0]]; t->RasColor.g = col[rsw[1]];
                t->RasColor.b = col[rsw[2]]; t->RasColor.a = col[rsw[3]];
            } else if (colorchan == 5 || colorchan == 6) {
                u8 bump = t->AlphaBump;
                if (colorchan == 6) bump |= bump >> 5;
                t->RasColor.r = t->RasColor.g = t->RasColor.b = t->RasColor.a = bump;
            } else {
                t->RasColor.r = t->RasColor.g = t->RasColor.b = t->RasColor.a = 0;
            }
        }

        /* combine inputs */
        s32 inA[4], inB[4], inC[4], inD[4];
        s16 r, g, b;
        color_arg(t, sc->argA, &r, &g, &b); inA[RED_C]=r; inA[GRN_C]=g; inA[BLU_C]=b;
        color_arg(t, sc->argB, &r, &g, &b); inB[RED_C]=r; inB[GRN_C]=g; inB[BLU_C]=b;
        color_arg(t, sc->argC, &r, &g, &b); inC[RED_C]=r; inC[GRN_C]=g; inC[BLU_C]=b;
        color_arg(t, sc->argD, &r, &g, &b); inD[RED_C]=r; inD[GRN_C]=g; inD[BLU_C]=b;
        inA[ALP_C] = alpha_arg(t, sc->aargA);
        inB[ALP_C] = alpha_arg(t, sc->aargB);
        inC[ALP_C] = alpha_arg(t, sc->aargC);
        inD[ALP_C] = alpha_arg(t, sc->aargD);

        if (sc->c16 != 3) draw_color_regular(t, sc, inA, inB, inC, inD);
        else              draw_color_compare(t, sc, inA, inB, inC, inD);
        if (sc->a16 != 3) draw_alpha_regular(t, sc, inA[ALP_C], inB[ALP_C], inC[ALP_C], inD[ALP_C]);
        else              draw_alpha_compare(t, sc, inA, inB, inC[ALP_C], inD[ALP_C]);
    }

    const TevStageCfg* last_sc = &s_cfg.stage[numstages];
    u32 color_dest = last_sc->c22;
    u32 alpha_dest = last_sc->a22;
    u8 output[4];
    output[ALP_C] = (u8)t->Reg[alpha_dest].a;
    output[BLU_C] = (u8)t->Reg[color_dest].b;
    output[GRN_C] = (u8)t->Reg[color_dest].g;
    output[RED_C] = (u8)t->Reg[color_dest].r;

    if (s_debug_pending_index >= 0) {
        u32 alpha = output[ALP_C];
        u32 tex_alpha = (u8)t->RawTexColor.a;
        s_debug_pending.alpha_tested++;
        s_debug_pending.alpha_sum += alpha;
        s_debug_pending.last_tex_alpha_sum += tex_alpha;
        if (alpha < s_debug_pending.alpha_min)
            s_debug_pending.alpha_min = alpha;
        if (alpha > s_debug_pending.alpha_max)
            s_debug_pending.alpha_max = alpha;
        if (tex_alpha < s_debug_pending.last_tex_alpha_min)
            s_debug_pending.last_tex_alpha_min = tex_alpha;
        if (tex_alpha > s_debug_pending.last_tex_alpha_max)
            s_debug_pending.last_tex_alpha_max = tex_alpha;
    }
    if (!alpha_test(output[ALP_C])) {
        if (s_debug_pending_index >= 0)
            s_debug_pending.alpha_rejected++;
        /* COMB bucket boundary (fail exit): stage loop + alpha_test, minus the
         * nested TEX delta accumulated above. */
        if (s_pixel_stats) s_tsc_comb += (__rdtsc() - comb_t0) - (s_tsc_tex - tex_before);
        return;
    }

    /* COMB bucket boundary (pass exit) + blend, timed together. */
    tev_comb_finish(t, output, comb_t0, tex_before, blend_stage);
}

/* ============================================================================
 * Fused specialized per-pixel TEV paths (perf task, CLAUDE.md gx-raster COMB
 * bucket). GCN_GX_TEV_CENSUS=1 over a real IPL-menu boot (5,000,000 ticks,
 * 2026-07-12) showed the entire menu's shaded pixels concentrate into 8
 * distinct full shading configs, and 3 of those alone cover ~95.7% of every
 * shaded pixel in the whole run:
 *
 *   config A: hash=3a916e1f stages=1 texgens=1 st0 cc=00F8CF ac=00F670       -- 44.3% px
 *   config B: hash=62776c53 stages=2 texgens=1 st0 cc=18F28F ac=08F670 en=1
 *                                              st1 cc=08FC0F ac=08F870 en=0  -- 45.8% px
 *   config C: hash=eefdb25f stages=1 texgens=1 st0 cc=18F28F ac=08F670       --  5.6% px
 *   (all: colchan=0, numcolchans=1, zt_enable=0, zt_early=0, da_enable=0,
 *    bm_blend_enable=1, bm_logic_enable=0, bm_subtract=0, bm_dither=1,
 *    bm_src_factor=4/SrcAlpha, bm_dst_factor=5/InvSrcAlpha, color_update=1,
 *    alpha_update=1, alpha-test word=0x7F0000, swaptab[0]==identity)
 *
 * The general tev_draw() stage loop re-decodes a fixed set of selector
 * switches (color_arg/alpha_arg cases, draw_color_regular's bias/op/scale/
 * dest, swap-table indirection, konst copy, the c16==3/a16==3 compare-mode
 * branch) on EVERY PIXEL even though every one of those decisions is
 * DRAW-INVARIANT once cc/ac/enable/colorchan/numtexgens/numcolchans and the
 * blend/z/alpha-test words are fixed (which build_draw_cfg's selection below
 * verifies bit-for-bit, not just "probably"). The three functions below fold
 * that decision tree once per DRAW (at selection time) into straight-line C
 * that reads only the genuinely per-pixel-varying inputs (RasColor, TexColor)
 * and per-draw-constant data values (TEV Reg[]/Konst[] registers — read
 * live, never assumed) with no remaining runtime branch on any folded
 * selector.
 *
 * EXACTNESS: every fold below was verified bit-exact against the general
 * path's own integer semantics (draw_color_regular/draw_alpha_regular,
 * transcribed verbatim into a standalone checker) by brute force over the
 * FULL possible range of every folded operand — not just plausible asset
 * values: TEV Reg[]/Konst[] components are signed 11-bit
 * (tev_load_registers' sext(...,11) => -1024..1023) wherever a fold uses one,
 * swept over that entire range, not just 0..255. Total: >850,000 case
 * combinations checked, 0 mismatches. See each function's derivation comment
 * for its own bit-field decode and formula. Blend folding (fused_blend_stage,
 * below) was verified the same way (200,000 random trials + an exhaustive
 * sweep over the source-alpha byte, 0 mismatches).
 *
 * D1 EXTENSION (2026-07-13): GCN_GX_TEV_CENSUS=1 over the BOOT ANIMATION
 * (rolling-cube G-logo, 24,000,000-block budget, mid-animation) showed A/B/C
 * above cover only 82.5% cumulative / ~63% and falling instantaneously by the
 * end of that window — the animation's dominant per-window growth is a NEW
 * config not present in the menu census:
 *
 *   config D: hash=9851f9ff stages=1 texgens=1 st0 cc=08FCAF ac=08F2F0       -- rising
 *   (same draw-global word set as A/B/C above: fused_common_match's
 *    precondition list, unchanged)
 *
 * fused_pixel_D below folds it the same way, verified bit-exact the same way
 * (standalone transcription of draw_color_regular/draw_alpha_regular, brute
 * force over the full domain — see fused_pixel_D's own derivation comment;
 * this config's cc/ac reference only RasColor/TexColor, both u8 0..255, no
 * TEV Reg/Konst operand, so the domain is 256 + 65536 = 65,792 combinations,
 * 0 mismatches).
 *
 * D1 RESIDUALS SWEEP (2026-07-13): with D covered, a re-run of
 * GCN_GX_TEV_CENSUS=1 over the same boot-animation window (24,000,000
 * blocks) found fused coverage at 94.1%, with exactly two configs left
 * uncovered — the same two the D1 census first spotted (then hashed
 * "40bead5f"/"8b773e26") but re-censused fresh rather than trusted from
 * memory, per the project's "evaluate with data before implementing"
 * discipline. That re-census caught a real mistake: both were assumed to be
 * bm_blend_enable==0 ("write-through, no blend", implying a new no-blend
 * fused_blend variant would be needed). The fresh dump shows otherwise —
 * both already satisfy every existing blend precondition
 * (fused_blend_common_match) bit-for-bit:
 *
 *   config E: hash=40bead5f stages=1 texgens=1 st0 cc=18428F ac=08F770       -- 3.6%->4.1%, rising
 *   config F: hash=8b773e26 stages=1 texgens=0 st0 cc=00AFFF ac=00BFF0       -- 1.8%, ~flat
 *   (both: blend=1 sf=4(SrcAlpha) df=5(InvSrcAlpha) da=0/0 dither=1
 *    at=0x7F0000 — the SAME blend/alpha-test word set as A/B/C/D; no new
 *    fused_blend_stage variant needed, both reuse it as-is)
 *
 * config E has texgens=1 like A/B/C/D, so it fits under the existing
 * fused_common_match() unchanged — just a new stage signature. config F has
 * texgens=0 (no texture unit sampled at all), which fused_common_match()'s
 * numtexgens==1 check would reject — fused_common_match was split into a
 * shared fused_blend_common_match() plus two thin callers (numtexgens==1 for
 * A/B/C/D/E, numtexgens==0 for F) rather than special-cased, so the split is
 * itself general infrastructure, not a one-off carve-out. See fused_pixel_E
 * and fused_pixel_F's own derivation comments for the bit-exact brute-force
 * verification of each (1.07 billion cases for E's two-register color lerp,
 * 524,288 for E's alpha, 2048/256 for F's trivial RasColor passthrough).
 * With E+F added, a re-run of GCN_GX_TEV_CENSUS=1 over the same window
 * confirms 100.0% fused coverage: every one of the 8 censused configs shows
 * fused=N(100.0%) and total_fused==total shaded pixels at every print in the
 * run (2026-07-13).
 * ==========================================================================*/

/* Folded BlendTev/BlendColor for configs A/B/C. Requires (checked once, at
 * selection time, by build_draw_cfg — see fused_common_match): zt_enable==0
 * (the general blend_stage's `if (zt_enable && !zt_early) return` is always
 * false here, so late-Z is skipped outright, not just usually-false-checked);
 * bm_blend_enable==1 && bm_subtract==0 && bm_logic_enable==0 (BlendColor is
 * the ONLY branch BlendTev can take, never SubtractBlend/LogicBlend);
 * bm_src_factor==4(SrcAlpha)/bm_dst_factor==5(InvSrcAlpha) (src_factor/
 * dst_factor's switch collapses to a splat of the shaded pixel's OWN alpha /
 * its inverse — note dst_factor's case 5 also keys off the SOURCE alpha, not
 * dst's, so both factors derive from `output[ALP_C]` alone); da_enable==0 (no
 * dst-alpha override); s_bm_cu==1 && s_bm_au==1 (always the Dither-then-
 * SetPixelAlphaColor branch, never SetPixelColorOnly/SetPixelAlphaOnly).
 *
 * NOT folded (deliberately, per the derivation): GetPixelColor/
 * SetPixelAlphaColor's own pixel_format switch (pixel_format is a genuinely
 * separate EFB-storage-format concern, orthogonal to the TEV combiner shape
 * this task targets, and both helpers are cheap/already-shared — no need to
 * assume or check a pixel_format constant here), and Dither's x/y parity +
 * s_pf==RGBA6_Z24 gate (real per-pixel data — the general Dither() is called
 * exactly as tev_draw's own blend_stage would call it). */
static inline void fused_blend_stage(Tev* t, u8* output) {
    u32 off = (u32)(u16)t->Position[0] + (u32)(u16)t->Position[1] * EFB_WIDTH;
    u32 dstClr = GetPixelColor(off);
    u8* d = (u8*)&dstClr;

    /* BlendColor folded: sf/df are always a splat of output[ALP_C] / its
     * inverse (SrcAlpha/InvSrcAlpha), so `s`/`d` in the general per-channel
     * loop are the SAME two scalars on every one of the 4 iterations — hoist
     * them out instead of re-deriving a 32-bit splat and re-masking &0xff
     * each time (verified bit-exact, see the file-header derivation note). */
    u32 srcA = output[ALP_C];
    u32 sa = srcA + (srcA >> 7);
    u32 invA = 255u - srcA;
    u32 da = invA + (invA >> 7);
    for (int i = 0; i < 4; i++) {
        u32 c = (output[i] * sa + d[i] * da) >> 8;
        d[i] = (c > 255u) ? 255u : (u8)c;
    }

    /* da_enable==0 folded away: no dstPtr[ALP_C]=da_alpha override. */
    Dither((u16)t->Position[0], (u16)t->Position[1], d);   /* real call, see above */
    SetPixelAlphaColor(off, d);                             /* s_bm_cu==1 && s_bm_au==1 */
    if (s_pixel_stats) s_ps_blend_writes++;   /* GCN_GX_PIXEL_STATS: blend_writes counter */
}

/* Same blend fold as fused_blend_stage (sf=4/df=5, da_enable==0), but for
 * configs Y/AC whose matcher pins cu==1 WITHOUT pinning au (see the Y/Z/AA
 * derivation comment above gpu_program_Y_match): the final write branches on
 * the live s_bm_au flag exactly the way general BlendTev does, instead of
 * assuming au==1. */
static inline void fused_blend_stage_auto(Tev* t, u8* output) {
    u32 off = (u32)(u16)t->Position[0] + (u32)(u16)t->Position[1] * EFB_WIDTH;
    u32 dstClr = GetPixelColor(off);
    u8* d = (u8*)&dstClr;
    u32 srcA = output[ALP_C];
    u32 sa = srcA + (srcA >> 7);
    u32 invA = 255u - srcA;
    u32 da = invA + (invA >> 7);
    for (int i = 0; i < 4; i++) {
        u32 c = (output[i] * sa + d[i] * da) >> 8;
        d[i] = (c > 255u) ? 255u : (u8)c;
    }
    Dither((u16)t->Position[0], (u16)t->Position[1], d);
    if (s_bm_au) SetPixelAlphaColor(off, d); else SetPixelColorOnly(off, d);
    if (s_pixel_stats) s_ps_blend_writes++;
}

/* Config Z's finish: blend_enable==0 && logic_enable==0 && subtract==0, so
 * BlendTev's three-way blend/logic/passthrough branch collapses to its
 * `else { dstPtr = color; }` case -- output is written as-is, no read of the
 * current dst pixel needed for the color math at all (still writes through
 * GetPixelColor's offset math only to share SetPixel*'s dst-preserving byte
 * for whichever channel this cu/au combo doesn't touch). da_enable==0 folded
 * away (matcher pins it). au read live, same reasoning as fused_blend_stage_auto. */
static inline void fused_write_stage_auto(Tev* t, u8* output) {
    u32 off = (u32)(u16)t->Position[0] + (u32)(u16)t->Position[1] * EFB_WIDTH;
    Dither((u16)t->Position[0], (u16)t->Position[1], output);
    if (s_bm_au) SetPixelAlphaColor(off, output); else SetPixelColorOnly(off, output);
    if (s_pixel_stats) s_ps_blend_writes++;
}

/* Config AA's finish: sf=6(DstAlpha)/df=1(One) reads the framebuffer's own
 * current alpha for the source blend factor -- a genuinely per-pixel-varying
 * value, not foldable to a hoisted scalar the way Y's sf=4/df=5 is -- so this
 * does the real per-channel BlendColor math (dst_factor==One's rounded
 * scalar, 0xff+(0xff>>7)==256, IS a compile-time constant, hoisted). The
 * da_enable==1/da_alpha==0 override runs unconditionally afterward (proven
 * in the derivation comment above gpu_program_AA_match), so the fold skips
 * computing/writing a blended alpha entirely and stores 0 directly. cu==1 &&
 * au==1 both pinned by the matcher (no live branch needed here). */
static inline void fused_blend_stage_dstalpha_da0(Tev* t, u8* output) {
    u32 off = (u32)(u16)t->Position[0] + (u32)(u16)t->Position[1] * EFB_WIDTH;
    u32 dstClr = GetPixelColor(off);
    u8* d = (u8*)&dstClr;
    u32 da_before = d[ALP_C];
    u32 sa = da_before + (da_before >> 7);
    for (int i = 0; i < 4; i++) {
        u32 c = (output[i] * sa + d[i] * 256u) >> 8;
        d[i] = (c > 255u) ? 255u : (u8)c;
    }
    d[ALP_C] = s_cfg.da_alpha;   /* == 0; unconditional da_enable override */
    Dither((u16)t->Position[0], (u16)t->Position[1], d);
    SetPixelAlphaColor(off, d);   /* s_bm_cu==1 && s_bm_au==1, matcher-pinned */
    if (s_pixel_stats) s_ps_blend_writes++;
}

/* Fused pixel functions Y/Z/AA (see the derivation comment above
 * gpu_program_Y_match for the combiner decode). All three are texgens==0,
 * en==0 -- no texture sample at all, so these are the cheapest fused
 * functions in the file: no tev_sample_stat call, just a RasColor read. */
static void fused_pixel_Y(Tev* t) {
    tev_stats_enter(1);
    u64 tex_before, comb_t0 = tev_comb_begin(&tex_before);
    u8 output[4];
    output[RED_C] = t->Color[0][0];
    output[GRN_C] = t->Color[0][1];
    output[BLU_C] = t->Color[0][2];
    output[ALP_C] = t->Color[0][3];
    /* alpha test always passes -- at word 0x3F0000 decodes to comp0==
     * comp1==CMP_ALWAYS with AND, same always-pass proof as 0x7F0000
     * (see fused_pixel_A's derivation and gpu_program_Y_match's comment). */
    tev_comb_finish(t, output, comb_t0, tex_before, fused_blend_stage_auto);
}
static void fused_pixel_Z(Tev* t) {
    tev_stats_enter(1);
    u64 tex_before, comb_t0 = tev_comb_begin(&tex_before);
    u8 output[4];
    output[RED_C] = t->Color[0][0];
    output[GRN_C] = t->Color[0][1];
    output[BLU_C] = t->Color[0][2];
    output[ALP_C] = t->Color[0][3];
    tev_comb_finish(t, output, comb_t0, tex_before, fused_write_stage_auto);
}
static void fused_pixel_AA(Tev* t) {
    tev_stats_enter(1);
    u64 tex_before, comb_t0 = tev_comb_begin(&tex_before);
    u8 output[4];
    output[RED_C] = t->Color[0][0];
    output[GRN_C] = t->Color[0][1];
    output[BLU_C] = t->Color[0][2];
    output[ALP_C] = 0;   /* ac=0x08FFF0: every alpha arg is ZERO -> result 0 */
    tev_comb_finish(t, output, comb_t0, tex_before, fused_blend_stage_dstalpha_da0);
}

/* Shared preconditions for every fused config (see the file-header derivation
 * table above): everything EXCEPT numtexgens (checked by the two callers
 * below — the boot-animation D1 census's configs E/F needed a texgens==0
 * variant, see their derivation comments) and the per-stage cc/ac/enable/
 * colorchan words themselves, which each fused_pixel_* signature check below
 * compares separately (an exact cc/ac word match already pins every selector
 * the fold depends on — argA..D/aargA..D, bias, op, clamp, scale, dest,
 * tswap_id, rswap_id — in one comparison, since those are exactly the bits
 * build_draw_cfg extracted them from). swaptab[0] must still be checked
 * separately: the cc/ac match only pins tswap_id/rswap_id to 0, not what
 * table entry 0 itself currently holds (a separate BP-register-driven array)
 * — see the derivation table's "swaptab[0]==identity" note. */
static int fused_blend_common_match(void) {
    for (u32 stage = 0; stage <= s_cfg.numtevstages; stage++)
        if (s_cfg.stage[stage].tevind != 0) return 0;
    return s_cfg.numcolchans == 1 &&
           s_cfg.zt_enable == 0 &&
           s_cfg.da_enable == 0 &&
           s_cfg.bm_blend_enable == 1 && s_cfg.bm_logic_enable == 0 &&
           s_cfg.bm_subtract == 0 && s_cfg.bm_dither == 1 &&
           s_cfg.bm_src_factor == 4 && s_cfg.bm_dst_factor == 5 &&
           s_bm_cu == 1 && s_bm_au == 1 &&
           s_bp[0xF3] == 0x7F0000u &&
           s_cfg.swaptab[0][0] == 0 && s_cfg.swaptab[0][1] == 1 &&
           s_cfg.swaptab[0][2] == 2 && s_cfg.swaptab[0][3] == 3;
}
/* numtexgens==1 variant — configs A/B/C/D/E, every one of which actually
 * samples a texture (their stage's enable==1 and numtexgens>0, so
 * tev_draw's `if (numtexgens > 0) tex_sample_stat(...)` branch is live). */
static int fused_common_match(void) {
    return s_cfg.numtexgens == 1 && fused_blend_common_match();
}
/* numtexgens==0 variant — config F (see its derivation comment): no texture
 * unit is ever sampled (tev_draw's own `else memset(texel,0,4)` branch is
 * what the general path takes here), so TexColor plays no role in F's
 * fold at all and this checks numtexgens==0 instead of ==1. */
static int fused_common_match_notex(void) {
    return s_cfg.numtexgens == 0 && fused_blend_common_match();
}

/* Exact per-stage cc/ac/enable/colorchan compare — see fused_common_match's
 * comment for why matching the raw cc/ac words is sufficient to pin every
 * argument/bias/op/clamp/scale/dest/swap-id selector the fold depends on. */
static inline int fused_stage_match(u32 stage_idx, u32 exp_cc, u32 exp_ac,
                                     u32 exp_enable, u32 exp_colorchan) {
    const TevStageCfg* sc = &s_cfg.stage[stage_idx];
    return sc->cc == exp_cc && sc->ac == exp_ac &&
           sc->enable == exp_enable && sc->colorchan == exp_colorchan;
}

/* GPU-only depth program K. Software deliberately stays on the general path
 * so its ordinary z slope/test/update remains the differential authority. */
static int gpu_depth_K_match(void) {
    return s_cfg.numtevstages == 0 && s_cfg.numtexgens == 1 &&
           s_cfg.numcolchans == 1 &&
           s_cfg.zt_enable == 1 && s_cfg.zt_early == 1 &&
           s_cfg.zt_func == CMP_LEQUAL && s_zt_upd == 1 &&
           s_cfg.da_enable == 0 &&
           s_cfg.bm_blend_enable == 1 && s_cfg.bm_logic_enable == 0 &&
           s_cfg.bm_subtract == 0 && s_cfg.bm_dither == 1 &&
           s_cfg.bm_src_factor == 4 && s_cfg.bm_dst_factor == 5 &&
           s_bm_cu == 1 && s_bm_au == 1 && s_bp[0xF3] == 0x7F0000u &&
           s_cfg.swaptab[0][0] == 0 && s_cfg.swaptab[0][1] == 1 &&
           s_cfg.swaptab[0][2] == 2 && s_cfg.swaptab[0][3] == 3 &&
           fused_stage_match(0, 0x00F8CFu, 0x00F770u, 1, 0);
}

/* GPU-only programs L--Q cover the remaining exact IPL states observed with
 * persistent SRAM/cards.  Software intentionally remains on tev_draw() so it
 * is the independent differential authority for every new program. */
static int gpu_depth_L_match(void) {
    return s_cfg.numtevstages == 0 && s_cfg.numtexgens == 1 &&
           s_cfg.numcolchans == 1 &&
           s_cfg.zt_enable == 1 && s_cfg.zt_early == 1 &&
           s_cfg.zt_func == CMP_LEQUAL && s_zt_upd == 1 &&
           s_cfg.da_enable == 0 &&
           s_cfg.bm_blend_enable == 1 && s_cfg.bm_logic_enable == 0 &&
           s_cfg.bm_subtract == 0 && s_cfg.bm_dither == 1 &&
           s_cfg.bm_src_factor == 4 && s_cfg.bm_dst_factor == 5 &&
           s_bm_cu == 1 && s_bm_au == 1 && s_bp[0xF3] == 0x7F0000u &&
           s_cfg.swaptab[0][0] == 0 && s_cfg.swaptab[0][1] == 1 &&
           s_cfg.swaptab[0][2] == 2 && s_cfg.swaptab[0][3] == 3 &&
           fused_stage_match(0, 0x00428Fu, 0x00F770u, 1, 0);
}

static int gpu_program_M_match(void) {
    return fused_common_match_notex() && s_cfg.numtevstages == 0 &&
           fused_stage_match(0, 0x08FFFAu, 0x08FFD0u, 0, 0);
}

static int gpu_program_N_match(void) {
    return fused_common_match() && s_cfg.numtevstages == 1 &&
           fused_stage_match(0, 0x18F28Fu, 0x18F670u, 1, 0) &&
           fused_stage_match(1, 0x08FC0Fu, 0x08F870u, 0, 0);
}

static int gpu_program_O_match(void) {
    return fused_common_match() && s_cfg.numtevstages == 1 &&
           fused_stage_match(0, 0x08FA82u, 0x38F610u, 1, 0) &&
           fused_stage_match(1, 0x08FC0Fu, 0x08F870u, 0, 0);
}

static int gpu_replace_common_match(u32 numtexgens) {
    return s_cfg.numtevstages == 0 && s_cfg.numtexgens == numtexgens &&
           s_cfg.numcolchans == 1 &&
           s_cfg.zt_enable == 0 && s_cfg.zt_early == 0 &&
           s_cfg.zt_func == CMP_LEQUAL && s_zt_upd == 0 &&
           s_cfg.da_enable == 0 &&
           s_cfg.bm_blend_enable == 0 && s_cfg.bm_logic_enable == 0 &&
           s_cfg.bm_subtract == 0 && s_cfg.bm_dither == 1 &&
           s_cfg.bm_src_factor == 4 && s_cfg.bm_dst_factor == 5 &&
           s_bm_cu == 1 && s_bm_au == 1 && s_bp[0xF3] == 0x3F0000u &&
           s_cfg.swaptab[0][0] == 0 && s_cfg.swaptab[0][1] == 1 &&
           s_cfg.swaptab[0][2] == 2 && s_cfg.swaptab[0][3] == 3;
}

static int gpu_program_P_match(void) {
    return gpu_replace_common_match(0) &&
           fused_stage_match(0, 0x00AFFFu, 0x00BFF0u, 1, 0);
}

static int gpu_program_Q_match(void) {
    return gpu_replace_common_match(1) &&
           fused_stage_match(0, 0x00F8CFu, 0x00F670u, 1, 0);
}

static int gpu_program_R_match(void) {
    return fused_common_match() && s_cfg.numtevstages == 0 &&
           fused_stage_match(0, 0x08FA82u, 0x38F610u, 1, 0);
}

static int gpu_program_S_match(void) {
    return fused_common_match() && s_cfg.numtevstages == 1 &&
           fused_stage_match(0, 0x18FD82u, 0x08F770u, 1, 0) &&
           fused_stage_match(1, 0x08FC0Fu, 0x08F870u, 0, 0);
}

/* GPU-only programs T--X: the card-manager screen's five states (2026-07-16
 * GCN_GX_TEV_CENSUS on that screen, configs a621c9bf / 83c9025f / 5095bc53 /
 * 62b7a1ff / 34b4545f).  Every one previously classified program 0 and forced
 * a synchronized resident fallback (413K/30s measured, 42.5 -> 6.6 fps).
 * Same convention as K--S: software stays on the general tev_draw() path as
 * the differential authority.  Each fold was brute-force verified bit-exact
 * against a verbatim draw_color_regular/draw_alpha_regular transcription over
 * the FULL operand domains (Reg/Konst swept -1024..1023, ras/tex 0..255;
 * 3,145,984 cases, 0 mismatches — scratchpad verify_txw.c, 2026-07-16). */

/* T: untextured flat color (the dominant card-grid fill state).  Color is
 * the A=0,B=ONE,C=RasColor identity; alpha = Konst0.a modulated by
 * RasColor.a's cc.  Stage never samples (enable=0, texgens=0). */
static int gpu_program_T_match(void) {
    return fused_common_match_notex() && s_cfg.numtevstages == 0 &&
           fused_stage_match(0, 0x08FCAFu, 0x08FAF0u, 0, 0);
}

/* U: program C's color (Reg1.rgb * texel, x2, clamp) but alpha from
 * Reg1.a * texel.a's cc instead of RasColor.a — the card screen's draw-count
 * driver (~194K draws per census window). */
static int gpu_program_U_match(void) {
    return fused_common_match() && s_cfg.numtevstages == 0 &&
           fused_stage_match(0, 0x18F28Fu, 0x08E670u, 1, 0);
}

/* V: U plus the standard konst-alpha second stage (same stage-1 words every
 * two-stage program in this family uses). */
static int gpu_program_V_match(void) {
    return fused_common_match() && s_cfg.numtevstages == 1 &&
           fused_stage_match(0, 0x18F28Fu, 0x08E670u, 1, 0) &&
           fused_stage_match(1, 0x08FC0Fu, 0x08F870u, 0, 0);
}

/* W: T's exact combiner but with a texture stage enabled (texgens=1) whose
 * texel no selector references — sampled-and-ignored, so the GPU program
 * skips the fetch and the output is identical. */
static int gpu_program_W_match(void) {
    return fused_common_match() && s_cfg.numtevstages == 0 &&
           fused_stage_match(0, 0x08FCAFu, 0x08FAF0u, 1, 0);
}

/* X: S's stage 0 alone (no second stage) under the K/L depth state
 * (early LEQUAL test + update) — the card-entry transition geometry. */
static int gpu_program_X_match(void) {
    return s_cfg.numtevstages == 0 && s_cfg.numtexgens == 1 &&
           s_cfg.numcolchans == 1 &&
           s_cfg.zt_enable == 1 && s_cfg.zt_early == 1 &&
           s_cfg.zt_func == CMP_LEQUAL && s_zt_upd == 1 &&
           s_cfg.da_enable == 0 &&
           s_cfg.bm_blend_enable == 1 && s_cfg.bm_logic_enable == 0 &&
           s_cfg.bm_subtract == 0 && s_cfg.bm_dither == 1 &&
           s_cfg.bm_src_factor == 4 && s_cfg.bm_dst_factor == 5 &&
           s_bm_cu == 1 && s_bm_au == 1 && s_bp[0xF3] == 0x7F0000u &&
           s_cfg.swaptab[0][0] == 0 && s_cfg.swaptab[0][1] == 1 &&
           s_cfg.swaptab[0][2] == 2 && s_cfg.swaptab[0][3] == 3 &&
           fused_stage_match(0, 0x18FD82u, 0x08F770u, 1, 0);
}

/* GPU/fused programs Y--AD: the six dominant Wind Waker "general" (program-0)
 * draw shapes named by a GCN_GX_TEV_CENSUS run over the title route
 * (captures/perf-census-20260803/run1.err.log, 2026-08-03), covering ~71% of
 * general pixels. Every fold below was brute-force verified bit-exact against
 * a verbatim transcription of draw_color_regular/draw_alpha_regular (and,
 * for AA, BlendColor) over the FULL signed-11-bit domain for every TEV
 * Reg/Konst operand and the full 0..255 domain for every Ras/TexColor byte
 * operand: 529,153 combiner cases (scratchpad verify_yz_aa_ab.c) + 16,777,216
 * blend cases for AA (scratchpad verify_shape3_blend.c), 0 mismatches, 2026-08-03.
 *
 * The census dump omits two hashed-but-unprinted fields: s_bm_cu/s_bm_au
 * (color_update/alpha_update, bp 0x41 bits 3/4). That's what distinguishes
 * the two pairs the task investigation started from -- hash 31a3e87e/20faf539
 * and c0e62b05/672366ca each print IDENTICAL dumps but differ in bm_au (cu=1
 * in all four; au=1 for the first of each pair, au=0 for the second) --
 * NOT cullmode (cullmode isn't hashed into the census key at all, same as
 * every existing program here). Rather than splitting each pair into two
 * near-duplicate programs the way T/W split on texgens, Y and Z generalize:
 * their matchers pin cu==1 but deliberately do NOT pin au, and their finish
 * helpers branch on the live s_bm_au flag exactly the way general BlendTev
 * does (Dither, then SetPixelAlphaColor if au else SetPixelColorOnly) --
 * this is the one config-hoisted branch in this whole fused family, added
 * because it is the one place two real census configs differ only in a
 * write-mask bit, not in any part of the pixel VALUE computation.
 *
 * zt_enable splits the six the same way the K--X family already established
 * the write-up discipline: Y/Z/AA (zt_enable==0) are safe to fuse into the
 * software raster too (no z semantics at stake); AB/AC/AD (zt_enable==1,
 * early LEQUAL/func3, zupd==0 -- test only, never write) stay GPU-only so
 * software's ordinary z slope/test remains the differential authority. */

/* Y/Z shared combiner: cc=0x08FFFA (argA=argB=argC=ZERO, argD=RasColor.rgb,
 * bias0 op0 clamp255 scale0) / ac=0x08FFD0 (aargA=aargB=aargC=ZERO(7),
 * aargD=RasColor.a, bias0 op0 clamp255 scale0). Both reduce to the algebraic
 * identity result==D exactly (A=B=0 makes the lerp term 0 regardless of C;
 * the bias/scale/rshift chain is then a pure passthrough of D) -- COLOR is
 * RasColor.rgb bit-for-bit, ALPHA is RasColor.a bit-for-bit, no Reg/Konst/Tex
 * operand appears anywhere in this combiner. Verified for the full 0..255
 * domain of every RasColor byte (each channel independent of the others). */
static int fused_common_notex_at3F(void) {
    for (u32 stage = 0; stage <= s_cfg.numtevstages; stage++)
        if (s_cfg.stage[stage].tevind != 0) return 0;
    return s_cfg.numtexgens == 0 && s_cfg.numcolchans == 1 &&
           s_cfg.zt_enable == 0 &&
           s_cfg.da_enable == 0 &&
           s_cfg.bm_logic_enable == 0 && s_cfg.bm_subtract == 0 &&
           s_cfg.bm_dither == 1 &&
           s_bm_cu == 1 &&   /* au deliberately NOT pinned -- see Y/Z's note */
           s_bp[0xF3] == 0x3F0000u &&   /* comp0=comp1=ALWAYS regardless of AND/OR,
                                          * same always-pass proof as 0x7F0000 (see
                                          * fused_pixel_A's derivation) -- also
                                          * already used unpinned-blend by P/Q. */
           s_cfg.swaptab[0][0] == 0 && s_cfg.swaptab[0][1] == 1 &&
           s_cfg.swaptab[0][2] == 2 && s_cfg.swaptab[0][3] == 3;
}
static int gpu_program_Y_match(void) {
    return fused_common_notex_at3F() && s_cfg.numtevstages == 0 &&
           s_cfg.bm_blend_enable == 1 &&
           s_cfg.bm_src_factor == 4 && s_cfg.bm_dst_factor == 5 &&
           fused_stage_match(0, 0x08FFFAu, 0x08FFD0u, 0, 0);
}
static int gpu_program_Z_match(void) {
    return fused_common_notex_at3F() && s_cfg.numtevstages == 0 &&
           s_cfg.bm_blend_enable == 0 &&
           fused_stage_match(0, 0x08FFFAu, 0x08FFD0u, 0, 0);
}

/* AA: same COLOR identity as Y/Z (cc=0x08FFFA) but ac=0x08FFF0 (aargA=aargB=
 * aargC=ZERO(7), aargD=ZERO(15)->0): every alpha arg is 0, so ALPHA==0
 * unconditionally (no operand dependence at all). Blend is sf=6(DstAlpha)
 * df=1(One) -- a REAL per-pixel read of the framebuffer's own current alpha
 * (not foldable to a hoisted scalar the way Y's sf=4/df=5 is), plus
 * da_enable=1/da_alpha=0 which unconditionally overwrites dstPtr[ALP_C]
 * after BlendColor regardless of what it computed there -- so the fold
 * skips computing a blended alpha at all and just stores da_alpha (0)
 * directly (an exact elision, not an approximation: da_enable's override in
 * BlendTev runs unconditionally whenever da_enable==1, independent of any
 * operand value). cu=1/au=1 both pinned exact (single observed config, no
 * pairing ambiguity like Y/Z). */
static int gpu_program_AA_match(void) {
    for (u32 stage = 0; stage <= s_cfg.numtevstages; stage++)
        if (s_cfg.stage[stage].tevind != 0) return 0;
    return s_cfg.numtexgens == 0 && s_cfg.numcolchans == 1 &&
           s_cfg.numtevstages == 0 &&
           s_cfg.zt_enable == 0 &&
           s_cfg.da_enable == 1 && s_cfg.da_alpha == 0 &&
           s_cfg.bm_blend_enable == 1 && s_cfg.bm_logic_enable == 0 &&
           s_cfg.bm_subtract == 0 && s_cfg.bm_dither == 1 &&
           s_cfg.bm_src_factor == 6 && s_cfg.bm_dst_factor == 1 &&
           s_bm_cu == 1 && s_bm_au == 1 && s_bp[0xF3] == 0x7F0000u &&
           s_cfg.swaptab[0][0] == 0 && s_cfg.swaptab[0][1] == 1 &&
           s_cfg.swaptab[0][2] == 2 && s_cfg.swaptab[0][3] == 3 &&
           fused_stage_match(0, 0x08FFFAu, 0x08FFF0u, 0, 0);
}

/* AB/AC/AD: GPU-only, zt_enable==1 (early LEQUAL/func3, zupd==0) -- software
 * deliberately stays on the general tev_draw() path as the differential
 * authority, same convention as K--X. All three pin cu==1/au==0 exactly
 * (each is a single observed census config, no pairing ambiguity). */

/* AB: cc=0x08EFFF (argA=StageKonst.rgb, argB=argC=ZERO, argD=ZERO, bias0 op0
 * clamp255 scale0) -> COLOR==clamp255(StageKonst.ch) (the lerp-to-B term is
 * 0 since argC==ZERO, leaving a pure passthrough of A through the bias/scale
 * chain); ac=0x08FFF0 -> ALPHA==0 (same all-zero shape as AA's alpha).
 * blend disabled (opaque write). */
static int gpu_program_AB_match(void) {
    return s_cfg.numtevstages == 0 && s_cfg.numtexgens == 0 &&
           s_cfg.numcolchans == 1 &&
           s_cfg.zt_enable == 1 && s_cfg.zt_early == 1 &&
           s_cfg.zt_func == CMP_LEQUAL && s_zt_upd == 0 &&
           s_cfg.da_enable == 0 &&
           s_cfg.bm_blend_enable == 0 && s_cfg.bm_logic_enable == 0 &&
           s_cfg.bm_subtract == 0 && s_cfg.bm_dither == 1 &&
           s_bm_cu == 1 && s_bm_au == 0 && s_bp[0xF3] == 0x7F0000u &&
           s_cfg.swaptab[0][0] == 0 && s_cfg.swaptab[0][1] == 1 &&
           s_cfg.swaptab[0][2] == 2 && s_cfg.swaptab[0][3] == 3 &&
           fused_stage_match(0, 0x08EFFFu, 0x08FFF0u, 0, 0);
}

/* AC: same cc=0x08EFFF COLOR==StageKonst.rgb as AB; ac=0x08BFF0 (aargA=
 * RasColor.a, aargB=aargC=ZERO, aargD=ZERO) -> ALPHA==RasColor.a exactly
 * (identity, same shape as Y/Z's alpha). blend=1 sf=4/df=5 -- the SAME
 * hoistable fold as the A--J/Y family (both factors derive from the shaded
 * pixel's own alpha). cu=1/au=0: the blended alpha is computed (blend math
 * needs it for sf/df) but never written to the EFB. */
static int gpu_program_AC_match(void) {
    return s_cfg.numtevstages == 0 && s_cfg.numtexgens == 0 &&
           s_cfg.numcolchans == 1 &&
           s_cfg.zt_enable == 1 && s_cfg.zt_early == 1 &&
           s_cfg.zt_func == CMP_LEQUAL && s_zt_upd == 0 &&
           s_cfg.da_enable == 0 &&
           s_cfg.bm_blend_enable == 1 && s_cfg.bm_logic_enable == 0 &&
           s_cfg.bm_subtract == 0 && s_cfg.bm_dither == 1 &&
           s_cfg.bm_src_factor == 4 && s_cfg.bm_dst_factor == 5 &&
           s_bm_cu == 1 && s_bm_au == 0 && s_bp[0xF3] == 0x7F0000u &&
           s_cfg.swaptab[0][0] == 0 && s_cfg.swaptab[0][1] == 1 &&
           s_cfg.swaptab[0][2] == 2 && s_cfg.swaptab[0][3] == 3 &&
           fused_stage_match(0, 0x08EFFFu, 0x08BFF0u, 0, 0);
}

/* AD: cc=0x082FFF (argA=Reg[1].rgb, argB=argC=ZERO, argD=ZERO, bias0 op0
 * clamp255 scale0) -> COLOR==clamp255(Reg[1].ch) (same passthrough shape as
 * AB's StageKonst color, for the Reg[1] operand instead); ac=0x08FAF0
 * (aargA=ZERO, aargB=StageKonst.a, aargC=RasColor.a, aargD=ZERO) -> a REAL
 * modulate: ALPHA==clamp255((StageKonst.a * cc(RasColor.a) + 128) >> 8),
 * the same "constant times RasColor.a's cc" shape as fused_pixel_A's alpha
 * fold. blend=1 sf=4/df=5 (the same hoistable fold). cu=1/au=0. */
static int gpu_program_AD_match(void) {
    return s_cfg.numtevstages == 0 && s_cfg.numtexgens == 0 &&
           s_cfg.numcolchans == 1 &&
           s_cfg.zt_enable == 1 && s_cfg.zt_early == 1 &&
           s_cfg.zt_func == CMP_LEQUAL && s_zt_upd == 0 &&
           s_cfg.da_enable == 0 &&
           s_cfg.bm_blend_enable == 1 && s_cfg.bm_logic_enable == 0 &&
           s_cfg.bm_subtract == 0 && s_cfg.bm_dither == 1 &&
           s_cfg.bm_src_factor == 4 && s_cfg.bm_dst_factor == 5 &&
           s_bm_cu == 1 && s_bm_au == 0 && s_bp[0xF3] == 0x7F0000u &&
           s_cfg.swaptab[0][0] == 0 && s_cfg.swaptab[0][1] == 1 &&
           s_cfg.swaptab[0][2] == 2 && s_cfg.swaptab[0][3] == 3 &&
           fused_stage_match(0, 0x082FFFu, 0x08FAF0u, 0, 0);
}

/* ---- config A: stages=1, texgens=1, st0 cc=0x00F8CF ac=0x00F670 ----------
 *
 * cc=0x00F8CF bit-field decode (bits() same as build_draw_cfg):
 *   argA=15(ZERO)=0  argB=8(TexColor.rgb)  argC=12(ONE)=255  argD=15(ZERO)=0
 *   bias(c16)=0  op(c18)=0(add)  clamp(c19)=0(->clamp1024)  scale(c20)=0  dest(c22)=0(Prev)
 * ac=0x00F670 bit-field decode:
 *   aargA=7(out of alpha_arg's 0..6 range -> 0/ZERO)  aargB=5(RasColor.a)
 *   aargC=4(TexColor.a)  aargD=7(-> 0/ZERO)
 *   bias(a16)=0  op(a18)=0(add)  clamp(a19)=0(->clamp1024)  scale(a20)=0  dest(a22)=0(Prev)
 *   tswap_id=0  rswap_id=0
 *
 * COLOR fold: draw_color_regular with A=0,B=X(=TexColor.ch),C=255,D=0,bias=0,
 * op=0,scale=0 is the textbook TEV "replace with texture" identity —
 * ((X*256 + 128) >> 8) == X exactly for every X in 0..255 (texel bytes are
 * always in this range) because the +128 remainder never reaches 256.
 * Verified for all 256 values, 0 mismatches -- there is no arithmetic left to
 * perform, Reg[Prev].ch IS TexColor.ch bit-for-bit.
 *
 * ALPHA fold: A=0,B=Y(=RasColor.a),C=X(=TexColor.a),D=0,bias=0,op=0,scale=0:
 *   cc = X + (X>>7);  temp = (Y*cc + 128) >> 8;  Reg[Prev].a = clamp1024(temp)
 * (clamp1024 instead of C's clamp255 since a19==0 here — but the natural
 * range of Y*cc>>8-ish is already 0..255, so the two clamps are
 * indistinguishable on the actual output byte; kept as clamp1024 to match the
 * general path's literal choice, not because it changes the result).
 * Verified bit-exact over the full Y,X in [0,255]x[0,255] (65536 cases). */
static void fused_pixel_A(Tev* t) {
    tev_stats_enter(1);
    u64 tex_before, comb_t0 = tev_comb_begin(&tex_before);

    u32 texmap = s_cfg.stage[0].texmap;
    /* texcoordSel is provably 0 whenever numtexgens==1 (build_draw_cfg clamps
     * sc->texcoordSel to 0 if it's >= numtexgens, and numtexgens==1 is part
     * of this config's signature), so UvS[0]/UvT[0] is the only possible
     * index — no need to read sc->texcoordSel at all. */
    u8 texel[4];
    tev_sample_stat(t->wid, texmap, t->UvS[0], t->UvT[0],
                    t->TextureLod[0], t->TextureLinear[0], texel);

    /* colorchan==0, rswap_id==0 with swaptab[0]==identity (both checked at
     * selection time) -> RasColor.a == t->Color[0][3] directly. */
    u8 ras_a = t->Color[0][3];

    u8 output[4];
    output[RED_C] = texel[0];   /* tswap_id==0 identity -> TexColor.ch == texel[ch] */
    output[GRN_C] = texel[1];
    output[BLU_C] = texel[2];
    {
        s32 X = texel[3];
        s32 cc = X + (X >> 7);
        s32 temp = (s32)ras_a * cc;
        temp += 128;
        temp >>= 8;
        output[ALP_C] = (u8)clamp1024((s16)temp);
    }

    /* alpha test: at word 0x7F0000 decodes to comp0==comp1==CMP_ALWAYS (7)
     * with op==OR; alpha_cmp(CMP_ALWAYS) returns 1 UNCONDITIONALLY (it never
     * reads alpha or ref), so c0||c1 is always true regardless of the shaded
     * alpha value — proven structurally from the word's bit fields, not
     * sampled/assumed. No alpha_test() call needed. */
    tev_comb_finish(t, output, comb_t0, tex_before, fused_blend_stage);
}

/* Config H: config A followed by B's already-derived stage-1 alpha fold. */
static void fused_pixel_H(Tev* t) {
    tev_stats_enter(1);
    u64 tex_before, comb_t0 = tev_comb_begin(&tex_before);
    u8 texel[4], output[4];
    tev_sample_stat(t->wid, s_cfg.stage[0].texmap, t->UvS[0], t->UvT[0],
                    t->TextureLod[0], t->TextureLinear[0], texel);
    output[RED_C] = texel[0];
    output[GRN_C] = texel[1];
    output[BLU_C] = texel[2];
    {
        s32 x = texel[3];
        s32 cc = x + (x >> 7);
        s32 temp = (s32)t->Color[0][3] * cc;
        temp += 128;
        temp >>= 8;
        output[ALP_C] = (u8)clamp1024((s16)temp);
    }
    {
        s32 p = output[ALP_C];
        s32 cc = p + (p >> 7);
        s32 temp = s_cfg.stage[1].stage_konst.a * cc;
        temp += 128;
        temp >>= 8;
        output[ALP_C] = (u8)clamp255((s16)temp);
    }
    tev_comb_finish(t, output, comb_t0, tex_before, fused_blend_stage);
}

/* Config I: texture-replace RGB, RasAlpha/KonstAlpha stage 0, then the
 * standard disabled-texture stage-1 alpha fold. Intermediate alpha stays
 * signed-11-bit exactly like Reg[Prev], rather than being narrowed to u8. */
static void fused_pixel_I(Tev* t) {
    tev_stats_enter(1);
    u64 tex_before, comb_t0 = tev_comb_begin(&tex_before);
    u8 texel[4], output[4];
    tev_sample_stat(t->wid, s_cfg.stage[0].texmap, t->UvS[0], t->UvT[0],
                    t->TextureLod[0], t->TextureLinear[0], texel);
    output[RED_C] = texel[0];
    output[GRN_C] = texel[1];
    output[BLU_C] = texel[2];
    s32 k0 = s_cfg.stage[0].stage_konst.a;
    u32 cc0 = (u32)k0 + ((u32)k0 >> 7);
    s32 temp0 = (s32)t->Color[0][3] * (s32)cc0;
    temp0 += 128;
    temp0 >>= 8;
    s32 p = clamp1024((s16)temp0);
    u32 cc1 = (u32)p + ((u32)p >> 7);
    s32 temp1 = s_cfg.stage[1].stage_konst.a * (s32)cc1;
    temp1 += 128;
    temp1 >>= 8;
    output[ALP_C] = (u8)clamp255((s16)temp1);
    tev_comb_finish(t, output, comb_t0, tex_before, fused_blend_stage);
}

/* Config J: no-texture RasColor passthrough followed by stage-1 alpha. */
static void fused_pixel_J(Tev* t) {
    tev_stats_enter(1);
    u64 tex_before, comb_t0 = tev_comb_begin(&tex_before);
    u8 output[4];
    output[RED_C] = t->Color[0][0];
    output[GRN_C] = t->Color[0][1];
    output[BLU_C] = t->Color[0][2];
    s32 p = t->Color[0][3];
    s32 cc = p + (p >> 7);
    s32 temp = s_cfg.stage[1].stage_konst.a * cc;
    temp += 128;
    temp >>= 8;
    output[ALP_C] = (u8)clamp255((s16)temp);
    tev_comb_finish(t, output, comb_t0, tex_before, fused_blend_stage);
}

/* ---- shared core for config B's stage0 and config C's only stage --------
 * cc=0x18F28F ac=0x08F670 bit-field decode:
 *   argA=15(ZERO)=0  argB=2(Reg[1].rgb, "Color0" TEV const-register)
 *   argC=8(TexColor.rgb)  argD=15(ZERO)=0
 *   bias(c16)=0  op(c18)=0(add)  clamp(c19)=1(->clamp255)  scale(c20)=1  dest(c22)=0(Prev)
 *   aargA=7(->0/ZERO)  aargB=5(RasColor.a)  aargC=4(TexColor.a)  aargD=7(->0/ZERO)
 *   bias(a16)=0  op(a18)=0(add)  clamp(a19)=1(->clamp255)  scale(a20)=0  dest(a22)=0(Prev)
 *   tswap_id=0  rswap_id=0
 *
 * COLOR fold: draw_color_regular with A=0,B=K(=Reg[1].ch),C=X(=TexColor.ch),
 * D=0,bias=0,op=0,scale=1,clamp=255. K is a TEV constant register loaded by
 * tev_load_registers as a SIGNED 11-bit field (sext(...,11) => -1024..1023,
 * NOT just 0..255) — read live every draw, never assumed:
 *   c = X + (X>>7)
 *   temp = (K * c) << 1              // s_LShift[scale=1] == 1
 *   temp += 128                      // scale!=3, op==0
 *   temp >>= 8
 *   Reg[Prev].ch = clamp255(temp)    // (0+0)<<1 + temp, >>s_RShift[1](==0), unchanged
 * Verified bit-exact over K in the FULL signed range [-1024,1023] x X in
 * [0,255] (524288 cases, 0 mismatches).
 *
 * ALPHA fold: same shape as config A's alpha (A=0,B=Y=RasColor.a,
 * C=X=TexColor.a,D=0,bias=0,op=0,scale=0), but clamp(a19)==1 here -> clamp255
 * (config A's a19==0 -> clamp1024; the two are byte-identical on this input
 * range regardless, see config A's note). Verified bit-exact over the full
 * Y,X in [0,255]x[0,255] (65536 cases). */
static inline void fused_core_C(Tev* t, u8 output[4]) {
    u32 texmap = s_cfg.stage[0].texmap;
    u8 texel[4];
    tev_sample_stat(t->wid, texmap, t->UvS[0], t->UvT[0],
                    t->TextureLod[0], t->TextureLinear[0], texel);

    u8 ras_a = t->Color[0][3];         /* colorchan==0, rswap_id==0 identity */
    const TColor* reg1 = &t->Reg[1];   /* "Color0" TEV const register, per-draw */

    s32 c;
    c = texel[0] + (texel[0] >> 7);
    { s32 temp = ((s32)reg1->r * c) << 1; temp += 128; temp >>= 8;
      output[RED_C] = (u8)clamp255((s16)temp); }
    c = texel[1] + (texel[1] >> 7);
    { s32 temp = ((s32)reg1->g * c) << 1; temp += 128; temp >>= 8;
      output[GRN_C] = (u8)clamp255((s16)temp); }
    c = texel[2] + (texel[2] >> 7);
    { s32 temp = ((s32)reg1->b * c) << 1; temp += 128; temp >>= 8;
      output[BLU_C] = (u8)clamp255((s16)temp); }

    {
        s32 X = texel[3];
        s32 cc = X + (X >> 7);
        s32 temp = (s32)ras_a * cc;
        temp += 128;
        temp >>= 8;
        output[ALP_C] = (u8)clamp255((s16)temp);
    }
}

/* ---- config C: stages=1, texgens=1, st0 cc=0x18F28F ac=0x08F670 ---------- */
static void fused_pixel_C(Tev* t) {
    tev_stats_enter(1);
    u64 tex_before, comb_t0 = tev_comb_begin(&tex_before);

    u8 output[4];
    fused_core_C(t, output);

    /* alpha test always passes -- see fused_pixel_A's identical derivation
     * (same 0x7F0000 word, checked as part of fused_common_match). */
    tev_comb_finish(t, output, comb_t0, tex_before, fused_blend_stage);
}

/* ---- config B: stages=2, texgens=1 ----------------------------------------
 * st0 cc=0x18F28F ac=0x08F670 en=1 -- identical to config C's only stage,
 *   see fused_core_C's derivation.
 * st1 cc=0x08FC0F ac=0x08F870 en=0 -- enable==0 means stage1 NEVER samples a
 *   texture (Tev::Draw only enters its tex_sample block `if (enable)`), so
 *   TexColor/RawTexColor simply keep stage0's stale value for the rest of the
 *   stage loop; RasColor is still recomputed every stage regardless of
 *   enable (SetRasColor has no enable gate) but stage1's args below never
 *   reference it, so that recomputation is dead work the fused path can just
 *   not do (it has zero effect on the final output either way).
 *
 * st1 cc=0x08FC0F bit-field decode:
 *   argA=15(ZERO)=0  argB=12(ONE)=255  argC=0(Reg[Prev].rgb == stage0's output)
 *   argD=15(ZERO)=0
 *   bias(c16)=0  op(c18)=0  clamp(c19)=1(->255)  scale(c20)=0  dest(c22)=0(Prev)
 * COLOR fold: A=0,B=255,C=P(=stage0 Prev.ch, already 0..255 from stage0's own
 * clamp255),D=0,bias=0,op=0,scale=0 -- the SAME "replace" identity as config
 * A's color fold: ((P*256+128)>>8) == P exactly for every P in 0..255.
 * Verified separately for this exact clamp mode too (256 cases, 0
 * mismatches) -- stage1 leaves output[RED_C]/[GRN_C]/[BLU_C] UNCHANGED, so
 * there is nothing left to compute; fused_pixel_B below does not touch them
 * after fused_core_C.
 *
 * st1 ac=0x08F870 bit-field decode:
 *   aargA=7(->0/ZERO)  aargB=6(StageKonst.a -- this stage's own per-draw
 *   konst alpha, resolved once per draw by build_draw_cfg into
 *   s_cfg.stage[1].stage_konst -- read live below, its numeric VALUE is never
 *   assumed or compared, only the SELECTOR case aargB==6 matters for the
 *   signature, which the exact ac-word match already pins)
 *   aargC=0(Reg[Prev].a == stage0's alpha, i.e. output[ALP_C] just computed)
 *   aargD=7(->0/ZERO)
 *   bias(a16)=0  op(a18)=0  clamp(a19)=1(->255)  scale(a20)=0  dest(a22)=0(Prev)
 * ALPHA fold: A=0,B=K2(=StageKonst.a),C=P(=stage0 alpha, 0..255),D=0,bias=0,
 * op=0,scale=0:
 *   cc = P + (P>>7);  temp = (K2*cc + 128) >> 8;  Reg[Prev].a = clamp255(temp)
 * K2 comes from konst_lookup's kasel path, which for sel>=16 returns a raw
 * Konst[] component directly -- the SAME signed 11-bit range as the color
 * register fold above, NOT just the fixed positive LUT. Verified bit-exact
 * over K2 in the full signed range [-1024,1023] x P in [0,255] (524288
 * cases, 0 mismatches). */
static void fused_pixel_B(Tev* t) {
    tev_stats_enter(1);
    u64 tex_before, comb_t0 = tev_comb_begin(&tex_before);

    u8 output[4];
    fused_core_C(t, output);   /* stage0 -- identical to config C */

    /* stage1 color: proven exact no-op (see derivation above), nothing to do. */

    /* stage1 alpha: modulate stage0's alpha by this stage's own konst alpha. */
    {
        s32 konst_a = s_cfg.stage[1].stage_konst.a;
        s32 P = output[ALP_C];
        s32 cc = P + (P >> 7);
        s32 temp = konst_a * cc;
        temp += 128;
        temp >>= 8;
        output[ALP_C] = (u8)clamp255((s16)temp);
    }

    /* alpha test always passes -- see fused_pixel_A's identical derivation. */
    tev_comb_finish(t, output, comb_t0, tex_before, fused_blend_stage);
}

/* ---- config D: stages=1, texgens=1, st0 cc=0x08FCAF ac=0x08F2F0 ----------
 * (D1 boot-animation census, see the file-header "D1 EXTENSION" note above)
 *
 * cc=0x08FCAF bit-field decode (bits() same as build_draw_cfg):
 *   argA=15(ZERO)=0  argB=12(ONE)=255  argC=10(RasColor.rgb)  argD=15(ZERO)=0
 *   bias(c16)=0  op(c18)=0(add)  clamp(c19)=1(->clamp255)  scale(c20)=0  dest(c22)=0(Prev)
 * ac=0x08F2F0 bit-field decode:
 *   aargA=7(->0/ZERO)  aargB=4(TexColor.a)  aargC=5(RasColor.a)  aargD=7(->0/ZERO)
 *   bias(a16)=0  op(a18)=0(add)  clamp(a19)=1(->clamp255)  scale(a20)=0  dest(a22)=0(Prev)
 *   tswap_id=0  rswap_id=0
 *
 * COLOR fold: draw_color_regular with A=0,B=255(ONE),C=Y(=RasColor.ch),D=0,
 * bias=0,op=0,scale=0 -- the mirror image of config A's color fold (there B
 * carried the interpolated value and C was the always-255 factor; here B is
 * the always-255 endpoint and C -- RasColor.ch -- drives the interpolation
 * factor): cc=Y+(Y>>7); ((255*cc+128)>>8) clamped to 0..255 reproduces Y
 * exactly for every Y in 0..255, the same "compress the factor, decompress
 * against a full-scale endpoint" round-trip config A's/B's stage1's replace
 * folds use. Verified for all 256 values, 0 mismatches -- Reg[Prev].ch IS
 * RasColor.ch bit-for-bit; the texture is sampled only for its alpha channel
 * (aargB below) and never touches color at all.
 *
 * ALPHA fold: A=0,B=X(=TexColor.a),C=Y(=RasColor.a),D=0,bias=0,op=0,scale=0:
 *   cc = Y + (Y>>7);  temp = (X*cc + 128) >> 8;  Reg[Prev].a = clamp255(temp)
 * Same shape as config A/C's alpha fold but with B/C's roles swapped --
 * TexColor.a supplies the interpolated VALUE here, RasColor.a supplies the
 * FACTOR (config A/C have it the other way around). The two are NOT
 * interchangeable formulas in general (the v+(v>>7) factor approximation
 * doesn't commute with which operand it's built from), so this was verified
 * as its own derivation, not reused from config A/C's alpha fold. Verified
 * bit-exact over the full X,Y in [0,255]x[0,255] (65536 cases, 0
 * mismatches). Neither operand is a TEV Reg[]/Konst[] value (no signed
 * 11-bit range involved in this stage's cc/ac at all), so 0..255 is already
 * the full domain -- see the standalone checker referenced in the file-header
 * "D1 EXTENSION" note. */
static void fused_pixel_D(Tev* t) {
    tev_stats_enter(1);
    u64 tex_before, comb_t0 = tev_comb_begin(&tex_before);

    u32 texmap = s_cfg.stage[0].texmap;
    /* texcoordSel is provably 0 whenever numtexgens==1 (build_draw_cfg clamps
     * sc->texcoordSel to 0 if it's >= numtexgens, and numtexgens==1 is part
     * of this config's signature) -- same reasoning as fused_pixel_A. */
    u8 texel[4];
    tev_sample_stat(t->wid, texmap, t->UvS[0], t->UvT[0],
                    t->TextureLod[0], t->TextureLinear[0], texel);

    /* colorchan==0, rswap_id==0 with swaptab[0]==identity (both checked at
     * selection time) -> RasColor.{r,g,b,a} == t->Color[0][0..3] directly. */
    u8 output[4];
    { s32 Y = t->Color[0][0]; s32 cc = Y + (Y >> 7); s32 temp = 255 * cc; temp += 128; temp >>= 8;
      output[RED_C] = (u8)clamp255((s16)temp); }
    { s32 Y = t->Color[0][1]; s32 cc = Y + (Y >> 7); s32 temp = 255 * cc; temp += 128; temp >>= 8;
      output[GRN_C] = (u8)clamp255((s16)temp); }
    { s32 Y = t->Color[0][2]; s32 cc = Y + (Y >> 7); s32 temp = 255 * cc; temp += 128; temp >>= 8;
      output[BLU_C] = (u8)clamp255((s16)temp); }
    {
        s32 X = texel[3];             /* tswap_id==0 identity -> TexColor.a == texel[3] */
        s32 Y = t->Color[0][3];
        s32 cc = Y + (Y >> 7);
        s32 temp = X * cc;
        temp += 128;
        temp >>= 8;
        output[ALP_C] = (u8)clamp255((s16)temp);
    }

    /* alpha test: same 0x7F0000 word as A/B/C (part of fused_common_match) ->
     * comp0==comp1==CMP_ALWAYS, always passes. No alpha_test() call needed. */
    tev_comb_finish(t, output, comb_t0, tex_before, fused_blend_stage);
}

/* ---- config E: stages=1, texgens=1, st0 cc=0x18428F ac=0x08F770 ----------
 * (D1 EXTENSION cont'd, fresh GCN_GX_TEV_CENSUS=1 24,000,000-block boot-
 * animation census, 2026-07-13 residuals sweep: this is the OLD "40bead5f"
 * config from the D1 census note above — 3.6%..4.1% of pixels and rising
 * across that window, same growth shape config D showed before its own
 * fuse. NOTE: an earlier pass mis-assumed this config had bm_blend_enable==0
 * ("write-through, no blend"); the actual census dump shows blend=1 sf=4
 * df=5 da=0/0 dither=1 at=0x7F0000 — i.e. it already satisfies
 * fused_common_match()'s existing blend preconditions bit-for-bit. No new
 * blend variant needed; fused_blend_stage is reused as-is.
 *
 * cc=0x18428F bit-field decode:
 *   argA=4(Reg[2].rgb, "Color1" TEV const register)  argB=2(Reg[1].rgb,
 *   "Color0")  argC=8(TexColor.rgb)  argD=15(ZERO)=0
 *   bias(c16)=0  op(c18)=0(add)  clamp(c19)=1(->clamp255)  scale(c20)=1  dest(c22)=0(Prev)
 * ac=0x08F770 bit-field decode:
 *   aargA=7(->0/ZERO)  aargB=5(RasColor.a)  aargC=6(StageKonst.a)  aargD=7(->0/ZERO)
 *   bias(a16)=0  op(a18)=0(add)  clamp(a19)=1(->clamp255)  scale(a20)=0  dest(a22)=0(Prev)
 *   tswap_id=0  rswap_id=0
 *
 * COLOR fold: draw_color_regular with A=Reg[2].ch, B=Reg[1].ch (both SIGNED
 * 11-bit TEV registers, -1024..1023, read live every draw — same discipline
 * as fused_core_C's K), C=X(=TexColor.ch, u8 0..255, the interpolation
 * factor), D=0, bias=0, op=0, scale=1, clamp255. Unlike every prior fold,
 * BOTH combiner operands are per-draw registers (not one register + one
 * always-255/always-0 endpoint), so this is a genuine two-point lerp, not a
 * degenerate replace/modulate identity — but the general formula itself is
 * still exact integer algebra and distributes cleanly:
 *   c = X + (X>>7)
 *   A*(256-c) + B*c  ==  A*256 + c*(B-A)          (exact, no rounding: pure
 *                                                   integer distributivity)
 *   temp = (A*256 + c*(B-A)) << 1; temp += 128; temp >>= 8
 *   Reg[Prev].ch = clamp255(temp)
 * The rewritten form trades the general path's 2 multiplies for 1 subtract
 * (A,B are per-draw-constant, but computed inline below rather than cached in
 * s_cfg — same "read live, no extra per-draw state" style as fused_core_C's
 * Reg[1] read) + 1 multiply, so it's still a strict reduction in per-pixel
 * work. Overflow check: A*256 in [-262144,261888], c in [0,256], (B-A) in
 * [-2047,2047], so c*(B-A) in [-524032,523776] — sum stays deep inside s32,
 * matching the general path's own s32 `temp`. Verified bit-exact (not just
 * "provably exact by algebra" — brute forced too, standalone checker, ALL of
 * A,B in the full signed range [-1024,1023] (2048x2048) x ALL 256 texel
 * values: 1,073,741,824 cases, 0 mismatches.
 *
 * ALPHA fold: A=0,B=Y(=RasColor.a, u8 0..255),C=K(=StageKonst.a — a SIGNED
 * 11-bit per-stage konst, same live-read discipline as fused_pixel_B's aargB
 * case, but used here as the INTERPOLATION FACTOR rather than a value
 * operand — a role no prior fused config exercised, so verified as its own
 * derivation, not reused from any other fold):
 *   cc = K + ((u32)K >> 7)   -- K reinterpreted as u32 before the shift,
 *                               exactly mirroring the general path's own
 *                               `(u32)inC[i] >> 7` cast (this is NOT the same
 *                               as an arithmetic shift on negative K; the
 *                               fold transcribes the cast verbatim rather
 *                               than reasoning about what it "means")
 *   temp = (Y*(s32)cc + 128) >> 8;  Reg[Prev].a = clamp255(temp)
 * (A=0 term drops out, so this is a direct 1-multiply transcription, no
 * algebraic rewrite needed.) Verified bit-exact over the full domain: Y in
 * [0,255] x K in the full signed range [-1024,1023] (524,288 cases, 0
 * mismatches). */
static inline void fused_core_E(Tev* t, u8 output[4]) {
    u32 texmap = s_cfg.stage[0].texmap;
    u8 texel[4];
    tev_sample_stat(t->wid, texmap, t->UvS[0], t->UvT[0],
                    t->TextureLod[0], t->TextureLinear[0], texel);

    u8 ras_a = t->Color[0][3];         /* colorchan==0, rswap_id==0 identity */
    const TColor* reg2 = &t->Reg[2];   /* "Color1" TEV const register, per-draw */
    const TColor* reg1 = &t->Reg[1];   /* "Color0" TEV const register, per-draw */
    s32 konst_a = s_cfg.stage[0].stage_konst.a;

    {
        s32 c = texel[0] + (texel[0] >> 7);
        s32 temp = ((s32)reg2->r << 8) + c * ((s32)reg1->r - (s32)reg2->r);
        temp = (temp << 1) + 128; temp >>= 8;
        output[RED_C] = (u8)clamp255((s16)temp);
    }
    {
        s32 c = texel[1] + (texel[1] >> 7);
        s32 temp = ((s32)reg2->g << 8) + c * ((s32)reg1->g - (s32)reg2->g);
        temp = (temp << 1) + 128; temp >>= 8;
        output[GRN_C] = (u8)clamp255((s16)temp);
    }
    {
        s32 c = texel[2] + (texel[2] >> 7);
        s32 temp = ((s32)reg2->b << 8) + c * ((s32)reg1->b - (s32)reg2->b);
        temp = (temp << 1) + 128; temp >>= 8;
        output[BLU_C] = (u8)clamp255((s16)temp);
    }
    {
        u32 cc = (u32)konst_a + ((u32)konst_a >> 7);
        s32 temp = (s32)ras_a * (s32)cc;
        temp += 128;
        temp >>= 8;
        output[ALP_C] = (u8)clamp255((s16)temp);
    }

}

static void fused_pixel_E(Tev* t) {
    tev_stats_enter(1);
    u64 tex_before, comb_t0 = tev_comb_begin(&tex_before);
    u8 output[4];
    fused_core_E(t, output);
    tev_comb_finish(t, output, comb_t0, tex_before, fused_blend_stage);
}

/* ---- config G: config E followed by B's exact stage-1 alpha fold ---------
 * st0 is byte-for-byte config E. st1 is the same disabled-texture
 * 08FC0F/08F870 stage already derived for config B: RGB is an identity and
 * alpha modulates the stage-0 result by stage_konst[1].a. */
static void fused_pixel_G(Tev* t) {
    tev_stats_enter(1);
    u64 tex_before, comb_t0 = tev_comb_begin(&tex_before);
    u8 output[4];
    fused_core_E(t, output);
    {
        s32 konst_a = s_cfg.stage[1].stage_konst.a;
        s32 p = output[ALP_C];
        s32 cc = p + (p >> 7);
        s32 temp = konst_a * cc;
        temp += 128;
        temp >>= 8;
        output[ALP_C] = (u8)clamp255((s16)temp);
    }
    tev_comb_finish(t, output, comb_t0, tex_before, fused_blend_stage);
}

/* ---- config F: stages=1, texgens=0, st0 cc=0x00AFFF ac=0x00BFF0 ----------
 * (D1 EXTENSION cont'd, same 24,000,000-block boot-animation census as
 * config E above: the old "8b773e26" config, ~1.8% of pixels and roughly
 * flat across the window. texgens=0 -- this draw has NO texture unit
 * active at all, so it needs fused_common_match_notex() rather than the
 * numtexgens==1 variant every other config uses.
 *
 * cc=0x00AFFF bit-field decode:
 *   argA=10(RasColor.rgb)  argB=15(out of range -> 0/ZERO)  argC=15(->0/ZERO)
 *   argD=15(->0/ZERO)
 *   bias(c16)=0  op(c18)=0(add)  clamp(c19)=0(->clamp1024)  scale(c20)=0  dest(c22)=0(Prev)
 * ac=0x00BFF0 bit-field decode:
 *   aargA=5(RasColor.a)  aargB=7(->0/ZERO)  aargC=7(->0/ZERO)  aargD=7(->0/ZERO)
 *   bias(a16)=0  op(a18)=0(add)  clamp(a19)=0(->clamp1024)  scale(a20)=0  dest(a22)=0(Prev)
 *   tswap_id=0  rswap_id=0
 *
 * Stage `enable`==1 in the census dump, but numtexgens==0 means tev_draw's
 * OWN `if (numtexgens > 0) tex_sample_stat(...) else memset(texel,0,4)`
 * never takes the sampling branch for this draw -- texel is always zero, and
 * neither cc nor ac's args reference TexColor/RawTexColor at all (argB/C/D
 * and aargB/C/D are all the ZERO case), so this fold correctly never touches
 * a texture at all, matching the general path exactly.
 *
 * COLOR fold: draw_color_regular with A=Y(=RasColor.ch),B=0,C=0,D=0,bias=0,
 * op=0,scale=0,clamp1024 -- C=0 makes the interpolation factor `c` itself
 * zero (0+(0>>7)=0), so temp = A*(256-0)+B*0 = A*256 exactly, and the
 * trailing `(+128)>>8` is the same "compress by 256, add half, truncate"
 * round-trip fused_pixel_A's/D's replace folds use: (A*256+128)>>8 == A for
 * every A in 0..255, and clamp1024(A) == A since A is already u8-range.
 * Reg[Prev].ch IS RasColor.ch bit-for-bit -- pure passthrough, the simplest
 * fold in the file (no texture, no TEV register, no combiner math at all).
 * Verified bit-exact for all 256 values of RasColor.ch, AND (extra margin,
 * matching the file's "full possible range" discipline even though
 * RasColor.ch is provably u8) over the full signed range A in [-1024,1023]
 * against ref_color's clamp1024 branch: 2048 cases, 0 mismatches either way.
 *
 * ALPHA fold: A=Y(=RasColor.a),B=0,C=0,D=0,bias=0,op=0,scale=0,clamp1024 --
 * identical shape to the color fold above (same (A*256+128)>>8==A identity,
 * this time via clamp1024). Reg[Prev].a IS RasColor.a bit-for-bit. Verified
 * for all 256 values, 0 mismatches. */
static void fused_pixel_F(Tev* t) {
    tev_stats_enter(1);
    u64 tex_before, comb_t0 = tev_comb_begin(&tex_before);

    /* colorchan==0, rswap_id==0 with swaptab[0]==identity (both checked at
     * selection time) -> RasColor.{r,g,b,a} == t->Color[0][0..3] directly.
     * No texture unit is sampled at all (texgens==0, see derivation). */
    u8 output[4];
    output[RED_C] = t->Color[0][0];
    output[GRN_C] = t->Color[0][1];
    output[BLU_C] = t->Color[0][2];
    output[ALP_C] = t->Color[0][3];

    /* alpha test always passes -- same 0x7F0000 word as every other fused
     * config, checked as part of fused_blend_common_match. */
    tev_comb_finish(t, output, comb_t0, tex_before, fused_blend_stage);
}

/* ============================================================================
 * Output vertex + rasterizer (Rasterizer.cpp).
 * ==========================================================================*/
typedef struct {
    float mvPosition[3];
    float projectedPosition[4];
    float screenPosition[3];
    float normal[3][3];
    u8    color[2][4];      /* [chan][R,G,B,A] */
    float texCoords[8][3];
    /* Diagnostic provenance (coverage-anomaly census): the untransformed
     * object-space position and the geometry-matrix slot that transformed
     * it. Clip-lerped vertices inherit vertex a's values (approximate —
     * fine for naming a draw's source data, meaningless for math). */
    float objPos[3];
    u8    posMtx;
} OutVtx;

typedef GxRasterSlope Slope;

/* One Tev per GX-MT worker. s_tev_w[0] is the ONLY instance the serial path
 * ever touches (it is the old single s_tev, renamed); workers 1..n-1 receive
 * a copy of it at each fork (Reg/Konst are the per-draw template loaded by
 * tev_load_registers — see the parallel_ok analysis in build_draw_cfg for why
 * a template copy is exact). Tev's own aligned(64) keeps workers off each
 * other's cache lines. */
static Tev    s_tev_w[GX_MT_MAX];

/* Per-frame coverage anomaly state (see the detector in the triangle setup
 * path and gx_raster_frame_anomaly_mark below). Written on the decode
 * thread only (triangle setup + the drawdone mark are both stream-ordered). */
typedef struct {
    u32 area, prog, dl;
    s32 minx, miny, maxx, maxy;
    float mv[3][3], w[3], sx[3], sy[3];
    float op[3][3];
    u8 pidx[3];
} FaTopDraw;
static u64      s_fa_frame_area;
static u32      s_fa_frame_tris;
static FaTopDraw s_fa_top[8];
static u64      s_fa_hist[32];      /* rolling window of per-frame area sums */
static u32      s_fa_hist_n;
static u64      s_fa_anomalies;
/* Per-TRIANGLE setup outputs, written by the main thread before any fork and
 * read-only for the whole scan (workers included) — shared by design. */
static Slope  s_ZSlope, s_WSlope, s_ColorSlopes[2][4], s_TexSlopes[8][3];

static float slope_value(const Slope* s, s32 x, s32 y) {
    float dx = s->xOff + (float)(x - s->x0);
    float dy = s->yOff + (float)(y - s->y0);
    return s->f0 + s->dfdx * dx + s->dfdy * dy;
}

typedef struct { float dx10, dx20, dy10, dy20; s32 x0, y0; float xOff, yOff; } SlopeCtx;

static SlopeCtx make_ctx(const OutVtx* v0, const OutVtx* v1, const OutVtx* v2,
                         s32 x0, s32 y0, s32 x_off, s32 y_off) {
    SlopeCtx c;
    const float adjust = 0.495f;
    c.xOff = ((float)x0 - (v0->screenPosition[0] - x_off)) + adjust;
    c.yOff = ((float)y0 - (v0->screenPosition[1] - y_off)) + adjust;
    c.dx10 = v1->screenPosition[0] - v0->screenPosition[0];
    c.dx20 = v2->screenPosition[0] - v0->screenPosition[0];
    c.dy10 = v1->screenPosition[1] - v0->screenPosition[1];
    c.dy20 = v2->screenPosition[1] - v0->screenPosition[1];
    c.x0 = x0; c.y0 = y0;
    return c;
}
static Slope make_slope(float f0, float f1, float f2, const SlopeCtx* ctx) {
    Slope s;
    float d20 = f2 - f0, d10 = f1 - f0;
    float a = d20 * ctx->dy10 - d10 * ctx->dy20;
    float b = ctx->dx20 * d10 - ctx->dx10 * d20;
    float c = ctx->dx20 * ctx->dy10 - ctx->dx10 * ctx->dy20;
    s.dfdx = a / c; s.dfdy = b / c;
    s.f0 = f0; s.x0 = ctx->x0; s.y0 = ctx->y0; s.xOff = ctx->xOff; s.yOff = ctx->yOff;
    return s;
}
static int iround(float x) { int t = (int)x; if ((x - t) >= 0.5f) return t + 1; return t; }

/* scissor rect + offset (from ComputeScissorRects Best()). */
static int s_scissor_left, s_scissor_top, s_scissor_right, s_scissor_bottom;
static int s_scissor_xoff, s_scissor_yoff;

#define BLK 2
typedef struct { float InvW; float Uv[8][2]; } RBPixel;
/* One 2x2 block buffer per GX-MT worker (indexed by Tev.wid, like the texel
 * cache); aligned so adjacent workers' buffers never share a cache line.
 * Serial mode only ever touches [0] — the old single s_rb. */
typedef struct { RBPixel px[BLK][BLK]; } __attribute__((aligned(64))) RBBlock;
static RBBlock s_rb_w[GX_MT_MAX];

static void calc_lod(Tev* t, s32* lodp, int* linear, u32 texmap, u32 texcoord) {
    /* tx_mode0/mode1 fields are per-draw-constant; s_cfg.tex[texmap] carries
     * them (build_draw_cfg), so this no longer re-reads BP per block. */
    const TexUnitCfg* tc = &s_cfg.tex[texmap];
    RBPixel (*rb)[BLK] = s_rb_w[t->wid].px;
    float* uv00 = rb[0][0].Uv[texcoord];
    float* uv10 = rb[1][0].Uv[texcoord];
    float* uv01 = rb[0][1].Uv[texcoord];
    float dudx = fabsf(uv00[0] - uv10[0]);
    float dvdx = fabsf(uv00[1] - uv10[1]);
    float dudy = fabsf(uv00[0] - uv01[0]);
    float dvdy = fabsf(uv00[1] - uv01[1]);
    float sDelta, tDelta;
    if (tc->lod_edge) { sDelta = dudx + dudy; tDelta = dvdx + dvdy; }
    else { sDelta = dudx > dudy ? dudx : dudy; tDelta = dvdx > dvdy ? dvdx : dvdy; }
    s32 lod = fixed_log2(sDelta > tDelta ? sDelta : tDelta);
    lod += tc->lod_bias_half;
    *linear = ((lod > 0 && tc->minf == 1) || (lod <= 0 && tc->magf == 1));
    s32 maxlod = (s32)tc->maxlod, minlod = (s32)tc->minlod;
    if (lod > maxlod) lod = maxlod; else if (lod < minlod) lod = minlod;
    *lodp = lod;
    (void)t;
}

static void build_block(Tev* t, s32 bx, s32 by) {
    u32 numtexgens = s_cfg.numtexgens;
    RBPixel (*rb)[BLK] = s_rb_w[t->wid].px;
    for (s32 yi = 0; yi < BLK; yi++) {
        for (s32 xi = 0; xi < BLK; xi++) {
            RBPixel* p = &rb[xi][yi];
            s32 x = xi + bx, y = yi + by;
            float invW = 1.0f / slope_value(&s_WSlope, x, y);
            p->InvW = invW;
            for (u32 i = 0; i < numtexgens; i++) {
                float projection = invW;
                float q = slope_value(&s_TexSlopes[i][2], x, y) * invW;
                if (q != 0.0f) projection = invW / q;
                p->Uv[i][0] = slope_value(&s_TexSlopes[i][0], x, y) * projection;
                p->Uv[i][1] = slope_value(&s_TexSlopes[i][1], x, y) * projection;
            }
        }
    }
    {
        u32 ind_count = gm_numindstages();
        if (ind_count > 4u) ind_count = 4u;
        u32 iref = s_bp[0x27];
        for (u32 i = 0; i < ind_count; i++) {
            u32 texmap = bits(iref, i * 6, 3);
            u32 texcoord = bits(iref, i * 6 + 3, 3);
            if (texcoord >= numtexgens) texcoord = 0;
            calc_lod(t, &t->IndirectLod[i], &t->IndirectLinear[i],
                     texmap, texcoord);
        }
    }
    u32 last = s_cfg.numtevstages;
    for (u32 i = 0; i <= last; i++) {
        const TevStageCfg* sc = &s_cfg.stage[i];
        if (sc->enable)
            calc_lod(t, &t->TextureLod[i], &t->TextureLinear[i], sc->texmap, sc->texcoordSel);
    }
}

/* GCN_GX_PIXEL_STATS SLOPE bucket: everything raster_pixel does before handing
 * off to tev_draw — z slope eval, early-Z, color slope evals, UV fixed-point
 * conversion. Returns 1 if the pixel is ready for tev_draw, 0 if early-Z
 * rejected it. Factored out of raster_pixel() purely so the timed wrapper
 * below has one call to time regardless of which of the two outcomes this
 * pixel hits — same technique as draw_triangle's wrapper over
 * draw_triangle_impl. Behavior is byte-identical to the pre-split function. */
static int raster_pixel_prep(Tev* t, s32 x, s32 y, s32 xi, s32 yi) {
    s32 z = (s32)slope_value(&s_ZSlope, x, y);
    if (z < 0) z = 0; else if (z > 16777215) z = 16777215;

    if (s_cfg.zt_enable && s_cfg.zt_early) {
        if (!ZCompare((u16)x, (u16)y, (u32)z)) return 0;
    }
    RBPixel* p = &s_rb_w[t->wid].px[xi][yi];
    t->Position[0] = x; t->Position[1] = y; t->Position[2] = z;
    for (u32 i = 0; i < s_cfg.numcolchans; i++)
        for (int comp = 0; comp < 4; comp++) {
            float c = slope_value(&s_ColorSlopes[i][comp], x, y);
            s16 cc = (s16)(c < 0 ? 0 : c > 255 ? 255 : c);
            t->Color[i][comp] = (u8)cc;
        }
    for (u32 i = 0; i < s_cfg.numtexgens; i++) {
        t->UvS[i] = (s32)(p->Uv[i][0] * 128.0f);
        t->UvT[i] = (s32)(p->Uv[i][1] * 128.0f);
    }
    return 1;
}

/* GCN_GX_PIXEL_STATS BLOCK bucket: build_block's whole per-2x2-block wall time,
 * timed at its single call site (draw_triangle_impl) — same thin-wrapper
 * technique as draw_triangle over draw_triangle_impl. */
static void build_block_timed(Tev* t, s32 bx, s32 by) {
    if (!s_pixel_stats) { build_block(t, bx, by); return; }
    u64 t0 = __rdtsc();
    build_block(t, bx, by);
    s_tsc_block += __rdtsc() - t0;
}

/* Dispatch to the fused specialized path when build_draw_cfg's selection
 * matched one (s_cfg.fused != NULL), else the general tev_draw — see the big
 * fused_pixel_A/B/C comment block above tev_draw for what this trades away
 * (nothing: verified bit-exact, same-binary A/B via GCN_GX_NO_FUSED is the
 * task's primary exactness proof). */
static inline void tev_shade(Tev* t) {
    if (s_cfg.fused) s_cfg.fused(t); else tev_draw(t);
}

static void raster_pixel(Tev* t, s32 x, s32 y, s32 xi, s32 yi) {
    if (!s_pixel_stats) {
        if (raster_pixel_prep(t, x, y, xi, yi)) tev_shade(t);   /* runs the combiner, late-Z, alpha test, then blends */
        return;
    }
    u64 t0 = __rdtsc();
    int ready = raster_pixel_prep(t, x, y, xi, yi);
    s_tsc_slope += __rdtsc() - t0;
    if (!ready) { s_ps_earlyz_rejected++; return; }
    tev_shade(t);
}

/* ============================================================================
 * Fused-draw specialization of raster_pixel/raster_pixel_prep (perf task,
 * dead-work elimination — CLAUDE.md gx-raster "skip dead per-pixel work on
 * fused draws"). Selected ONCE PER TRIANGLE by draw_triangle_impl (which reads
 * s_cfg.fused — itself set ONCE PER DRAW by build_draw_cfg — into a local
 * function pointer before entering the pixel loops; see draw_triangle_impl),
 * never re-checked per pixel. Used only when s_cfg.fused != NULL, i.e. every
 * fused_common_match()/fused_stage_match() precondition documented above
 * fused_pixel_A/B/C already holds for the whole draw.
 *
 * What's skipped, and why it is provably dead for a fused draw:
 *
 *  - z slope eval (slope_value(&s_ZSlope,...)), its clamp, and the
 *    t->Position[2] write. Grepped every reader of t->Position[2] in this
 *    file: (1) raster_pixel_prep's own early-Z branch, gated on
 *    `s_cfg.zt_enable && s_cfg.zt_early`; (2) blend_stage's late-Z ZCompare,
 *    gated on `s_cfg.zt_enable && !s_cfg.zt_early`. fused_common_match pins
 *    zt_enable==0, so BOTH gates are always false for a fused draw. And
 *    blend_stage itself is never even called on a fused draw: tev_shade only
 *    reaches it via tev_draw's tail, whereas a fused draw's tev_comb_finish
 *    always passes fused_blend_stage, which reads Position[0]/[1] only (EFB
 *    offset, Dither) and never Position[2]. So z has zero readers on a fused
 *    draw's pixels, in either the early or late position.
 *  - the 3 RGB color-slope evals (Color[0][0..2]; comp 3/alpha is unaffected
 *    and always evaluated below), UNLESS the selected fused function actually
 *    reads RasColor.rgb — tracked per-draw by `s_cfg.fused_needs_ras_rgb` (set
 *    alongside `s_cfg.fused` in build_draw_cfg's tail). fused_common_match
 *    pins numcolchans==1 (so channel 1 is never touched even by the general
 *    path) and every fused stage's colorchan==0. fused_pixel_A/fused_core_C
 *    (shared by B and C) are read in full above: each reads exactly
 *    `t->Color[0][3]` for RasColor.a and never references Color[0][0]/[1]/[2]
 *    (RasColor.r/g/b) at all, so their draws still skip the RGB evals below —
 *    the general path only ever reads those through SetRasColor/color_arg,
 *    which the fused functions bypass entirely (they never call tev_draw).
 *    fused_pixel_D and fused_pixel_F are the exceptions (their color folds
 *    ARE RasColor.rgb, see their derivation comments): `fused_needs_ras_rgb`
 *    is set for both, so this function evaluates Color[0][0..2] for their
 *    draws exactly like raster_pixel_prep's own unconditional loop does —
 *    this is NOT dead work for a config-D/config-F draw, so it is not
 *    elided. fused_pixel_E (the other D1-residuals-sweep addition) is like
 *    A/B/C: its color fold reads TexColor/Reg[1]/Reg[2], never RasColor.rgb,
 *    so it does NOT set fused_needs_ras_rgb and still skips this work.
 *
 * Stale-read hazard analysis (t->Position[2] and t->Color[0][0..2] are fields
 * of the single shared `s_tev`/Tev* instance reused across every pixel and
 * draw, so a skipped WRITE could in principle leak a PRIOR draw's/pixel's
 * value to a later READ):
 *  - Within a fused draw whose function doesn't need RasColor.rgb (A/B/C/E):
 *    no reader exists (see above), so whatever stale bytes sit in
 *    Position[2]/Color[0][0..2] are never observed. Not a hazard.
 *  - Within a config-D or config-F draw: Color[0][0..2] is freshly written
 *    below, every pixel, before fused_pixel_D/fused_pixel_F reads it. Not a
 *    hazard either.
 *  - A later GENERAL-path draw: raster_pixel (unmodified) always calls the
 *    unmodified raster_pixel_prep first, which unconditionally recomputes and
 *    overwrites Position[2] and Color[i][0..3] for every one of its pixels
 *    BEFORE tev_shade/tev_draw ever reads them. So a general draw can never
 *    observe a fused draw's skipped writes either. No cross-draw hazard in
 *    either direction.
 * ==========================================================================*/
static void raster_pixel_prep_fused(Tev* t, s32 x, s32 y, s32 xi, s32 yi) {
    RBPixel* p = &s_rb_w[t->wid].px[xi][yi];
    t->Position[0] = x; t->Position[1] = y;   /* Position[2]/z: skipped, see above */
    if (s_cfg.fused_needs_ras_rgb) {
        /* fused_pixel_D/fused_pixel_F need the full RasColor -- same
         * per-component loop body as raster_pixel_prep's general numcolchans
         * loop, unrolled for comp 0..2 (comp 3 below is shared with the
         * A/B/C/E case). */
        for (int comp = 0; comp < 3; comp++) {
            float c = slope_value(&s_ColorSlopes[0][comp], x, y);
            s16 cc = (s16)(c < 0 ? 0 : c > 255 ? 255 : c);
            t->Color[0][comp] = (u8)cc;
        }
    }
    {
        /* Channel-0 alpha (comp 3) — every fused_pixel_A/B/C/D reads this. */
        float c = slope_value(&s_ColorSlopes[0][3], x, y);
        s16 cc = (s16)(c < 0 ? 0 : c > 255 ? 255 : c);
        t->Color[0][3] = (u8)cc;
    }
    for (u32 i = 0; i < s_cfg.numtexgens; i++) {
        t->UvS[i] = (s32)(p->Uv[i][0] * 128.0f);
        t->UvT[i] = (s32)(p->Uv[i][1] * 128.0f);
    }
}

/* Fused counterpart of raster_pixel(). No ready/reject branch to mirror: a
 * fused draw's zt_early is always 0 (fused_common_match), so
 * raster_pixel_prep's early-Z gate — the only way raster_pixel_prep can ever
 * return 0 — never fires; every fused pixel always proceeds to shading, same
 * as the general path would for these same draws (verified by the
 * GCN_GX_NO_FUSED=1 A/B: general-path early-Z rejection count on these draws
 * is always 0 too, since zt_early==0 there as well). */
static void raster_pixel_fused(Tev* t, s32 x, s32 y, s32 xi, s32 yi) {
    if (!s_pixel_stats) {
        raster_pixel_prep_fused(t, x, y, xi, yi);
        s_cfg.fused(t);
        return;
    }
    u64 t0 = __rdtsc();
    raster_pixel_prep_fused(t, x, y, xi, yi);
    s_tsc_slope += __rdtsc() - t0;
    s_cfg.fused(t);
}

static void update_zslope(const OutVtx* v0, const OutVtx* v1, const OutVtx* v2,
                          s32 x_off, s32 y_off) {
    /* zfreeze is off for the menu; recompute each triangle. */
    s32 X1 = iround(16.0f * (v0->screenPosition[0] - x_off)) - 9;
    s32 Y1 = iround(16.0f * (v0->screenPosition[1] - y_off)) - 9;
    SlopeCtx ctx = make_ctx(v0, v1, v2, (X1 + 0xF) >> 4, (Y1 + 0xF) >> 4, x_off, y_off);
    s_ZSlope = make_slope(v0->screenPosition[2], v1->screenPosition[2], v2->screenPosition[2], &ctx);
}

/* ============================================================================
 * GX-MT: tile-parallel triangle scan (see the GX-MT block comment at the top
 * of the file for the state-ownership argument, and draw_parallel_ok() in
 * build_draw_cfg for the per-draw carry-freedom proof that gates every fork).
 *
 * Partition: block row k (y = block_miny + k*BLK) belongs to worker
 * (k mod nthreads). Every EFB pixel of the triangle is therefore scanned by
 * exactly one worker, in that worker's ordinary top-to-bottom/left-to-right
 * order — and since a triangle covers any pixel at most once, per-location
 * write ORDER across triangles is enforced by the join below, making the
 * result byte-identical to the serial scan regardless of thread scheduling.
 * With nthreads==1 / row0==0 / rowstride==1 the loop below IS the old serial
 * loop, same iteration sequence, same arithmetic.
 * ==========================================================================*/
typedef GxRasterTriScan TriScan;

static void scan_one_block_row(Tev* t, const TriScan* ts, s32 y) {
    /* Fused-path pixel-fn selection, hoisted out of the pixel loops:
     * s_cfg.fused is set once per DRAW by build_draw_cfg (never mid-draw). See
     * the big comment above raster_pixel_prep_fused for what the fused
     * specialization skips and why it's provably dead work. */
    void (*pixel_fn)(Tev*, s32, s32, s32, s32) = s_cfg.fused ? raster_pixel_fused : raster_pixel;

    {
        for (s32 x = ts->block_minx; x < ts->maxx; x += BLK) {
            s32 x1_ = x + BLK - 1, y1_ = y + BLK - 1;
            s32 x0 = x << 4, xx1 = x1_ << 4, y0 = y << 4, yy1 = y1_ << 4;
            int a00 = ts->C1 + ts->DX12 * y0 - ts->DY12 * x0 > 0, a10 = ts->C1 + ts->DX12 * y0 - ts->DY12 * xx1 > 0;
            int a01 = ts->C1 + ts->DX12 * yy1 - ts->DY12 * x0 > 0, a11 = ts->C1 + ts->DX12 * yy1 - ts->DY12 * xx1 > 0;
            int a = (a00) | (a10 << 1) | (a01 << 2) | (a11 << 3);
            int b00 = ts->C2 + ts->DX23 * y0 - ts->DY23 * x0 > 0, b10 = ts->C2 + ts->DX23 * y0 - ts->DY23 * xx1 > 0;
            int b01 = ts->C2 + ts->DX23 * yy1 - ts->DY23 * x0 > 0, b11 = ts->C2 + ts->DX23 * yy1 - ts->DY23 * xx1 > 0;
            int bb = (b00) | (b10 << 1) | (b01 << 2) | (b11 << 3);
            int c00 = ts->C3 + ts->DX31 * y0 - ts->DY31 * x0 > 0, c10 = ts->C3 + ts->DX31 * y0 - ts->DY31 * xx1 > 0;
            int c01 = ts->C3 + ts->DX31 * yy1 - ts->DY31 * x0 > 0, c11 = ts->C3 + ts->DX31 * yy1 - ts->DY31 * xx1 > 0;
            int cc = (c00) | (c10 << 1) | (c01 << 2) | (c11 << 3);
            if (a == 0 || bb == 0 || cc == 0) continue;

            build_block_timed(t, x, y);
            if (a == 0xF && bb == 0xF && cc == 0xF && x >= ts->minx && x1_ < ts->maxx && y >= ts->miny && y1_ < ts->maxy) {
                for (s32 iy = 0; iy < BLK; iy++)
                    for (s32 ix = 0; ix < BLK; ix++)
                        pixel_fn(t, x + ix, y + iy, ix, iy);
            } else {
                s32 CY1 = ts->C1 + ts->DX12 * y0 - ts->DY12 * x0;
                s32 CY2 = ts->C2 + ts->DX23 * y0 - ts->DY23 * x0;
                s32 CY3 = ts->C3 + ts->DX31 * y0 - ts->DY31 * x0;
                for (s32 iy = 0; iy < BLK; iy++) {
                    s32 CX1 = CY1, CX2 = CY2, CX3 = CY3;
                    for (s32 ix = 0; ix < BLK; ix++) {
                        if (CX1 > 0 && CX2 > 0 && CX3 > 0) {
                            if (x + ix >= ts->minx && x + ix < ts->maxx && y + iy >= ts->miny && y + iy < ts->maxy)
                                pixel_fn(t, x + ix, y + iy, ix, iy);
                        }
                        CX1 -= ts->FDY12; CX2 -= ts->FDY23; CX3 -= ts->FDY31;
                    }
                    CY1 += ts->FDX12; CY2 += ts->FDX23; CY3 += ts->FDX31;
                }
            }
        }
    }
}

/* Serial scan: all block rows in order — with one worker this is the old
 * single loop, same iteration sequence, same arithmetic. */
static void scan_block_rows_serial(Tev* t, const TriScan* ts) {
    for (s32 y = ts->block_miny; y < ts->maxy; y += BLK)
        scan_one_block_row(t, ts, y);
}

/* ---- GX-MT worker pool ----------------------------------------------------
 * Spawned once per process by gx_mt_resolve() (first gx_raster_draw). Workers
 * hybrid-wait on s_mt_epoch: spin briefly (a draw burst publishes forks
 * back-to-back, often < 1ms apart), then WaitOnAddress so idle frames cost no
 * CPU.
 *
 * Row distribution is DYNAMIC: every participant (main included) claims the
 * next unclaimed block row and bumps s_mt_rows_done after finishing it; the
 * join condition is rows_done == nrows, NOT "all workers checked in". This
 * makes stragglers harmless: a worker that wakes late (or whose pinned core
 * is busy with background load) simply claims fewer rows — or none — and
 * the fork completes without it. (The first, static row0/stride split
 * waited for every worker's check-in, so one preempted or still-waking
 * worker gated every fork — measured as join-wait >> scan on a loaded
 * desktop.) Which worker scans a row only selects which private
 * Tev/rb/texel-cache computes it; the bytes are identical, so dynamic
 * assignment does not affect exactness.
 *
 * The claim is a CAS on ONE 64-bit word packing (fork_id << 32 |
 * nrows << 16 | next_row). Packing nrows and the row counter together is
 * load-bearing, not an optimization: with a separate fetch_add counter and
 * nrows variable, a worker suspended between the two could validate a STALE
 * row index against the NEXT fork's row count, double-claiming a row the
 * new fork's counter also hands out (double-scanned row + corrupted
 * rows_done -> the join's `!=` spin never exits; hit in the wild on the
 * card-select screen, gdb-confirmed). With the packed word, the bounds
 * check reads nrows from the very value the CAS claims against, so a claim
 * is valid-by-construction for the job the word currently describes:
 *  - CAS succeeds -> row k < nrows of the CURRENT word's fork is owned
 *    exclusively (the +1 only touches the row field; nrows <= 264 rows so
 *    the field never carries). s_mt_job/main's join guarantee then keep the
 *    job fields frozen until this row's rows_done increment lands — main
 *    cannot pass the join, let alone republish, while an owned row is
 *    unfinished.
 *  - CAS fails (any interleaving: another claimer, or a republish changing
 *    fork_id) -> reload and re-validate. A stale worker either helps the
 *    new fork or sees row >= nrows and goes back to sleep.
 *
 * Publication protocol per forked triangle (main thread):
 *   1. copy the per-draw Tev template into every worker's s_tev_w[i]
 *   2. s_mt_job = triangle, rows_done = 0
 *   3. s_mt_grab = (new fork_id, nrows, row 0) (RELEASE — after 1+2, so a
 *      claim on the new word sees the fully-written job it belongs to)
 *   4. s_mt_epoch++ (RELEASE) + WakeByAddressAll
 *   5. claim rows itself, then spin until rows_done == nrows (ACQUIRE —
 *      every row's EFB writes are release-published by its rows_done
 *      increment; no increment can leak across forks, since a fork's join
 *      only exits once every claimed row of THAT fork has finished) */
static int s_mt_threads = -1;   /* resolved count incl. main thread; 1 = serial */
static u32 s_mt_min_area = 2048; /* fork gate, px of post-scissor bbox
                                  * (GCN_GX_MT_MIN_AREA). Default from the
                                  * [gx-area-hist] measurement: >= 2^11 covers
                                  * ~92% of scan wall in ~5.9K forks/boot. */
static TriScan s_mt_job;
static u32 s_mt_fork_id;             /* main-only; distinguishes forks in s_mt_grab */
static volatile s64 s_mt_grab;       /* fork_id<<32 | nrows<<16 | next_row */
static volatile s32 s_mt_epoch;
static volatile s32 s_mt_rows_done;  /* completed block rows this fork */

/* Claim-and-scan loop shared by main and workers during a fork. */
static void scan_rows_dynamic(Tev* t) {
    for (;;) {
        s64 cur = __atomic_load_n(&s_mt_grab, __ATOMIC_ACQUIRE);
        u32 k = (u32)((u64)cur & 0xFFFFu);
        u32 nrows = (u32)(((u64)cur >> 16) & 0xFFFFu);
        if (k >= nrows) return;
        if (!__atomic_compare_exchange_n(&s_mt_grab, &cur, cur + 1, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            continue;   /* lost the claim — revalidate from the fresh word */
        scan_one_block_row(t, &s_mt_job, s_mt_job.block_miny + (s32)k * BLK);
        __atomic_add_fetch(&s_mt_rows_done, 1, __ATOMIC_RELEASE);
    }
}

/* GCN_GX_MT_STATS=1: fork/join accounting. Unlike the per-pixel stats knobs
 * this does NOT force serial — every counter below is written by the MAIN
 * thread only (at fork/join boundaries, never inside the pixel path), so it
 * is MT-safe by construction and measures the real parallel run. Printed by
 * gx_raster_print_mt_stats() on gx.c's shared stats cadence. */
static int s_mt_stats = -1;
static u64 s_mt_forks;          /* forked triangles */
static u64 s_mt_serial_tris;    /* triangles scanned serially (any reason) */
static u64 s_mt_fork_tsc;       /* main-thread wall inside the whole fork path
                                 * (template copies + publish + own scan + join) */
static u64 s_mt_scan_tsc;       /* ...of which: main's own scan_block_rows */
static u64 s_mt_join_tsc;       /* ...of which: join spin after own scan done */

static DWORD WINAPI gx_mt_worker(LPVOID arg) {
    const int wid = (int)(intptr_t)arg;
    s32 seen = 0;
    for (;;) {
        s32 e;
        int spins = 0;
        while ((e = __atomic_load_n(&s_mt_epoch, __ATOMIC_ACQUIRE)) == seen) {
            if (++spins < 16384) { _mm_pause(); continue; }
            s32 cmp = seen;
            WaitOnAddress((volatile VOID*)&s_mt_epoch, &cmp, sizeof cmp, INFINITE);
            spins = 0;
        }
        seen = e;
        scan_rows_dynamic(&s_tev_w[wid]);
    }
    return 0;   /* unreachable — workers live for the whole process */
}

static void gx_mt_resolve(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const int ncpu = (int)si.dwNumberOfProcessors;
    const char* e = getenv("GCN_GX_THREADS");
    int n = e ? atoi(e) : 0;
    if (n <= 0) {
        const char* backend = getenv("GCN_GX_BACKEND");
        const char* window = getenv("GCN_WINDOW");
        int resident_vulkan =
            (backend && strcmp(backend, "vulkan") == 0) ||
            ((!backend || !*backend) && window && *window && *window != '0');
        if (!e && resident_vulkan) {
            /* Exact resident draws return from the Vulkan triangle sink
             * before the CPU pixel scanner runs. Spawning GX scan workers in
             * that mode only creates idle-spin/scheduler contention. */
            n = 1;
        } else {
            n = ncpu / 2;   /* one worker per physical core (SMT/2) */
            if (n > 8) n = 8;
            if (n < 1) n = 1;
        }
    }
    if (n > GX_MT_MAX) n = GX_MT_MAX;
    /* The per-pixel stats/census knobs accumulate into SHARED counters from
     * inside the pixel path — exact only single-threaded. They are
     * measurement modes, so they win: force serial rather than race the
     * counters. (GCN_DISPATCH_STATS is unaffected — it measures outside the
     * rasterizer and keeps working, now timing the parallel wall.) */
    if (n > 1 && (s_draw_stats == 1 || s_pixel_stats == 1 ||
                  getenv("GCN_GX_TEV_CENSUS"))) {
        fprintf(stderr, "gx_raster: GX-MT forced serial — a GX stats/census knob is on\n");
        n = 1;
    }
    { const char* a = getenv("GCN_GX_MT_MIN_AREA");
      if (a) { long v = atol(a); if (v > 0) s_mt_min_area = (u32)v; } }
    s_mt_stats = getenv("GCN_GX_MT_STATS") ? 1 : 0;   /* MT-safe, see s_mt_stats */
    /* Affinity: one distinct PHYSICAL core per thread (Windows enumerates SMT
     * siblings as adjacent logical pairs, so even indices are distinct
     * cores). Without pinning, the scheduler regularly parks an idle-SPINNING
     * worker on the main thread's SMT sibling, and the spin steals core
     * throughput from EVERYTHING the main thread runs (CPU blocks, DSP, GX
     * decode, its own row scan) — measured as the whole MT win evaporating
     * (main's row-scan share cost ~2.2-2.7x fair, [gx-mt-stats]).
     *
     * Main goes on the LAST physical core, workers on cores 0..n-2: logical
     * CPU 0 is where Windows concentrates interrupts/DPCs and stray threads,
     * and pinning MAIN there measurably inflated every non-GX dispatch
     * bucket (GCN_DISPATCH_STATS: dsp/block-exec ~+38% vs serial). A worker
     * on the noisy core just grabs fewer rows (the dynamic distribution
     * absorbs it); main must not share its core with anything. Skipped when
     * there aren't enough logical CPUs for the even mapping — better
     * unpinned than workers time-slicing one core. */
    const int pin = (2 * (n - 1) < ncpu);
    if (n > 1 && pin)
        SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << (ncpu - 2));
    for (int i = 1; i < n; i++) {
        HANDLE h = CreateThread(NULL, 0, gx_mt_worker, (LPVOID)(intptr_t)i, 0, NULL);
        if (!h) {
            fprintf(stderr, "gx_raster: GX-MT CreateThread(%d) failed; continuing with %d thread(s)\n",
                    i, i);
            n = i;   /* workers 1..i-1 exist; dynamic rows cover the rest */
            break;
        }
        if (pin)
            SetThreadAffinityMask(h, (DWORD_PTR)1 << (2 * (i - 1)));
        CloseHandle(h);   /* never joined — workers live for the process */
    }
    if (n > 1)
        fprintf(stderr, "gx_raster: GX-MT %d threads (fork at bbox >= %u px; "
                "GCN_GX_THREADS=1 to disable)\n", n, s_mt_min_area);
    s_mt_threads = n;
}

static u32 compute_program_id(void) {
    if (s_cfg.fused == fused_pixel_A) return 1;
    if (s_cfg.fused == fused_pixel_B) return 2;
    if (s_cfg.fused == fused_pixel_C) return 3;
    if (s_cfg.fused == fused_pixel_D) return 4;
    if (s_cfg.fused == fused_pixel_E) return 5;
    if (s_cfg.fused == fused_pixel_F) return 6;
    if (s_cfg.fused == fused_pixel_G) return 7;
    if (s_cfg.fused == fused_pixel_H) return 8;
    if (s_cfg.fused == fused_pixel_I) return 9;
    if (s_cfg.fused == fused_pixel_J) return 10;
    if (!s_cfg.fused && gpu_depth_K_match()) return 11;
    if (!s_cfg.fused && gpu_depth_L_match()) return 12;
    if (!s_cfg.fused && gpu_program_M_match()) return 13;
    if (!s_cfg.fused && gpu_program_N_match()) return 14;
    if (!s_cfg.fused && gpu_program_O_match()) return 15;
    if (!s_cfg.fused && gpu_program_P_match()) return 16;
    if (!s_cfg.fused && gpu_program_Q_match()) return 17;
    if (!s_cfg.fused && gpu_program_R_match()) return 18;
    if (!s_cfg.fused && gpu_program_S_match()) return 19;
    if (!s_cfg.fused && gpu_program_T_match()) return 20;
    if (!s_cfg.fused && gpu_program_U_match()) return 21;
    if (!s_cfg.fused && gpu_program_V_match()) return 22;
    if (!s_cfg.fused && gpu_program_W_match()) return 23;
    if (!s_cfg.fused && gpu_program_X_match()) return 24;
    if (s_cfg.fused == fused_pixel_Y) return 25;
    if (s_cfg.fused == fused_pixel_Z) return 26;
    if (s_cfg.fused == fused_pixel_AA) return 27;
    if (!s_cfg.fused && gpu_program_AB_match()) return 28;
    if (!s_cfg.fused && gpu_program_AC_match()) return 29;
    if (!s_cfg.fused && gpu_program_AD_match()) return 30;
    return 0;
}

static u32 fused_program_id(void) { return s_cfg.program_id; }

static void draw_triangle_impl(Tev* t, const OutVtx* v0, const OutVtx* v1, const OutVtx* v2) {
    /* Setup only — edge equations, bbox/scissor clamp, slopes. The pixel
     * loops (and the fused-path pixel-fn selection, hoisted per triangle)
     * live in scan_block_rows(), which the tail below runs either serially
     * (worker 0, the exact old loop) or forked across the GX-MT pool. */
    if (s_draw_stats) s_last_tri_area = 0;   /* bucket 0 unless the bbox clamp below survives */

    s32 x_off = s_scissor_xoff, y_off = s_scissor_yoff;
    u32 triangle_program = fused_program_id();
    /* Programs whose z slope is provably dead: R/S plus the zt_enable==0
     * card-screen programs T/U/V/W (their matchers pin zt_enable==0 and
     * zupd==0, so both of Position[2]'s gated readers are dead on any path —
     * same argument as the fused skip below).  X (24) is excluded: its
     * matcher pins zt_enable==1, and both the GPU depth block and a software
     * scan of its draws read the z slope. */
    int rs_gpu_program = triangle_program == 18u || triangle_program == 19u ||
                         (triangle_program >= 20u && triangle_program <= 23u);
    /* Dead-slope-setup skip (perf task, follow-up to ff6b617): update_zslope
     * populates s_ZSlope, whose one and only reader is raster_pixel_prep's
     * `s32 z = (s32)slope_value(&s_ZSlope, ...)` — reached exclusively via
     * raster_pixel (the GENERAL pixel path). A fused draw's pixel_fn is
     * raster_pixel_fused -> raster_pixel_prep_fused, which never calls
     * raster_pixel_prep and never reads s_ZSlope (see the big comment above
     * raster_pixel_prep_fused: fused_common_match pins zt_enable==0, so both
     * of Position[2]'s gated readers are dead). Grepped: s_ZSlope has exactly
     * one read site in this file. Skipping the eval on fused draws is provably
     * a no-op for them; the general path still calls update_zslope
     * unconditionally on every triangle (this `if` compiles away to nothing
     * for it), so it keeps rebuilding s_ZSlope before raster_pixel_prep reads
     * it, same as before this change. */
    if (!s_cfg.fused && !rs_gpu_program)
        update_zslope(v0, v1, v2, x_off, y_off);

    s32 Y1 = iround(16.0f * (v0->screenPosition[1] - y_off)) - 9;
    s32 Y2 = iround(16.0f * (v1->screenPosition[1] - y_off)) - 9;
    s32 Y3 = iround(16.0f * (v2->screenPosition[1] - y_off)) - 9;
    s32 X1 = iround(16.0f * (v0->screenPosition[0] - x_off)) - 9;
    s32 X2 = iround(16.0f * (v1->screenPosition[0] - x_off)) - 9;
    s32 X3 = iround(16.0f * (v2->screenPosition[0] - x_off)) - 9;

    s32 DX12 = X1 - X2, DX23 = X2 - X3, DX31 = X3 - X1;
    s32 DY12 = Y1 - Y2, DY23 = Y2 - Y3, DY31 = Y3 - Y1;
    s32 FDX12 = DX12 * 16, FDX23 = DX23 * 16, FDX31 = DX31 * 16;
    s32 FDY12 = DY12 * 16, FDY23 = DY23 * 16, FDY31 = DY31 * 16;

    s32 minx = (((X1 < X2 ? X1 : X2) < X3 ? (X1 < X2 ? X1 : X2) : X3) + 0xF) >> 4;
    s32 maxx = (((X1 > X2 ? X1 : X2) > X3 ? (X1 > X2 ? X1 : X2) : X3) + 0xF) >> 4;
    s32 miny = (((Y1 < Y2 ? Y1 : Y2) < Y3 ? (Y1 < Y2 ? Y1 : Y2) : Y3) + 0xF) >> 4;
    s32 maxy = (((Y1 > Y2 ? Y1 : Y2) > Y3 ? (Y1 > Y2 ? Y1 : Y2) : Y3) + 0xF) >> 4;

    if (minx < s_scissor_left)   minx = s_scissor_left;
    if (maxx > s_scissor_right)  maxx = s_scissor_right;
    if (miny < s_scissor_top)    miny = s_scissor_top;
    if (maxy > s_scissor_bottom) maxy = s_scissor_bottom;
    if (minx >= maxx || miny >= maxy) return;
    if (s_debug_pending_index >= 0) {
        GxDebugDraw* d = &s_debug_pending;
        u32 tri_area = (u32)((maxx - minx) * (maxy - miny));
        d->triangles_rasterized++;
        d->bbox_area_sum += tri_area;
        if (!d->bbox_valid) {
            d->bbox_valid = 1;
            d->bbox_minx = minx; d->bbox_miny = miny;
            d->bbox_maxx = maxx; d->bbox_maxy = maxy;
        } else {
            if (minx < d->bbox_minx) d->bbox_minx = minx;
            if (miny < d->bbox_miny) d->bbox_miny = miny;
            if (maxx > d->bbox_maxx) d->bbox_maxx = maxx;
            if (maxy > d->bbox_maxy) d->bbox_maxy = maxy;
        }
        if (tri_area > d->largest_triangle_area) {
            d->largest_triangle_area = tri_area;
            for (u32 i = 0; i < 3u; ++i) {
                const OutVtx* v = i == 0 ? v0 : (i == 1 ? v1 : v2);
                memcpy(d->largest_triangle[i].obj, v->objPos,
                       sizeof d->largest_triangle[i].obj);
                memcpy(d->largest_triangle[i].mv, v->mvPosition,
                       sizeof d->largest_triangle[i].mv);
                memcpy(d->largest_triangle[i].clip, v->projectedPosition,
                       sizeof d->largest_triangle[i].clip);
                memcpy(d->largest_triangle[i].screen, v->screenPosition,
                       sizeof d->largest_triangle[i].screen);
                memcpy(d->largest_triangle[i].normal, v->normal,
                       sizeof d->largest_triangle[i].normal);
                memcpy(d->largest_triangle[i].color, v->color,
                       sizeof d->largest_triangle[i].color);
                memcpy(d->largest_triangle[i].texcoord, v->texCoords,
                       sizeof d->largest_triangle[i].texcoord);
                d->largest_triangle[i].pos_mtx = v->posMtx;
            }
        }
    }
    /* Histogram input for the draw_triangle wrapper: post-scissor bbox area.
     * (Entry already zeroed it, so the early returns above land in bucket 0.) */
    if (s_draw_stats) s_last_tri_area = (u32)((maxx - minx) * (maxy - miny));

    /* Per-frame coverage anomaly detector (always on, IPL flood/drop
     * investigation): accumulate every triangle's post-scissor bbox area for
     * the frame and remember the top-8 largest with provenance (program,
     * bbox, per-stage vertex positions). gx_raster_frame_anomaly_mark()
     * (called at GXSetDrawDone) compares the frame's total against a rolling
     * median: a flood spikes it (many stretched mid-size triangles), a drop
     * dips it (panel draws missing) — either way the top-8 of the anomalous
     * frame names the culprit draws. A 50%-screen single-triangle census
     * already came back EMPTY across garbled frames, so the corruption is
     * mid-size-many, not one giant triangle — hence per-frame accounting. */
    {
        u32 tri_area = (u32)((maxx - minx) * (maxy - miny));
        s_fa_frame_area += tri_area;
        s_fa_frame_tris++;
        if (tri_area > s_fa_top[7].area) {
            int slot = 7;
            while (slot > 0 && tri_area > s_fa_top[slot - 1].area) slot--;
            for (int m = 7; m > slot; m--) s_fa_top[m] = s_fa_top[m - 1];
            s_fa_top[slot].area = tri_area;
            s_fa_top[slot].prog = triangle_program;
            s_fa_top[slot].dl = gcn_gx_current_dl();
            s_fa_top[slot].minx = minx; s_fa_top[slot].miny = miny;
            s_fa_top[slot].maxx = maxx; s_fa_top[slot].maxy = maxy;
            for (int c = 0; c < 3; c++) {
                const OutVtx* vv = c == 0 ? v0 : (c == 1 ? v1 : v2);
                s_fa_top[slot].mv[c][0] = vv->mvPosition[0];
                s_fa_top[slot].mv[c][1] = vv->mvPosition[1];
                s_fa_top[slot].mv[c][2] = vv->mvPosition[2];
                s_fa_top[slot].w[c] = vv->projectedPosition[3];
                s_fa_top[slot].sx[c] = vv->screenPosition[0];
                s_fa_top[slot].sy[c] = vv->screenPosition[1];
                s_fa_top[slot].op[c][0] = vv->objPos[0];
                s_fa_top[slot].op[c][1] = vv->objPos[1];
                s_fa_top[slot].op[c][2] = vv->objPos[2];
                s_fa_top[slot].pidx[c] = vv->posMtx;
            }
        }
    }

    SlopeCtx ctx = make_ctx(v0, v1, v2, (X1 + 0xF) >> 4, (Y1 + 0xF) >> 4, x_off, y_off);
    float w[3] = { 1.0f / v0->projectedPosition[3], 1.0f / v1->projectedPosition[3],
                   1.0f / v2->projectedPosition[3] };
    s_WSlope = make_slope(w[0], w[1], w[2], &ctx);
    /* Dead-slope-setup skip continued: the 3 RGB (comp 0..2) color-slope
     * evals are provably dead on a fused draw whose selected function never
     * reads RasColor.rgb — fused_common_match pins numcolchans==1 (so this
     * loop's `i` only ever reaches 0 when fused; the i==1 iteration simply
     * never runs, same as always) and every fused stage's colorchan==0, and
     * fused_pixel_A/fused_core_C (shared by B/C) and fused_pixel_E read only
     * Color[0][3] (alpha) — never Color[0][0]/[1]/[2] (see the big comment
     * above raster_pixel_prep_fused). fused_pixel_D and fused_pixel_F are the
     * exceptions: their color folds ARE RasColor.rgb, so their draws
     * (`s_cfg.fused_needs_ras_rgb`, set alongside `s_cfg.fused` in
     * build_draw_cfg) must NOT skip comp 0..2 here
     * — this triangle-level setup feeds raster_pixel_prep_fused's per-pixel
     * slope_value() reads of s_ColorSlopes[0][0..2], so skipping this and
     * fixing only the per-pixel side (or vice versa) would leave the other
     * half of the pipeline reading/writing a slope that was never built.
     * comp 3 (alpha) is unaffected and still evaluated either way. General
     * path: s_cfg.fused is NULL, so comp starts at 0 exactly as before —
     * byte-identical. */
    /* Programs U/V/X (21/22/24) never reference RasColor.rgb in any selector
     * (their pinned cc words name only Reg1/Tex/ONE/HALF inputs), so their
     * RGB color slopes are dead exactly like S's.  T/W (20/23) are the
     * opposite: their color IS the RasColor.rgb identity — comp 0 stays.
     * AB/AC/AD (28/29/30, see their derivation above gpu_program_AB_match)
     * are GPU-only like U/V/X and their pinned cc words name only
     * StageKonst/Reg1 -- RasColor.rgb never appears in any of the three, so
     * they join U/V/X's dead-RGB-slope list. (Y/Z/AA, 25-27, are FUSED and
     * DO read RasColor.rgb -- s_cfg.fused_needs_ras_rgb is set for all three
     * in build_draw_cfg, so the first clause below already keeps comp 0 for
     * them; they do not need a triangle_program entry here.) */
    int first_color_comp =
        ((s_cfg.fused && !s_cfg.fused_needs_ras_rgb) ||
         triangle_program == 19u || triangle_program == 21u ||
         triangle_program == 22u || triangle_program == 24u ||
         triangle_program == 28u || triangle_program == 29u ||
         triangle_program == 30u) ? 3 : 0;
    for (u32 i = 0; i < s_cfg.numcolchans; i++)
        for (int comp = first_color_comp; comp < 4; comp++)
            s_ColorSlopes[i][comp] = make_slope(v0->color[i][comp], v1->color[i][comp],
                                                v2->color[i][comp], &ctx);
    for (u32 i = 0; i < s_cfg.numtexgens; i++) {
        s_TexSlopes[i][0] = make_slope(v0->texCoords[i][0]*w[0], v1->texCoords[i][0]*w[1], v2->texCoords[i][0]*w[2], &ctx);
        s_TexSlopes[i][1] = make_slope(v0->texCoords[i][1]*w[0], v1->texCoords[i][1]*w[1], v2->texCoords[i][1]*w[2], &ctx);
        s_TexSlopes[i][2] = make_slope(v0->texCoords[i][2]*w[0], v1->texCoords[i][2]*w[1], v2->texCoords[i][2]*w[2], &ctx);
    }

    TriScan ts;
    ts.C1 = DY12 * X1 - DX12 * Y1;
    ts.C2 = DY23 * X2 - DX23 * Y2;
    ts.C3 = DY31 * X3 - DX31 * Y3;
    if (DY12 < 0 || (DY12 == 0 && DX12 > 0)) ts.C1++;
    if (DY23 < 0 || (DY23 == 0 && DX23 > 0)) ts.C2++;
    if (DY31 < 0 || (DY31 == 0 && DX31 > 0)) ts.C3++;

    ts.block_minx = minx & ~(BLK - 1);
    ts.block_miny = miny & ~(BLK - 1);
    ts.minx = minx; ts.maxx = maxx; ts.miny = miny; ts.maxy = maxy;
    ts.DX12 = DX12; ts.DX23 = DX23; ts.DX31 = DX31;
    ts.DY12 = DY12; ts.DY23 = DY23; ts.DY31 = DY31;
    ts.FDX12 = FDX12; ts.FDX23 = FDX23; ts.FDX31 = FDX31;
    ts.FDY12 = FDY12; ts.FDY23 = FDY23; ts.FDY31 = FDY31;

    if (s_triangle_sink) {
        GxRasterTriangleJob job;
        job.scan = ts;
        /* The sink packet exposes the full software fallback shape, but the
         * active program consumes only color/tex channels named by the draw
         * config.  Clearing the whole ~1 KiB struct for every resident GPU
         * triangle was several GiB of dead stores per IPL menu run. */
        memset(&job.z, 0, sizeof job.z);
        if (!s_cfg.fused && !rs_gpu_program) job.z = s_ZSlope;
        job.w = s_WSlope;
        job.num_color_chans = s_cfg.numcolchans;
        job.num_texgens = s_cfg.numtexgens;
        job.pixel_format = s_pf;
        job.fused_program = triangle_program;
        job.bm_au = s_bm_au;   /* only consumed by programs Y/Z (25/26) */
        for (u32 reg = 0; reg < 4; ++reg) {
            job.tev_reg[reg][0] = s_tev_w[0].Reg[reg].r;
            job.tev_reg[reg][1] = s_tev_w[0].Reg[reg].g;
            job.tev_reg[reg][2] = s_tev_w[0].Reg[reg].b;
            job.tev_reg[reg][3] = s_tev_w[0].Reg[reg].a;
        }
        for (u32 stage = 0; stage < 2; ++stage) {
            job.stage_konst[stage][0] = s_cfg.stage[stage].stage_konst.r;
            job.stage_konst[stage][1] = s_cfg.stage[stage].stage_konst.g;
            job.stage_konst[stage][2] = s_cfg.stage[stage].stage_konst.b;
            job.stage_konst[stage][3] = s_cfg.stage[stage].stage_konst.a;
        }
        memset(job.color[0], 0, sizeof job.color[0]);
        for (u32 i = 0; i < s_cfg.numcolchans; ++i) {
            int first = first_color_comp;
            for (int comp = first; comp < 4; ++comp)
                job.color[i][comp] = s_ColorSlopes[i][comp];
        }
        memset(job.tex[0], 0, sizeof job.tex[0]);
        for (u32 i = 0; i < s_cfg.numtexgens; ++i)
            for (u32 comp = 0; comp < 3; ++comp)
                job.tex[i][comp] = s_TexSlopes[i][comp];
        if (s_triangle_sink(s_triangle_sink_user, &job, 0))
            return;

        /* Keep the packet alive across the authoritative scan so a
         * differential sink can compare the GPU result after software. */
        if (s_mt_threads > 1 && s_cfg.parallel_ok &&
            (u32)((maxx - minx) * (maxy - miny)) >= s_mt_min_area) {
            u64 t0 = s_mt_stats == 1 ? __rdtsc() : 0;
            for (int i = 1; i < s_mt_threads; i++) {
                s_tev_w[i] = s_tev_w[0];
                s_tev_w[i].wid = i;
            }
            s32 nrows = (ts.maxy - ts.block_miny + BLK - 1) / BLK;
            s_mt_job = ts;
            __atomic_store_n(&s_mt_rows_done, 0, __ATOMIC_RELAXED);
            s_mt_fork_id++;
            __atomic_store_n(&s_mt_grab,
                             (s64)(((u64)s_mt_fork_id << 32) |
                                   ((u64)(u32)nrows << 16)),
                             __ATOMIC_RELEASE);
            __atomic_add_fetch(&s_mt_epoch, 1, __ATOMIC_RELEASE);
            WakeByAddressAll((PVOID)&s_mt_epoch);
            if (s_mt_stats != 1) {
                scan_rows_dynamic(t);
                while (__atomic_load_n(&s_mt_rows_done, __ATOMIC_ACQUIRE) != nrows)
                    _mm_pause();
            } else {
                u64 t1 = __rdtsc();
                scan_rows_dynamic(t);
                u64 t2 = __rdtsc();
                while (__atomic_load_n(&s_mt_rows_done, __ATOMIC_ACQUIRE) != nrows)
                    _mm_pause();
                u64 t3 = __rdtsc();
                s_mt_forks++;
                s_mt_scan_tsc += t2 - t1;
                s_mt_join_tsc += t3 - t2;
                s_mt_fork_tsc += t3 - t0;
            }
        } else {
            if (s_mt_stats == 1) s_mt_serial_tris++;
            scan_block_rows_serial(t, &ts);
        }
        (void)s_triangle_sink(s_triangle_sink_user, &job, 1);
        return;
    }

    /* GX-MT fork gate: enough pixels to amortize a fork/join (measured via
     * [gx-area-hist], see s_mt_min_area), and a draw whose pixel program is
     * provably carry-free (draw_parallel_ok). Everything else scans serially
     * on worker 0 — the exact old loop. */
    if (s_mt_threads > 1 && s_cfg.parallel_ok &&
        (u32)((maxx - minx) * (maxy - miny)) >= s_mt_min_area) {
        u64 t0 = s_mt_stats == 1 ? __rdtsc() : 0;
        for (int i = 1; i < s_mt_threads; i++) {
            s_tev_w[i] = s_tev_w[0];   /* per-draw template: Reg/Konst et al. */
            s_tev_w[i].wid = i;
        }
        s32 nrows = (ts.maxy - ts.block_miny + BLK - 1) / BLK;
        s_mt_job = ts;
        __atomic_store_n(&s_mt_rows_done, 0, __ATOMIC_RELAXED);
        s_mt_fork_id++;
        __atomic_store_n(&s_mt_grab,
                         (s64)(((u64)s_mt_fork_id << 32) | ((u64)(u32)nrows << 16)),
                         __ATOMIC_RELEASE);   /* publishes job + rows_done reset */
        __atomic_add_fetch(&s_mt_epoch, 1, __ATOMIC_RELEASE);
        WakeByAddressAll((PVOID)&s_mt_epoch);
        if (s_mt_stats != 1) {
            scan_rows_dynamic(t);
            while (__atomic_load_n(&s_mt_rows_done, __ATOMIC_ACQUIRE) != nrows)
                _mm_pause();
        } else {
            u64 t1 = __rdtsc();
            scan_rows_dynamic(t);
            u64 t2 = __rdtsc();
            while (__atomic_load_n(&s_mt_rows_done, __ATOMIC_ACQUIRE) != nrows)
                _mm_pause();
            u64 t3 = __rdtsc();
            s_mt_forks++;
            s_mt_scan_tsc += t2 - t1;
            s_mt_join_tsc += t3 - t2;
            s_mt_fork_tsc += t3 - t0;
        }
    } else {
        if (s_mt_stats == 1) s_mt_serial_tris++;
        scan_block_rows_serial(t, &ts);
    }
}

/* GCN_GX_STATS bucket (b) — triangle scan/pixel: draw_triangle_impl covers
 * setup (edge equations, slopes) through every raster_pixel/tev_draw call for
 * this triangle, i.e. everything gx_raster_draw's vertex loop is NOT (see the
 * vtx/tri split comment near the top of the file). A thin wrapper rather than
 * inline timing so draw_triangle_impl's early `return` (degenerate/empty
 * bbox) doesn't need a second accumulation site. */
static void draw_triangle(Tev* t, const OutVtx* v0, const OutVtx* v1, const OutVtx* v2) {
    if (!s_draw_stats) { draw_triangle_impl(t, v0, v1, v2); return; }
    u64 px_before = s_pixels_shaded;
    u64 t0 = __rdtsc();
    draw_triangle_impl(t, v0, v1, v2);
    u64 dt = __rdtsc() - t0;
    s_tsc_tri += dt;
    /* Area histogram (see s_hist_* comment near the top): bucket by
     * log2(post-scissor bbox area), attributing this triangle's wall and
     * shaded-pixel delta to it. area==0 (degenerate/early-return) -> bucket 0. */
    u32 area = s_last_tri_area;
    int b = area ? 32 - __builtin_clz(area) : 0;
    if (b >= GX_AREA_HIST_BUCKETS) b = GX_AREA_HIST_BUCKETS - 1;
    s_hist_tris[b]++;
    s_hist_pixels[b] += s_pixels_shaded - px_before;
    s_hist_tsc[b] += dt;
}

/* ============================================================================
 * Clipper (Clipper.cpp): trivial-reject, cull, clip, perspective-divide, draw.
 * ==========================================================================*/
enum { CLIP_POS_X = 1, CLIP_NEG_X = 2, CLIP_POS_Y = 4, CLIP_NEG_Y = 8, CLIP_POS_Z = 16, CLIP_NEG_Z = 32 };

static int calc_clip_mask(const OutVtx* v) {
    int m = 0;
    float x = v->projectedPosition[0], y = v->projectedPosition[1];
    float z = v->projectedPosition[2], w = v->projectedPosition[3];
    if (w - x < 0) m |= CLIP_POS_X;
    if (x + w < 0) m |= CLIP_NEG_X;
    if (w - y < 0) m |= CLIP_POS_Y;
    if (y + w < 0) m |= CLIP_NEG_Y;
    if (w * z > 0) m |= CLIP_POS_Z;
    if (z + w < 0) m |= CLIP_NEG_Z;
    return m;
}
static void vtx_lerp(OutVtx* o, float t, const OutVtx* a, const OutVtx* b) {
#define LI(OUT, IN) ((OUT) + (((IN) - (OUT)) * t))
    for (int i = 0; i < 3; i++) o->objPos[i] = a->objPos[i];
    o->posMtx = a->posMtx;
    for (int i = 0; i < 3; i++) o->mvPosition[i] = LI(a->mvPosition[i], b->mvPosition[i]);
    for (int i = 0; i < 4; i++) o->projectedPosition[i] = LI(a->projectedPosition[i], b->projectedPosition[i]);
    for (int n = 0; n < 3; n++) for (int i = 0; i < 3; i++) o->normal[n][i] = LI(a->normal[n][i], b->normal[n][i]);
    u16 ti = (u16)(t * 256);
    for (int c = 0; c < 2; c++) for (int i = 0; i < 4; i++)
        o->color[c][i] = (u8)(a->color[c][i] + (((int)b->color[c][i] - (int)a->color[c][i]) * ti >> 8));
    for (int n = 0; n < 8; n++) for (int i = 0; i < 3; i++) o->texCoords[n][i] = LI(a->texCoords[n][i], b->texCoords[n][i]);
#undef LI
}

static float vp_wd(void)  { return xf_f(0x101a); }
static float vp_ht(void)  { return xf_f(0x101b); }
static float vp_zr(void)  { return xf_f(0x101c); }
static float vp_xo(void)  { return xf_f(0x101d); }
static float vp_yo(void)  { return xf_f(0x101e); }
static float vp_fz(void)  { return xf_f(0x101f); }

static void perspective_divide(OutVtx* v) {
    float wInv = 1.0f / v->projectedPosition[3];
    v->screenPosition[0] = v->projectedPosition[0] * wInv * vp_wd() + vp_xo();
    v->screenPosition[1] = v->projectedPosition[1] * wInv * vp_ht() + vp_yo();
    v->screenPosition[2] = v->projectedPosition[2] * wInv * vp_zr() + vp_fz();
}
static int is_backface(const OutVtx* v0, const OutVtx* v1, const OutVtx* v2) {
    float x0 = v0->projectedPosition[0], x1 = v1->projectedPosition[0], x2 = v2->projectedPosition[0];
    float y0 = v0->projectedPosition[1], y1 = v1->projectedPosition[1], y2 = v2->projectedPosition[1];
    float w0 = v0->projectedPosition[3], w1 = v1->projectedPosition[3], w2 = v2->projectedPosition[3];
    float nz = (x0 * w2 - x2 * w0) * y1 + (x2 * y0 - x0 * y2) * w1 + (y2 * w0 - y0 * w2) * x1;
    int backface = nz <= 0.0f;
    if (vp_ht() > 0) backface = !backface;
    return backface;
}

/* Storage for clipped vertices (Clipper.cpp NUM_CLIPPED_VERTICES=33). */
#define NUM_CLIPPED 33
static OutVtx  s_clipped[NUM_CLIPPED];
static OutVtx* s_verts[NUM_CLIPPED + 3];

static float clip_dot(int i, float A, float B, float C, float D) {
    return s_verts[i]->projectedPosition[0] * A + s_verts[i]->projectedPosition[1] * B +
           s_verts[i]->projectedPosition[2] * C + s_verts[i]->projectedPosition[3] * D;
}
static int s_numVertices;
static void add_interp(float t, int out, int in) {
    vtx_lerp(s_verts[s_numVertices++], t, s_verts[out], s_verts[in]);
}
#define DIFF_SIGNS(x, y) (((x) <= 0 && (y) > 0) || ((x) > 0 && (y) <= 0))

static void clip_triangle(int* indices, int* numIndices, int mask) {
    int vlist[2][2 * 6 + 1];
    int* inlist = vlist[0];
    int* outlist = vlist[1];
    int n = 3;
    s_numVertices = 3;
    inlist[0] = 0; inlist[1] = 1; inlist[2] = 2;
    indices[0] = indices[1] = indices[2] = -1;

    static const struct { int bit; float A, B, C, D; } planes[6] = {
        { CLIP_POS_X, -1, 0, 0, 1 }, { CLIP_NEG_X, 1, 0, 0, 1 },
        { CLIP_POS_Y, 0, -1, 0, 1 }, { CLIP_NEG_Y, 0, 1, 0, 1 },
        { CLIP_POS_Z, 0, 0, 0, 1 },  { CLIP_NEG_Z, 0, 0, 1, 1 },
    };
    for (int pl = 0; pl < 6; pl++) {
        if (!(mask & planes[pl].bit)) continue;
        float A = planes[pl].A, B = planes[pl].B, C = planes[pl].C, D = planes[pl].D;
        int idxPrev = inlist[0];
        float dpPrev = clip_dot(idxPrev, A, B, C, D);
        int outcount = 0;
        inlist[n] = inlist[0];
        for (int j = 1; j <= n; j++) {
            int idx = inlist[j];
            float dp = clip_dot(idx, A, B, C, D);
            if (dpPrev >= 0) outlist[outcount++] = idxPrev;
            if (DIFF_SIGNS(dp, dpPrev)) {
                if (dp < 0) { float t = dp / (dp - dpPrev); add_interp(t, idx, idxPrev); }
                else        { float t = dpPrev / (dpPrev - dp); add_interp(t, idxPrev, idx); }
                outlist[outcount++] = s_numVertices - 1;
            }
            idxPrev = idx; dpPrev = dp;
        }
        if (outcount < 3) return;   /* fully clipped (indices left as -1) */
        int* tmp = inlist; inlist = outlist; outlist = tmp; n = outcount;
    }
    indices[0] = inlist[0]; indices[1] = inlist[1]; indices[2] = inlist[2];
    for (int j = 3; j < n; ++j) {
        indices[(*numIndices)++] = inlist[0];
        indices[(*numIndices)++] = inlist[j - 1];
        indices[(*numIndices)++] = inlist[j];
    }
}

static void process_triangle(Tev* t, OutVtx* v0, OutVtx* v1, OutVtx* v2) {
    if (s_debug_pending_index >= 0)
        s_debug_pending.triangles_submitted++;
    int m = calc_clip_mask(v0) & calc_clip_mask(v1) & calc_clip_mask(v2);
    if (m != 0) {
        if (s_debug_pending_index >= 0)
            s_debug_pending.triangles_trivial_rejected++;
        return;   /* trivially rejected */
    }

    int backface = is_backface(v0, v1, v2);
    u32 cull = s_cfg.cullmode;
    if ((!backface && (cull == 1 || cull == 3)) ||
        (backface && (cull == 2 || cull == 3))) {
        if (s_debug_pending_index >= 0)
            s_debug_pending.triangles_culled++;
        return;
    }

    for (int i = 3; i < NUM_CLIPPED + 3; i++) s_verts[i] = &s_clipped[i - 3];
    int indices[NUM_CLIPPED + 3];
    for (int i = 0; i < NUM_CLIPPED + 3; i++) indices[i] = -1;
    indices[0] = 0; indices[1] = 1; indices[2] = 2;
    int numIndices = 3;

    if (backface) { s_verts[0] = v0; s_verts[1] = v2; s_verts[2] = v1; }
    else          { s_verts[0] = v0; s_verts[1] = v1; s_verts[2] = v2; }

    int mask = calc_clip_mask(s_verts[0]) | calc_clip_mask(s_verts[1]) | calc_clip_mask(s_verts[2]);
    if (mask != 0) {
        if (s_debug_pending_index >= 0)
            s_debug_pending.triangles_clipped++;
        clip_triangle(indices, &numIndices, mask);
    }

    for (int i = 0; i + 3 <= numIndices; i += 3) {
        if (indices[i] == -1) continue;
        perspective_divide(s_verts[indices[i]]);
        perspective_divide(s_verts[indices[i + 1]]);
        perspective_divide(s_verts[indices[i + 2]]);
        draw_triangle(t, s_verts[indices[i]], s_verts[indices[i + 1]], s_verts[indices[i + 2]]);
    }
}

/* ============================================================================
 * Line processing (Clipper.cpp ProcessLine/ClipLine/CopyLineVertex). A line
 * is clipped parametrically in clip space, perspective-divided, then expanded
 * into a quad (two CCW triangles) offset ±line_half_width along the MINOR
 * axis — GC's quirky vertical-or-horizontal line caps ("FIXME: what does
 * real hardware do at 45°?" — Dolphin's words; we reproduce Dolphin, the
 * oracle). linesize is in 1/6th-pixel units (LPSize, BP 0x22 bits 0-7), so
 * half-width = linesize/12. The two -px/-py copies get the
 * LINE_PT_TEX_OFFSETS[lineoff] texcoord bump on every texgen whose TexSize
 * s.line_offset bit (BP 0x30+2c bit 18) is set, scaled like tf_texcoord's
 * scale pass. Triangles go straight to draw_triangle — Dolphin calls
 * DrawTriangleFrontFace directly, bypassing cull mode, and draw_triangle is
 * exactly that stage (process_triangle's cull runs before it, not inside). */
static const float s_line_pt_tex_offsets[8] = {
    0, 1 / 16.f, 1 / 8.f, 1 / 4.f, 1 / 2.f, 1, 1, 1,
};

static void copy_line_vertex(OutVtx* dst, const OutVtx* src, int px, int py,
                             int apply_line_offset) {
    const float line_half_width = (float)bits(s_bp[0x22], 0, 8) / 12.0f;
    *dst = *src;
    dst->screenPosition[0] = src->screenPosition[0] + (float)px * line_half_width;
    dst->screenPosition[1] = src->screenPosition[1] + (float)py * line_half_width;
    const u32 lineoff = bits(s_bp[0x22], 16, 3);
    if (apply_line_offset && s_line_pt_tex_offsets[lineoff] != 0) {
        u32 numtexgens = s_xf[0x103f] & 0xf;   /* xfmem.numTexGen, as Clipper reads it */
        for (u32 c = 0; c < numtexgens; c++) {
            if (bits(s_bp[0x30 + c * 2], 18, 1)) {   /* TexSize s.line_offset */
                dst->texCoords[c][0] += (float)(bits(s_bp[0x30 + c * 2], 0, 16) + 1)
                                        * s_line_pt_tex_offsets[lineoff];
            }
        }
    }
}

static void process_line(Tev* t, OutVtx* v0, OutVtx* v1) {
    int m0 = calc_clip_mask(v0), m1 = calc_clip_mask(v1);
    int mask = m0 | m1;

    OutVtx interp0, interp1;
    if (mask) {
        /* ClipLine: accumulate the largest clip parameter per endpoint over
         * every violated plane; reject when both endpoints are outside one
         * plane. Plane table order matches the CLIP_POS_X..CLIP_NEG_Z bit
         * order of calc_clip_mask. */
        static const float planes[6][4] = {
            { -1, 0, 0, 1 }, { 1, 0, 0, 1 },   /* POS_X, NEG_X */
            { 0, -1, 0, 1 }, { 0, 1, 0, 1 },   /* POS_Y, NEG_Y */
            { 0, 0, -1, 1 }, { 0, 0, 1, 1 },   /* POS_Z, NEG_Z */
        };
        float t0 = 0.0f, t1 = 0.0f;
        for (int pl = 0; pl < 6; pl++) {
            if (!(mask & (1 << pl))) continue;
            const float* P = planes[pl];
            float dp0 = v0->projectedPosition[0] * P[0] + v0->projectedPosition[1] * P[1] +
                        v0->projectedPosition[2] * P[2] + v0->projectedPosition[3] * P[3];
            float dp1 = v1->projectedPosition[0] * P[0] + v1->projectedPosition[1] * P[1] +
                        v1->projectedPosition[2] * P[2] + v1->projectedPosition[3] * P[3];
            if (dp0 < 0 && dp1 < 0) return;   /* fully clipped */
            if (dp1 < 0) {
                float tc = dp1 / (dp1 - dp0);
                if (tc > t1) t1 = tc;
            } else if (dp0 < 0) {
                float tc = dp0 / (dp0 - dp1);
                if (tc > t0) t0 = tc;
            }
        }
        /* Both lerps read the ORIGINAL endpoints (Clipper's Vertices[0/1]
         * stay the originals even when indices are remapped). */
        if (m0) vtx_lerp(&interp0, t0, v0, v1);
        if (m1) vtx_lerp(&interp1, t1, v1, v0);
        if (m0) v0 = &interp0;
        if (m1) v1 = &interp1;
    }

    perspective_divide(v0);
    perspective_divide(v1);

    const float dx = v1->screenPosition[0] - v0->screenPosition[0];
    const float dy = v1->screenPosition[1] - v0->screenPosition[1];
    int px = 0, py = 0;
    /* px/py sign choice keeps the two triangles CCW (Clipper's note). */
    if (fabsf(dx) > fabsf(dy)) py = (dx > 0) ? -1 : 1;
    else                       px = (dy > 0) ? 1 : -1;

    OutVtx tri[3];
    copy_line_vertex(&tri[0], v0, px, py, 0);
    copy_line_vertex(&tri[1], v1, px, py, 0);
    copy_line_vertex(&tri[2], v1, -px, -py, 1);
    draw_triangle(t, &tri[2], &tri[1], &tri[0]);   /* ccw winding */
    copy_line_vertex(&tri[1], v0, -px, -py, 1);
    draw_triangle(t, &tri[0], &tri[1], &tri[2]);
}

/* ============================================================================
 * Transform (TransformUnit.cpp).
 * ==========================================================================*/
typedef struct {
    float position[3];
    float normal[3][3];
    u8    color[2][4];         /* [chan][ALP_C,BLU_C,GRN_C,RED_C] (abgr); always
                                * populated: decoded attribute or the routed
                                * missing-color default (opaque white)        */
    float texCoords[8][2];
    u8    posMtx;
    u8    texMtx[8];
} InVtx;

/* The GX vertex loader retains the last submitted normal/NBT values. If a
 * later vertex format omits those attributes, TransformUnit consumes the
 * retained values rather than zero. This is observable in lit draws with no
 * normal field (Dolphin's VertexLoaderManager normal/tangent/binormal caches)
 * and is hardware-facing vertex state, not an HLE title workaround. */
static float s_normal_cache[3][3];

static void mul_vec3_mat34(const float* v, const float* m, float* r) {
    r[0] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2] + m[3];
    r[1] = m[4]*v[0] + m[5]*v[1] + m[6]*v[2] + m[7];
    r[2] = m[8]*v[0] + m[9]*v[1] + m[10]*v[2] + m[11];
}
static void mul_vec3_mat33(const float* v, const float* m, float* r) {
    r[0] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2];
    r[1] = m[3]*v[0] + m[4]*v[1] + m[5]*v[2];
    r[2] = m[6]*v[0] + m[7]*v[1] + m[8]*v[2];
}
static void mul_vec2_mat24(const float* v, const float* m, float* r) {
    r[0] = m[0]*v[0] + m[1]*v[1] + m[2] + m[3];
    r[1] = m[4]*v[0] + m[5]*v[1] + m[6] + m[7];
    r[2] = 1.0f;
}
static void mul_vec3_mat24(const float* v, const float* m, float* r) {
    r[0] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2] + m[3];
    r[1] = m[4]*v[0] + m[5]*v[1] + m[6]*v[2] + m[7];
    r[2] = 1.0f;
}
static void mul_vec2_mat34(const float* v, const float* m, float* r) {
    r[0] = m[0]*v[0] + m[1]*v[1] + m[2] + m[3];
    r[1] = m[4]*v[0] + m[5]*v[1] + m[6] + m[7];
    r[2] = m[8]*v[0] + m[9]*v[1] + m[10] + m[11];
}
static void mul_vec3_mat34_tex(const float* v, const float* m, float* r) { mul_vec3_mat34(v, m, r); }

static void normalize3(float* v) {
    float len = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len > 0.0f) { v[0] /= len; v[1] /= len; v[2] /= len; }
}

/* pos/normal matrix pointers into XF float memory */
static void get_posmat(u8 idx, float* m) { for (int i = 0; i < 12; i++) m[i] = xf_f((u32)idx * 4 + i); }
static void get_normmat(u8 idx, float* m) { for (int i = 0; i < 9; i++) m[i] = xf_f(0x400 + (u32)(idx & 31) * 3 + i); }
static void get_texmat(u8 idx, float* m) { for (int i = 0; i < 12; i++) m[i] = xf_f((u32)idx * 4 + i); }
static void get_postmat(u8 idx, float* m) { for (int i = 0; i < 12; i++) m[i] = xf_f(0x500 + (u32)idx * 4 + i); }

/* [ENHANCEMENT, opt-in] GCN_ASPECT=16:9 / 21:9 — Dolphin-style widescreen
 * hack: scale the PERSPECTIVE projection's X row by (4/3)/target so the
 * frustum captures a wider FOV; the host window (host_window.c, same env)
 * widens the display by the inverse, netting undistorted, wider 3D.
 * Ortho/2D content is deliberately untouched (it takes the display stretch —
 * the classic trade-off). Unset/anything else returns 0 (off) and the
 * projection math below is UNTOUCHED — not even a *1.0f — so the default
 * raster stays bit-identical (golden gates never set GCN_ASPECT). */
static float gx_aspect_persp_xscale(void) {
    static float s_scale = -1.0f;
    if (s_scale < 0.0f) {
        const char* e = getenv("GCN_ASPECT");
        if (e && strcmp(e, "16:9") == 0)      s_scale = (4.0f/3.0f) / (16.0f/9.0f);
        else if (e && strcmp(e, "21:9") == 0) s_scale = (4.0f/3.0f) / (21.0f/9.0f);
        else                                  s_scale = 0.0f;   /* off */
    }
    return s_scale;
}

typedef struct {
    float m[12];
    float p[6];
    u32 ptype;
} PositionTransform;

/* [gx-xfaudit] trigger: the flood signature is the cube DL's draws running
 * under a wall-scale position matrix (|linear|~22; every matrix the guest
 * ever loads for the cubes has |linear|<=2.2 — 10x separation, threshold 3).
 * The instant that happens, dump everything needed to name the break:
 * which slot was read (posMtx + the CP MATINDEX default in force), what that
 * slot holds right now, and the gx.c write-audit rings showing exactly which
 * XF/BP writes decode executed leading up to this draw (with frame + DL
 * provenance). Recording is always-on; only this dump is signature-gated.
 * GCN_GX_XF_AUDIT_DL overrides the watched DL (hex; 0 disables the trigger);
 * the default is the IPL menu's cube DL under investigation. */
static void gx_xf_audit_check(u8 pos_mtx, const float* m) {
    /* Default DISARMED now that the flood bug is fixed (7fb3d69): during
     * menu transitions the cube DL legitimately runs |linear| up to ~2700
     * (zoom sweep), so the trigger burned its dump budget on real matrices
     * and then printed suppressed-hit counters forever. Arm explicitly with
     * GCN_GX_XF_AUDIT_DL=<hex dl> (e.g. AF13C0) for a future hunt; the
     * write-audit RINGS this dumps stay always-on either way. */
    static u32 s_watch_dl = 0xFFFFFFFFu;   /* lazy env sentinel */
    if (s_watch_dl == 0xFFFFFFFFu) {
        const char* e = getenv("GCN_GX_XF_AUDIT_DL");
        s_watch_dl = e ? (u32)strtoul(e, NULL, 16) : 0u;
    }
    if (s_watch_dl == 0u || gcn_gx_current_dl() != s_watch_dl)
        return;
    float maxlin = 0.0f;
    for (int i = 0; i < 12; i++) {
        if ((i & 3) == 3) continue;        /* translation column */
        float a = fabsf(m[i]);
        if (a > maxlin) maxlin = a;
    }
    if (maxlin <= 3.0f)
        return;

    /* Rate limit: floods repeat the bad draw hundreds of times per frame and
     * recur ~1-2/min for the whole soak — 2 full dumps per frame, 16 total,
     * then a suppressed-hit counter so the event rate stays measurable. */
    static u64 s_hits, s_dumps;
    static u64 s_lastframe = ~0ull;
    static u32 s_frame_dumps;
    u64 frame = gcn_gx_frame_count();
    s_hits++;
    if (frame != s_lastframe) { s_lastframe = frame; s_frame_dumps = 0u; }
    if (s_frame_dumps >= 2u || s_dumps >= 16u) {
        if ((s_hits & 255u) == 0u)
            fprintf(stderr, "[gx-xfaudit] %llu hits total (dumps capped, "
                    "latest frame=%llu maxlin=%.4g)\n",
                    (unsigned long long)s_hits, (unsigned long long)frame,
                    (double)maxlin);
        return;
    }
    s_frame_dumps++;
    s_dumps++;

    fprintf(stderr,
            "[gx-xfaudit] HIT #%llu frame=%llu dl=%08X posMtx=%u matidxA=0x%08X "
            "maxlin=%.4g\n",
            (unsigned long long)s_hits, (unsigned long long)frame,
            gcn_gx_current_dl(), pos_mtx,
            s_trap_cp ? s_trap_cp->matrix_index_a : 0u, (double)maxlin);
    for (int r = 0; r < 3; r++)
        fprintf(stderr, "[gx-xfaudit]   applied r%d = %.6g %.6g %.6g | %.6g\n",
                r, (double)m[r*4+0], (double)m[r*4+1], (double)m[r*4+2],
                (double)m[r*4+3]);
    if (pos_mtx != 0u) {
        /* The applied matrix came from slot pos_mtx; show what slot 0 (the
         * per-instance load target) holds RIGHT NOW for the index-leak case. */
        for (int r = 0; r < 3; r++)
            fprintf(stderr, "[gx-xfaudit]   slot0   r%d = %.6g %.6g %.6g | %.6g\n",
                    r, (double)xf_f((u32)r*4+0), (double)xf_f((u32)r*4+1),
                    (double)xf_f((u32)r*4+2), (double)xf_f((u32)r*4+3));
    }
    gcn_gx_state_audit_dump();
}

static void prepare_position_transform(u8 pos_mtx, PositionTransform* tf) {
    get_posmat(pos_mtx, tf->m);
    gx_xf_audit_check(pos_mtx, tf->m);
    tf->ptype = s_xf[0x1026];      /* ProjectionType (0 persp, 1 ortho) */
    for (int i = 0; i < 6; i++) tf->p[i] = xf_f(0x1020 + i);
    if (tf->ptype == 0) {
        float aspect_k = gx_aspect_persp_xscale();
        if (aspect_k > 0.0f) {
            tf->p[0] *= aspect_k;
            tf->p[1] *= aspect_k;
        }
    }
}

static void tf_position_prepared(const InVtx* in, OutVtx* out,
                                 const PositionTransform* tf) {
    const float* m = tf->m;
    const float* p = tf->p;
    out->objPos[0] = in->position[0];
    out->objPos[1] = in->position[1];
    out->objPos[2] = in->position[2];
    out->posMtx = in->posMtx;
    mul_vec3_mat34(in->position, m, out->mvPosition);
    if (tf->ptype == 0) {          /* Perspective (MultipleVec3Perspective) */
        out->projectedPosition[0] = p[0]*out->mvPosition[0] + p[1]*out->mvPosition[2];
        out->projectedPosition[1] = p[2]*out->mvPosition[1] + p[3]*out->mvPosition[2];
        out->projectedPosition[2] = (p[4]*out->mvPosition[2] + p[5]) * (1.0f - 1e-7f);
        out->projectedPosition[3] = -out->mvPosition[2];
    } else {                       /* Orthographic */
        out->projectedPosition[0] = p[0]*out->mvPosition[0] + p[1];
        out->projectedPosition[1] = p[2]*out->mvPosition[1] + p[3];
        out->projectedPosition[2] = p[4]*out->mvPosition[2] + p[5];
        out->projectedPosition[3] = 1.0f;
    }
}

static void tf_position(const InVtx* in, OutVtx* out) {
    PositionTransform tf;
    prepare_position_transform(in->posMtx, &tf);
    tf_position_prepared(in, out, &tf);
}
static void tf_normal(const InVtx* in, OutVtx* out) {
    float m[9]; get_normmat(in->posMtx, m);
    mul_vec3_mat33(in->normal[0], m, out->normal[0]);
    mul_vec3_mat33(in->normal[1], m, out->normal[1]);
    mul_vec3_mat33(in->normal[2], m, out->normal[2]);
    normalize3(out->normal[0]);
}

/* Lighting (TransformUnit.cpp:204-314). One light per channel is in scope. */
static float dot3(const float* a, const float* b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
static void light_ptr(int n, u8 col[4], float cosatt[3], float distatt[3], float pos[3], float dir[3]) {
    u32 base = 0x600 + (u32)n * 16;
    u32 c = s_xf[base + 3];
    /* Light.color[4] overlays the host-order xfmem word (TransformUnit.cpp:194-
     * 202): color[0]=byte0=alpha, color[1]=blue, color[2]=green, color[3]=red
     * (word = R<<24|G<<16|B<<8|A). AddScaledIntegerColor uses color[1..3]. */
    col[0] = (u8)c; col[1] = (u8)(c >> 8); col[2] = (u8)(c >> 16); col[3] = (u8)(c >> 24);
    for (int i = 0; i < 3; i++) cosatt[i]  = xf_f(base + 4 + i);
    for (int i = 0; i < 3; i++) distatt[i] = xf_f(base + 7 + i);
    for (int i = 0; i < 3; i++) pos[i]     = xf_f(base + 10 + i);
    for (int i = 0; i < 3; i++) dir[i]     = xf_f(base + 13 + i);
}
static float safe_div(float n, float d) { return (d == 0) ? (n > 0 ? 1 : 0) : n / d; }
static float calc_light_attn(const float* lcol_dir, float* ldir, const float* normal,
                             u32 attnfunc, u32 diffusefunc,
                             const float* cosatt, const float* distatt, const float* ldir_light) {
    float attn = 1.0f;
    (void)lcol_dir;
    switch (attnfunc) {
    case 0: case 2: {   /* None / Dir */
        normalize3(ldir);
        if (ldir[0] == 0 && ldir[1] == 0 && ldir[2] == 0) { ldir[0]=normal[0]; ldir[1]=normal[1]; ldir[2]=normal[2]; }
        break;
    }
    case 1: {           /* Spec */
        normalize3(ldir);
        attn = dot3(ldir, normal) >= 0.0f ? (dot3(ldir_light, normal) > 0 ? dot3(ldir_light, normal) : 0) : 0;
        float attLen[3] = { 1.0f, attn, attn*attn };
        float cA = cosatt[0]*attLen[0] + cosatt[1]*attLen[1] + cosatt[2]*attLen[2];
        float dA;
        if (diffusefunc != 0) {
            float d[3] = { distatt[0], distatt[1], distatt[2] }; normalize3(d);
            dA = d[0]*attLen[0] + d[1]*attLen[1] + d[2]*attLen[2];
        } else {
            dA = distatt[0]*attLen[0] + distatt[1]*attLen[1] + distatt[2]*attLen[2];
        }
        attn = safe_div(cA > 0 ? cA : 0, dA);
        break;
    }
    case 3: {           /* Spot */
        float dist2 = dot3(ldir, ldir);
        float dist = sqrtf(dist2);
        ldir[0] /= dist; ldir[1] /= dist; ldir[2] /= dist;
        attn = dot3(ldir, ldir_light); if (attn < 0) attn = 0;
        float cA = cosatt[0] + cosatt[1]*attn + cosatt[2]*attn*attn;
        float dA = distatt[0] + distatt[1]*dist + distatt[2]*dist2;
        attn = safe_div(cA > 0 ? cA : 0, dA);
        break;
    }
    default: TRAP(attn, "invalid attnfunc"); break;
    }
    return attn;
}
static void light_color(const float* pos, const float* normal, int lnum, u32 diffusefunc,
                        u32 attnfunc, float* lightCol) {
    u8 col[4]; float cosatt[3], distatt[3], lpos[3], ldirL[3];
    light_ptr(lnum, col, cosatt, distatt, lpos, ldirL);
    float ldir[3] = { lpos[0]-pos[0], lpos[1]-pos[1], lpos[2]-pos[2] };
    float attn = calc_light_attn(NULL, ldir, normal, attnfunc, diffusefunc, cosatt, distatt, ldirL);
    float difAttn = dot3(ldir, normal);
    switch (diffusefunc) {
    case 0: lightCol[0]+=col[1]*attn; lightCol[1]+=col[2]*attn; lightCol[2]+=col[3]*attn; break;
    case 1: lightCol[0]+=col[1]*attn*difAttn; lightCol[1]+=col[2]*attn*difAttn; lightCol[2]+=col[3]*attn*difAttn; break;
    default: if (difAttn < 0) difAttn = 0;
             lightCol[0]+=col[1]*attn*difAttn; lightCol[1]+=col[2]*attn*difAttn; lightCol[2]+=col[3]*attn*difAttn; break;
    }
}
static void light_alpha(const float* pos, const float* normal, int lnum, u32 diffusefunc,
                        u32 attnfunc, float* lightCol) {
    u8 col[4]; float cosatt[3], distatt[3], lpos[3], ldirL[3];
    light_ptr(lnum, col, cosatt, distatt, lpos, ldirL);
    float ldir[3] = { lpos[0]-pos[0], lpos[1]-pos[1], lpos[2]-pos[2] };
    float attn = calc_light_attn(NULL, ldir, normal, attnfunc, diffusefunc, cosatt, distatt, ldirL);
    float difAttn = dot3(ldir, normal);
    switch (diffusefunc) {
    case 0: *lightCol += col[0]*attn; break;
    case 1: *lightCol += col[0]*attn*difAttn; break;
    default: if (difAttn < 0) difAttn = 0; *lightCol += col[0]*attn*difAttn; break;
    }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void tf_color_n(const InVtx* in, OutVtx* out, u32 channels) {
    for (u32 chan = 0; chan < channels; chan++) {
        u32 colorreg = s_xf[0x100e + chan];   /* LitChannel color */
        u32 alphareg = s_xf[0x1010 + chan];   /* LitChannel alpha */
        /* matcolor as [R,G,B,A]-ish; the SW backend treats matcolor as
         * [A,B,G,R]? Actually indices [1..3]=B,G,R, [0]=A. matColor reg word. */
        u32 matc = s_xf[0x100c + chan];
        u8 matcolor[4];   /* index: 0=A,1=B,2=G,3=R (abgr) */
        matcolor[0] = (u8)matc; matcolor[1] = (u8)(matc >> 8);
        matcolor[2] = (u8)(matc >> 16); matcolor[3] = (u8)(matc >> 24);
        /* TransformUnit.cpp TransformColor: matsource==Vertex takes matcolor
         * straight from the vertex color (in->color[chan], already in
         * [ALP_C,BLU_C,GRN_C,RED_C]=[0,1,2,3] order and ALWAYS populated by
         * load_vertex — decoded attribute or the missing-color default);
         * Register keeps the XF material color already loaded. Only
         * matcolor[1..3] (B,G,R) come from the COLOR matsource bit —
         * matcolor[0] (A) is decided independently by the ALPHA matsource bit
         * below, exactly mirroring Dolphin's two separate matsource checks. */
        if (bits(colorreg, 0, 1) != 0) {
            matcolor[1] = in->color[chan][1];
            matcolor[2] = in->color[chan][2];
            matcolor[3] = in->color[chan][3];
        }

        u8 chancolor[4];
        if (bits(colorreg, 1, 1)) {   /* enablelighting */
            float lightCol[3];
            if (bits(colorreg, 6, 1)) {   /* ambsource == Vertex */
                lightCol[0] = (float)in->color[chan][1];
                lightCol[1] = (float)in->color[chan][2];
                lightCol[2] = (float)in->color[chan][3];
            } else {
                u32 amb = s_xf[0x100a + chan];
                lightCol[0] = (float)((amb >> 8) & 0xff);   /* b? follows abgr idx1..3 */
                lightCol[1] = (float)((amb >> 16) & 0xff);
                lightCol[2] = (float)((amb >> 24) & 0xff);
            }
            u32 mask = bits(colorreg, 1, 1) ? (bits(colorreg, 2, 4) | (bits(colorreg, 11, 4) << 4)) : 0;
            u32 diffusefunc = bits(colorreg, 7, 2), attnfunc = bits(colorreg, 9, 2);
            for (int i = 0; i < 8; i++)
                if (mask & (1 << i)) light_color(out->mvPosition, out->normal[0], i, diffusefunc, attnfunc, lightCol);
            int lx = clampi((int)lightCol[0], 0, 255);
            int ly = clampi((int)lightCol[1], 0, 255);
            int lz = clampi((int)lightCol[2], 0, 255);
            chancolor[1] = (u8)((matcolor[1] * (lx + (lx >> 7))) >> 8);
            chancolor[2] = (u8)((matcolor[2] * (ly + (ly >> 7))) >> 8);
            chancolor[3] = (u8)((matcolor[3] * (lz + (lz >> 7))) >> 8);
        } else {
            chancolor[1] = matcolor[1]; chancolor[2] = matcolor[2]; chancolor[3] = matcolor[3];
        }

        /* alpha matsource (TransformColor:369-372): Vertex takes the vertex
         * color's alpha (populated or missing-color default), Register the XF
         * material alpha. */
        u8 mata = bits(alphareg, 0, 1) ? in->color[chan][0] : (u8)matc;
        if (bits(alphareg, 1, 1)) {   /* alpha lighting */
            float la;
            if (bits(alphareg, 6, 1)) la = (float)in->color[chan][0];
            else la = (float)(s_xf[0x100a + chan] & 0xff);
            u32 mask = bits(alphareg, 2, 4) | (bits(alphareg, 11, 4) << 4);
            u32 diffusefunc = bits(alphareg, 7, 2), attnfunc = bits(alphareg, 9, 2);
            for (int i = 0; i < 8; i++)
                if (mask & (1 << i)) light_alpha(out->mvPosition, out->normal[0], i, diffusefunc, attnfunc, &la);
            int a = clampi((int)la, 0, 255);
            chancolor[0] = (u8)((mata * (a + (a >> 7))) >> 8);
        } else {
            chancolor[0] = mata;
        }

        /* chancolor is abgr (0=A,1=B,2=G,3=R); store out as [R,G,B,A]. */
        out->color[chan][0] = chancolor[3];   /* R */
        out->color[chan][1] = chancolor[2];   /* G */
        out->color[chan][2] = chancolor[1];   /* B */
        out->color[chan][3] = chancolor[0];   /* A */
    }
}

static void tf_color(const InVtx* in, OutVtx* out) {
    tf_color_n(in, out, 2u);
}

static void tf_texcoord(const InVtx* in, OutVtx* out) {
    u32 numtexgens = s_xf[0x103f] & 0xf;
    int dualtex = bits(s_xf[0x1012], 0, 1);
    for (u32 c = 0; c < numtexgens; c++) {
        u32 info = s_xf[0x1040 + c];
        u32 texgentype = bits(info, 4, 3);
        u32 sourcerow = bits(info, 7, 5);
        u32 projection = bits(info, 1, 1);   /* TexSize: 0 ST, 1 STQ */
        u32 inputform = bits(info, 2, 1);    /* 0 AB11, 1 ABC1 */
        float* dst = out->texCoords[c];
        if (texgentype == 1u) {                         /* EmbossMap */
            u32 source = bits(info, 12, 3);
            u32 light = bits(info, 15, 3);
            if (source >= c) {
                TRAP(embosssrc, "emboss source texgen is not earlier");
                dst[0] = dst[1] = dst[2] = 0.0f;
            } else {
                u8 col[4]; float cosatt[3], distatt[3], lpos[3], ldirL[3];
                light_ptr((int)light, col, cosatt, distatt, lpos, ldirL);
                float ldir[3] = {
                    lpos[0] - out->mvPosition[0],
                    lpos[1] - out->mvPosition[1],
                    lpos[2] - out->mvPosition[2]
                };
                normalize3(ldir);
                dst[0] = out->texCoords[source][0] + dot3(ldir, out->normal[1]);
                dst[1] = out->texCoords[source][1] + dot3(ldir, out->normal[2]);
                dst[2] = out->texCoords[source][2];
            }
        } else if (texgentype == 2u || texgentype == 3u) { /* Color0/Color1 */
            u32 chan = texgentype - 2u;
            dst[0] = (float)out->color[chan][0] * (1.0f / 255.0f);
            dst[1] = (float)out->color[chan][1] * (1.0f / 255.0f);
            dst[2] = 1.0f;
        } else if (texgentype == 0u) {
            float src[3];
            if (sourcerow == 0) { src[0]=in->position[0]; src[1]=in->position[1]; src[2]=in->position[2]; }
            else if (sourcerow == 1) { src[0]=out->normal[0][0]; src[1]=out->normal[0][1]; src[2]=out->normal[0][2]; }
            else if (sourcerow >= 5 && sourcerow <= 12) {
                u32 tn = sourcerow - 5;
                src[0] = in->texCoords[tn][0]; src[1] = in->texCoords[tn][1]; src[2] = 1.0f;
            } else { TRAP(texsrcrow, "tex source row (binormal)"); src[0]=src[1]=0; src[2]=1; }

            float m[12]; get_texmat(in->texMtx[c], m);
            if (projection == 0) {   /* ST */
                if (inputform == 0) mul_vec2_mat24(src, m, dst);
                else                mul_vec3_mat24(src, m, dst);
            } else {                 /* STQ */
                if (inputform == 0) mul_vec2_mat34(src, m, dst);
                else                mul_vec3_mat34_tex(src, m, dst);
            }
        } else {
            TRAPF(texgentype, "invalid texgen type %u", texgentype);
            dst[0] = dst[1] = dst[2] = 0.0f;
        }
        if (dualtex && texgentype == 0u) {
            u32 pinfo = s_xf[0x1050 + c];
            u32 pidx = bits(pinfo, 0, 6);
            int norm = bits(pinfo, 8, 1);
            float tmp[3];
            if (norm) { tmp[0]=dst[0]; tmp[1]=dst[1]; tmp[2]=dst[2]; normalize3(tmp); }
            else { tmp[0]=dst[0]; tmp[1]=dst[1]; tmp[2]=dst[2]; }
            float pm[12]; get_postmat((u8)pidx, pm);
            mul_vec3_mat34(tmp, pm, dst);
        }
        if (texgentype == 0u && dst[2] == 0.0f) {
            float x = dst[0] / 2.0f, y = dst[1] / 2.0f;
            dst[0] = x < -1 ? -1 : x > 1 ? 1 : x;
            dst[1] = y < -1 ? -1 : y > 1 ? 1 : y;
        }
    }
    /* texcoord scale (TransformUnit.cpp:445-449): *= texcoords[i].{s,t}.scale_minus_1+1 */
    for (u32 c = 0; c < numtexgens; c++) {
        u32 s_scale = bits(s_bp[0x30 + c * 2], 0, 16) + 1;
        u32 t_scale = bits(s_bp[0x31 + c * 2], 0, 16) + 1;
        out->texCoords[c][0] *= (float)s_scale;
        out->texCoords[c][1] *= (float)t_scale;
    }
}

/* ============================================================================
 * Vertex loader — parse the display-list vertex payload into InVtx, honoring
 * the recorded indexed formats (Position float / Normal short / TexCoord0
 * short). Direct attrs, per-vertex color, and unexpected formats trap.
 * ==========================================================================*/
enum { VCF_NONE = 0, VCF_DIRECT = 1, VCF_INDEX8 = 2, VCF_INDEX16 = 3 };

/* returns index value + advances *off; width 1 or 2 bytes. */
static u32 read_index(const u8* v, u32* off, u32 type) {
    if (type == VCF_INDEX8)  { u32 i = v[*off]; *off += 1; return i; }
    else                     { u32 i = be_u16(&v[*off]); *off += 2; return i; }
}
static const u8* array_ptr(const GxCpState* cp, u32 arr, u32 index, u32 need) {
    u32 stride = cp->array_strides[arr];
    u32 base = cp->array_bases[arr] & 0x1FFFFFFFu;
    u64 addr = (u64)base + (u64)index * stride;
    if (!s_cpu || !s_cpu->ram || addr + need > (u64)s_cpu->ram_size) return NULL;
    return s_cpu->ram + addr;
}

/* Position-only loader for the late IPL menu's tiny R/S fans.  Those streams
 * have no per-vertex matrix indices and use indexed float positions as their
 * first attribute.  Keep the checks explicit: if the stream ever changes,
 * gx_raster_draw_impl falls back to the complete vertex loader below. */
static int load_indexed_float_position_only(const GxCpState* cp, u32 vat,
                                             const u8* v, u32 vstride,
                                             InVtx* out) {
    u32 low = cp->vtx_desc_lo;
    u32 g0 = cp->vat_g0[vat];
    u32 postype = (low >> 9) & 3u;
    u32 posfmt = (g0 >> 1) & 7u;
    u32 poselem = g0 & 1u;
    u32 index_bytes;

    if ((low & 0x1ffu) != 0u || posfmt != 4u)
        return 0;
    if (postype == VCF_INDEX8)
        index_bytes = 1u;
    else if (postype == VCF_INDEX16)
        index_bytes = 2u;
    else
        return 0;
    if (vstride < index_bytes)
        return 0;

    u32 idx = postype == VCF_INDEX8 ? (u32)v[0] : (u32)be_u16(v);
    u32 n = poselem ? 3u : 2u;
    const u8* p = array_ptr(cp, 0, idx, n * 4u);
    if (!p)
        return 0;

    out->posMtx = (u8)bits(cp->matrix_index_a, 0, 6);
    out->position[0] = be_f32(p);
    out->position[1] = be_f32(p + 4);
    out->position[2] = n == 3u ? be_f32(p + 8) : 0.0f;
    return 1;
}

/* Direct-attribute component read (VertexLoader_{Position,TextCoord}.cpp
 * Pos_ReadDirect<T>/TexCoord_ReadDirect<T>): raw value in `fmt`'s ComponentFormat
 * (0 u8 / 1 s8 / 2 u16 / 3 s16 / 4..7 float), advances *off by GetElementSize
 * (CPMemory.h:142-161: 0/1->1, 2/3->2, 4..7->4). Returns the RAW value —
 * PosScale/TCScale's dequant multiply (applied to every non-float format,
 * skipped for float) is the caller's job, same as the indexed paths below. */
static float read_direct_elem(const u8* v, u32* off, u32 fmt) {
    switch (fmt & 7u) {
    case 0: { u32 x = v[*off]; *off += 1; return (float)x; }             /* u8  */
    case 1: { s8  x = (s8)v[*off]; *off += 1; return (float)x; }         /* s8  */
    case 2: { u32 x = be_u16(&v[*off]); *off += 2; return (float)x; }    /* u16 */
    case 3: { s16 x = be_s16(&v[*off]); *off += 2; return (float)x; }    /* s16 */
    default: { float x = be_f32(&v[*off]); *off += 4; return x; }        /* float family */
    }
}

/* Texture-coordinate VAT fields straddle VAT g0/g1/g2. */
static void texcoord_vat_fields(const GxCpState* cp, u32 vat, u32 tc,
                                u32* elem, u32* fmt, u32* frac) {
    u32 g0 = cp->vat_g0[vat], g1 = cp->vat_g1[vat], g2 = cp->vat_g2[vat];
    switch (tc) {
    case 0: *elem = bits(g0, 21, 1); *fmt = bits(g0, 22, 3); *frac = bits(g0, 25, 5); break;
    case 1: *elem = bits(g1,  0, 1); *fmt = bits(g1,  1, 3); *frac = bits(g1,  4, 5); break;
    case 2: *elem = bits(g1,  9, 1); *fmt = bits(g1, 10, 3); *frac = bits(g1, 13, 5); break;
    case 3: *elem = bits(g1, 18, 1); *fmt = bits(g1, 19, 3); *frac = bits(g1, 22, 5); break;
    case 4: *elem = bits(g1, 27, 1); *fmt = bits(g1, 28, 3); *frac = bits(g2,  0, 5); break;
    case 5: *elem = bits(g2,  5, 1); *fmt = bits(g2,  6, 3); *frac = bits(g2,  9, 5); break;
    case 6: *elem = bits(g2, 14, 1); *fmt = bits(g2, 15, 3); *frac = bits(g2, 18, 5); break;
    default:
        *elem = bits(g2, 23, 1); *fmt = bits(g2, 24, 3); *frac = bits(g2, 27, 5); break;
    }
}

/* Color0/Color1 vertex attribute (VertexLoader_Color.cpp SetCol/SetCol565/
 * SetCol4444/SetCol6666 + Read24/Read32; CPMemory.h ColorFormat 0..5). Every
 * format decodes to full 8-bit R/G/B/A (sub-8-bit channels expanded via the
 * standard N->8 bit replication) and is written out[ALP_C,BLU_C,GRN_C,RED_C] —
 * the exact order TransformUnit.cpp TransformColor reads a vertex color in
 * ("// abgr"), matching the material-color path already used above (matcolor
 * index 0=A,1=B,2=G,3=R). Byte widths (VertexLoader_Color.h s_table_size Direct
 * row): 565/4444 = 2, 888/6666 = 3, 888x/8888 = 4. */
enum { CFMT_RGB565 = 0, CFMT_RGB888 = 1, CFMT_RGB888X = 2, CFMT_RGBA4444 = 3,
       CFMT_RGBA6666 = 4, CFMT_RGBA8888 = 5 };

static u32 color_fmt_bytes(u32 cfmt) {
    static const u32 w[6] = { 2, 3, 4, 2, 3, 4 };
    return (cfmt <= 5u) ? w[cfmt] : 0u;
}
static void decode_color_565(const u8* p, u8 out[4]) {
    u16 val = be_u16(p);
    u8 r5 = (u8)((val >> 11) & 0x1Fu), g6 = (u8)((val >> 5) & 0x3Fu), b5 = (u8)(val & 0x1Fu);
    out[ALP_C] = 0xFFu; out[RED_C] = Convert5To8(r5); out[GRN_C] = Convert6To8(g6);
    out[BLU_C] = Convert5To8(b5);
}
/* 888/888x direct source bytes are already R,G,B in transmission order (the
 * same big-endian-bytes-as-is convention SWEfbInterface/matColor rely on);
 * 888x consumes one extra padding byte (caller advances via color_fmt_bytes). */
static void decode_color_888(const u8* p, u8 out[4]) {
    out[ALP_C] = 0xFFu; out[RED_C] = p[0]; out[GRN_C] = p[1]; out[BLU_C] = p[2];
}
static void decode_color_4444(const u8* p, u8 out[4]) {
    u16 val = be_u16(p);   /* nibbles (MSB->LSB): B[15:12] A[11:8] R[7:4] G[3:0] */
    out[ALP_C] = Convert4To8((u8)((val >> 8) & 0xFu));
    out[RED_C] = Convert4To8((u8)((val >> 4) & 0xFu));
    out[GRN_C] = Convert4To8((u8)(val & 0xFu));
    out[BLU_C] = Convert4To8((u8)((val >> 12) & 0xFu));
}
static void decode_color_6666(const u8* p, u8 out[4]) {
    u32 val = ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];  /* RRRRRRGG GGGGBBBB BBAAAAAA */
    out[RED_C] = Convert6To8((u8)((val >> 18) & 0x3Fu));
    out[GRN_C] = Convert6To8((u8)((val >> 12) & 0x3Fu));
    out[BLU_C] = Convert6To8((u8)((val >> 6) & 0x3Fu));
    out[ALP_C] = Convert6To8((u8)(val & 0x3Fu));
}
static void decode_color_8888(const u8* p, u8 out[4]) {
    out[RED_C] = p[0]; out[GRN_C] = p[1]; out[BLU_C] = p[2]; out[ALP_C] = p[3];
}
static void decode_color_by_fmt(u32 cfmt, const u8* p, u8 out[4]) {
    switch (cfmt) {
    case CFMT_RGB565:   decode_color_565(p, out);   break;
    case CFMT_RGB888:   decode_color_888(p, out);   break;
    case CFMT_RGB888X:  decode_color_888(p, out);   break;
    case CFMT_RGBA4444: decode_color_4444(p, out);  break;
    case CFMT_RGBA6666: decode_color_6666(p, out);  break;
    default:             decode_color_8888(p, out); break;  /* CFMT_RGBA8888 */
    }
}

static int load_vertex(const GxCpState* cp, u32 vat, const u8* v, u32 vstride, InVtx* out) {
    u32 low = cp->vtx_desc_lo, high = cp->vtx_desc_hi;
    u32 g0 = cp->vat_g0[vat];
    u32 off = 0;
    memset(out, 0, sizeof *out);
    memcpy(out->normal, s_normal_cache, sizeof out->normal);

    /* matrix indices (bits 0-8) — none present in the IPL frame. */
    if (low & 1) { out->posMtx = v[off++]; }
    else { out->posMtx = (u8)bits(cp->matrix_index_a, 0, 6); }
    for (int tt = 0; tt < 8; tt++) {
        if ((low >> (1 + tt)) & 1) out->texMtx[tt] = v[off++];
        else if (tt < 4) out->texMtx[tt] = (u8)bits(cp->matrix_index_a, 6 + tt * 6, 6);
        else out->texMtx[tt] = (u8)bits(cp->matrix_index_b, (tt - 4) * 6, 6);
    }

    /* position: Direct (VertexLoader_Position.cpp Pos_ReadDirect) or Index
     * (Pos_ReadIndex) — float XYZ is the only indexed case exercised so far;
     * Direct short/byte formats are dequantized by PosScale = 1/2^PosFrac
     * (VertexLoader.cpp:73), applied to every non-float format alike. */
    u32 postype = (low >> 9) & 3;
    u32 posfmt = (g0 >> 1) & 7, poselem = g0 & 1;
    u32 posfrac = (g0 >> 4) & 0x1f;
    if (postype == VCF_NONE) { TRAP(nopos, "vertex without position"); return 0; }
    if (postype == VCF_DIRECT) {
        u32 n = poselem ? 3 : 2;
        float scale = ((posfmt & 7u) < 4u) ? 1.0f / (float)(1u << posfrac) : 1.0f;
        float p[3] = {0.0f, 0.0f, 0.0f};
        for (u32 i = 0; i < n; i++) p[i] = read_direct_elem(v, &off, posfmt) * scale;
        out->position[0] = p[0]; out->position[1] = p[1]; out->position[2] = p[2];
    } else {
        u32 idx = read_index(v, &off, postype);
        u32 n = poselem ? 3 : 2;
        u32 esize = posfmt < 2u ? 1u : posfmt < 4u ? 2u : 4u;
        float scale = posfmt < 4u ? 1.0f / (float)(1u << posfrac) : 1.0f;
        const u8* p = array_ptr(cp, 0, idx, n * esize);
        if (!p) { TRAP(posoob, "position array out of MEM1"); return 0; }
        u32 poff = 0;
        for (u32 i = 0; i < n; i++)
            out->position[i] = read_direct_elem(p, &poff, posfmt) * scale;
    }

    /* normal (VertexLoader_Normal.cpp): Direct or Index8/16, every component
     * format — FracAdjust is FIXED per width (byte: 6 fraction bits, short:
     * 14; float raw), NOT the VAT frac field, unlike position/texcoord.
     * NormalElements (VAT g0 bit 9) selects N (1 group of 3) vs NBT (3
     * groups -> out->normal[0..2]); NormalIndex3 (VAT g0 bit 31) gives each
     * NBT group its OWN index, with group i's data at byte offset i*3*elem
     * from its indexed element (VertexLoader_Normal's ReadIndexedNormal
     * offset) — the single-index NBT layout is the same 3 consecutive
     * groups read from one element. */
    u32 normtype = (low >> 11) & 3;
    if (normtype != VCF_NONE) {
        u32 normfmt = (g0 >> 10) & 7, normelem = (g0 >> 9) & 1;
        u32 nidx3 = (g0 >> 31) & 1;
        u32 groups = normelem ? 3u : 1u;
        u32 esize = (normfmt < 2u) ? 1u : (normfmt < 4u) ? 2u : 4u;
        float scale = (normfmt < 2u) ? (1.0f / 64.0f)
                    : (normfmt < 4u) ? (1.0f / 16384.0f) : 1.0f;
        if (normtype == VCF_DIRECT) {
            for (u32 gI = 0; gI < groups; gI++)
                for (u32 c = 0; c < 3; c++)
                    out->normal[gI][c] = read_direct_elem(v, &off, normfmt) * scale;
        } else if (nidx3 && groups == 3u) {
            for (u32 gI = 0; gI < 3u; gI++) {
                u32 idx = read_index(v, &off, normtype);
                const u8* p = array_ptr(cp, 1, idx, (gI + 1u) * 3u * esize);
                if (!p) { TRAP(normoob, "normal array out of MEM1"); return 0; }
                p += gI * 3u * esize;
                for (u32 c = 0; c < 3; c++) {
                    u32 o = c * esize;
                    out->normal[gI][c] = read_direct_elem(p, &o, normfmt) * scale;
                }
            }
        } else {
            u32 idx = read_index(v, &off, normtype);
            const u8* p = array_ptr(cp, 1, idx, groups * 3u * esize);
            if (!p) { TRAP(normoob, "normal array out of MEM1"); return 0; }
            for (u32 gI = 0; gI < groups; gI++)
                for (u32 c = 0; c < 3; c++) {
                    u32 o = (gI * 3u + c) * esize;
                    out->normal[gI][c] = read_direct_elem(p, &o, normfmt) * scale;
                }
        }
        for (u32 gI = 0; gI < groups; ++gI)
            memcpy(s_normal_cache[gI], out->normal[gI],
                   sizeof s_normal_cache[gI]);
    }

    /* color0/color1 (VertexLoader_Color.cpp): Direct or Index8/16, all 6 GX
     * component formats — decoded as [ALP_C,BLU_C,GRN_C,RED_C] (see
     * decode_color_by_fmt above). Attribute bytes are consumed in VCD order,
     * then routed per ParseColorAttributes (SWVertexLoader.cpp:164-192): a
     * lone enabled attribute always feeds CHANNEL 0, and any channel without
     * an attribute gets the "missing color" default — opaque white, Dolphin's
     * GFX_HACK_MISSING_COLOR_VALUE default 0xFFFFFFFF (GraphicsSettings.cpp:
     * 211-212; set_default_color unpacks it to A=B=G=R=0xFF). This makes
     * matsource=Vertex WITHOUT a vertex color attribute defined behavior
     * (matching the oracle exactly), not a trap. */
    {
        u8  col_attr[2][4];
        int col_en[2];
        for (int ch = 0; ch < 2; ch++) {
            u32 ctype = (low >> (13 + ch * 2)) & 3u;
            col_en[ch] = (ctype != VCF_NONE);
            if (ctype == VCF_NONE) continue;
            u32 cfmt = (g0 >> (14 + ch * 4)) & 7u;   /* Color0Comp bits14-16 / Color1Comp bits18-20 */
            if (cfmt > 5u) {
                TRAPF(colorfmt, "color%d format %u > RGBA8888(5) (vat %u vcd_lo=0x%08X vat_g0=0x%08X)",
                      ch, cfmt, vat, low, g0);
                return 0;
            }
            u32 w = color_fmt_bytes(cfmt);
            if (ctype == VCF_DIRECT) {
                decode_color_by_fmt(cfmt, &v[off], col_attr[ch]);
                off += w;
            } else {
                u32 idx = read_index(v, &off, ctype);
                const u8* p = array_ptr(cp, 2u + (u32)ch, idx, w);   /* CPArray::Color0=2/Color1=3 */
                if (!p) {
                    TRAPF(coloroob, "color%d array out of MEM1 (idx %u, fmt %u)", ch, idx, cfmt);
                    return 0;
                }
                decode_color_by_fmt(cfmt, p, col_attr[ch]);
            }
        }
        memset(out->color, 0xFF, sizeof out->color);   /* missing-color default */
        if (col_en[0]) {
            memcpy(out->color[0], col_attr[0], 4);
            if (col_en[1]) memcpy(out->color[1], col_attr[1], 4);
        } else if (col_en[1]) {
            memcpy(out->color[0], col_attr[1], 4);     /* lone attribute -> chan 0 */
        }
    }

    /* tex coords: Direct (TexCoord_ReadDirect) or Index (TexCoord_ReadIndex),
     * both scaled by TCScale = 1/2^frac (VertexLoader_TextCoord.cpp), applied
     * to every non-float format alike; float passes through unscaled. */
    for (int tc = 0; tc < 8; tc++) {
        u32 tctype = (high >> (2 * tc)) & 3;
        if (tctype == VCF_NONE) continue;
        u32 fmt, elem, frac;
        texcoord_vat_fields(cp, vat, (u32)tc, &elem, &fmt, &frac);
        u32 n = elem ? 2 : 1;
        float scale = ((fmt & 7u) < 4u) ? 1.0f / (float)(1u << frac) : 1.0f;
        if (tctype == VCF_DIRECT) {
            float t[2] = {0.0f, 0.0f};
            for (u32 i = 0; i < n; i++) t[i] = read_direct_elem(v, &off, fmt) * scale;
            out->texCoords[tc][0] = t[0]; out->texCoords[tc][1] = t[1];
        } else {
            u32 idx = read_index(v, &off, tctype);
            u32 esize = fmt < 2u ? 1u : fmt < 4u ? 2u : 4u;
            const u8* p = array_ptr(cp, 4 + tc, idx, n * esize);
            if (!p) { TRAP(tcoob, "texcoord array out of MEM1"); return 0; }
            u32 poff = 0;
            for (u32 i = 0; i < n; i++)
                out->texCoords[tc][i] = read_direct_elem(p, &poff, fmt) * scale;
        }
    }

    (void)vstride;
    return 1;
}

/* Complete the position-only R/S vertex without re-running the general VCD /
 * VAT parser.  Both exact programs use VAT0=50F76C09: one indexed XYZ float
 * position, one indexed signed-short normal, and one indexed signed-short ST
 * coordinate (8 fractional bits), with no vertex matrix or color fields.
 * Return zero on any layout change so the caller uses load_vertex instead. */
static int load_rs_vertex_rest(const GxCpState* cp, u32 vat, const u8* v,
                               u32 vstride, InVtx* out) {
    u32 low = cp->vtx_desc_lo;
    u32 pos_bytes;
    if (vat != 0u || cp->vtx_desc_hi != 0x00000002u ||
        cp->vat_g0[vat] != 0x50F76C09u)
        return 0;
    if (low == 0x00001600u && vstride == 4u)
        pos_bytes = 2u;
    else if (low == 0x00001400u && vstride == 3u)
        pos_bytes = 1u;
    else
        return 0;

    float position[3] = {
        out->position[0], out->position[1], out->position[2]
    };
    u8 pos_mtx = out->posMtx;
    memset(out, 0, sizeof *out);
    out->posMtx = pos_mtx;
    memcpy(out->position, position, sizeof position);
    memcpy(out->normal, s_normal_cache, sizeof out->normal);
    const u8* normal = array_ptr(cp, 1, v[pos_bytes], 6u);
    if (!normal)
        return 0;
    out->normal[0][0] = be_s16(normal) * (1.0f / 16384.0f);
    out->normal[0][1] = be_s16(normal + 2) * (1.0f / 16384.0f);
    out->normal[0][2] = be_s16(normal + 4) * (1.0f / 16384.0f);
    memcpy(s_normal_cache[0], out->normal[0], sizeof s_normal_cache[0]);
    memset(out->color, 0xFF, sizeof out->color);
    return 1;
}

/* ============================================================================
 * Setup unit (triangle fan) + public draw.
 * ==========================================================================*/
static void recompute_scissor(void) {
    /* ComputeScissorRects Best() — pick the rect with the largest viewport area,
     * tie-broken by total area (BPFunctions.cpp:43-172). */
    u32 tl = s_bp[0x20], br = s_bp[0x21], soff = s_bp[0x59];
    int left = (int)bits(tl, 12, 11);   /* ScissorPos.x */
    int top  = (int)bits(tl, 0, 11);    /* ScissorPos.y */
    int right = (int)bits(br, 12, 11);
    int bottom = (int)bits(br, 0, 11);
    int xoff = (int)bits(soff, 0, 9) << 1;
    int yoff = (int)bits(soff, 10, 9) << 1;

    s_scissor_left = 0; s_scissor_top = 0; s_scissor_right = 0; s_scissor_bottom = 0;
    s_scissor_xoff = 0; s_scissor_yoff = 0;
    if (left > right || top > bottom) return;

    float vx0 = vp_xo() - vp_wd(), vx1 = vp_xo() + vp_wd();
    if (vx0 > vx1) { float t = vx0; vx0 = vx1; vx1 = t; }
    float vy0 = vp_yo() - vp_ht(), vy1 = vp_yo() + vp_ht();
    if (vy0 > vy1) { float t = vy0; vy0 = vy1; vy1 = t; }

    long best_vp = -1, best_area = -1;
    for (int ex = -4096; ex <= 4096; ex += 1024) {
        int nx = xoff + ex;
        int xs = clampi(left - nx, 0, (int)EFB_WIDTH);
        int xe = clampi(right - nx + 1, 0, (int)EFB_WIDTH);
        if (xs >= xe) continue;
        for (int ey = -4096; ey <= 4096; ey += 1024) {
            int ny = yoff + ey;
            int ys = clampi(top - ny, 0, (int)EFB_HEIGHT);
            int ye = clampi(bottom - ny + 1, 0, (int)EFB_HEIGHT);
            if (ys >= ye) continue;
            int vpx0 = clampi(xs + nx, (int)vx0, (int)vx1);
            int vpx1 = clampi(xe + nx, (int)vx0, (int)vx1);
            int vpy0 = clampi(ys + ny, (int)vy0, (int)vy1);
            int vpy1 = clampi(ye + ny, (int)vy0, (int)vy1);
            long vparea = (long)(vpx1 - vpx0) * (vpy1 - vpy0);
            long area = (long)(xe - xs) * (ye - ys);
            if (vparea > best_vp || (vparea == best_vp && area > best_area)) {
                best_vp = vparea; best_area = area;
                s_scissor_left = xs; s_scissor_right = xe;
                s_scissor_top = ys; s_scissor_bottom = ye;
                s_scissor_xoff = nx; s_scissor_yoff = ny;
            }
        }
    }
}

/* Populate s_cfg (+ the shared s_pf/s_zt_upd/s_bm_cu/s_bm_au quad) once for the
 * whole draw call — see the big "Per-draw config cache" comment above
 * GetPixelColor for why this is exact rather than approximate. Must run AFTER
 * tev_load_registers(&s_tev): stage_konst resolution reads s_tev.Konst[],
 * which that call just (re)loaded from bp 0xE0-0xE7. */
/* ============================================================================
 * GX-MT per-draw carry-freedom analysis. A draw may be scanned by multiple
 * workers iff NO pixel's shading reads Tev state left behind by an earlier
 * pixel. tev_draw's per-stage order is: (1) tex sample if `enable` (writes
 * TexColor), (2) StageKonst copy and (3) RasColor recompute — both
 * UNCONDITIONAL, so both are always defined before any read below — then
 * (4) the four color_arg + four alpha_arg reads, then (5) the Reg[c22].rgb /
 * Reg[a22].a writes (regular AND compare variants both write their dest
 * unconditionally). The fused paths are proven exact folds of this same
 * program, so the analysis on the decoded cfg words covers them too.
 *
 * A read is carry-free iff the component is either (a) already written
 * earlier in the SAME pixel's stage sequence, or (b) a per-draw constant:
 * Reg/Konst are reloaded from BP by tev_load_registers at every draw entry,
 * so a Reg component the program never writes is constant across the draw's
 * pixels (and a worker's template copy of it is exact). TexColor and the
 * rasterized Color[chan] channels are NOT reloaded per draw, so reading one
 * before this pixel defines it means inheriting the PREVIOUS pixel's (or
 * previous draw's) value -> not partitionable; scan serially. The TRAPs
 * below additionally flag the cross-draw flavor of that carry when GX-MT is
 * active, because after any forked draw even the SERIAL fallback's carry-in
 * value could differ from true-serial history — none of the IPL's 8 censused
 * configs does this (verified: traps never fire on the golden runs), and the
 * byte-exact XFB gate arbitrates.
 * ==========================================================================*/
static int draw_parallel_ok(void) {
    u8 reg_rgb_w[4] = {0,0,0,0}, reg_a_w[4] = {0,0,0,0};       /* written anywhere */
    u8 reg_rgb_def[4] = {0,0,0,0}, reg_a_def[4] = {0,0,0,0};   /* defined so far this pixel */
    int texc_def = 0;

    for (u32 st = 0; st <= s_cfg.numtevstages; st++) {
        reg_rgb_w[s_cfg.stage[st].c22] = 1;
        reg_a_w[s_cfg.stage[st].a22] = 1;
    }
    for (u32 st = 0; st <= s_cfg.numtevstages; st++) {
        const TevStageCfg* sc = &s_cfg.stage[st];
        if (sc->enable) texc_def = 1;   /* (1) runs before this stage's reads */

        /* Rasterized color channel: raster_pixel_prep only writes
         * Color[i] for i < numcolchans each pixel, so a stage selecting a
         * channel >= numcolchans reads a stale value carried across pixels
         * (and draws) — Dolphin semantics our serial path preserves, but not
         * partitionable. (colorchan 7 loads zeros; others trap in tev_draw.) */
        if ((sc->colorchan == 0 || sc->colorchan == 1) &&
            sc->colorchan >= s_cfg.numcolchans) {
            if (s_mt_threads > 1)
                TRAP(mt_colorchan_carry, "GX-MT: stage reads a color channel "
                     "raster_pixel_prep does not write (carry) — serial fallback; "
                     "cross-draw carry-in after a forked draw is not serial-exact, "
                     "golden gate arbitrates");
            return 0;
        }

        const u32 cargs[4] = { sc->argA, sc->argB, sc->argC, sc->argD };
        for (int i = 0; i < 4; i++) {
            u32 a = cargs[i];
            switch (a) {
            case 0: case 2: case 4: case 6:       /* Reg[a>>1].rgb */
                if (reg_rgb_w[a >> 1] && !reg_rgb_def[a >> 1]) return 0;
                break;
            case 1: case 3: case 5: case 7:       /* Reg[(a-1)>>1].a */
                if (reg_a_w[(a - 1) >> 1] && !reg_a_def[(a - 1) >> 1]) return 0;
                break;
            case 8: case 9:                        /* TexColor.rgb / .a */
                if (!texc_def) {
                    if (s_mt_threads > 1)
                        TRAP(mt_texc_carry, "GX-MT: stage reads TexColor before "
                             "any enabled stage writes it (carry) — serial "
                             "fallback; cross-draw carry-in after a forked draw "
                             "is not serial-exact, golden gate arbitrates");
                    return 0;
                }
                break;
            default: break;   /* 10/11 RasColor (recomputed this stage),
                               * 12/13 constants, 14 StageKonst (per-draw),
                               * 15 zero — all carry-free */
            }
        }
        const u32 aargs[4] = { sc->aargA, sc->aargB, sc->aargC, sc->aargD };
        for (int i = 0; i < 4; i++) {
            u32 a = aargs[i];
            if (a <= 3) {                          /* Reg[a].a */
                if (reg_a_w[a] && !reg_a_def[a]) return 0;
            } else if (a == 4) {                   /* TexColor.a */
                if (!texc_def) {
                    if (s_mt_threads > 1)
                        TRAP(mt_texca_carry, "GX-MT: stage reads TexColor.a before "
                             "any enabled stage writes it (carry) — serial "
                             "fallback; cross-draw carry-in after a forked draw "
                             "is not serial-exact, golden gate arbitrates");
                    return 0;
                }
            }
            /* 5 RasColor.a, 6 StageKonst.a, 7 zero — carry-free */
        }

        reg_rgb_def[sc->c22] = 1;                  /* (5) this stage's writes */
        reg_a_def[sc->a22] = 1;
    }
    return 1;
}

static void advance_texel_cache_generation(void) {
    /* Bump the per-draw texel cache's generation (see the big "Per-draw texel
     * cache" comment above tex_sample) — this is the ENTIRE invalidation cost
     * for a new draw: one integer increment, no memset. Skip stored-gen 0 (it
     * is the "slot never written since last full clear" sentinel); on the
     * u32 wraparound that lands exactly on 0, pay for one full-table clear
     * (under a MB across all worker caches — happens once every ~4 billion
     * draws, i.e. never in practice) and resume at 1. */
    if (++s_texel_cache_gen == 0) {
        memset(s_texel_cache_w, 0, sizeof s_texel_cache_w);
        s_texel_cache_gen = 1;
    }
}

static void build_draw_cfg(void) {

    s_pf     = pixel_format();
    s_zt_upd = zm_update_enable();
    s_bm_cu  = bm_color_update();
    s_bm_au  = bm_alpha_update();

    s_cfg.numtexgens   = gm_numtexgens();
    s_cfg.numcolchans  = gm_numcolchans();
    s_cfg.numtevstages = gm_numtevstages();
    s_cfg.cullmode     = gm_cull_mode();

    s_cfg.zt_enable = zm_test_enable();
    s_cfg.zt_early  = early_ztest();
    s_cfg.zt_func   = zm_func();

    s_cfg.bm_blend_enable = bm_blend_enable();
    s_cfg.bm_logic_enable = bm_logic_enable();
    s_cfg.bm_dither       = bm_dither();
    s_cfg.bm_dst_factor   = bm_dst_factor();
    s_cfg.bm_src_factor   = bm_src_factor();
    s_cfg.bm_subtract     = bm_subtract();
    s_cfg.bm_logic_mode   = bm_logic_mode();

    s_cfg.da_enable = da_enable();
    s_cfg.da_alpha  = da_alpha();

    for (u32 id = 0; id < 4; id++) swap_table(id, s_cfg.swaptab[id]);

    for (u32 stage = 0; stage <= s_cfg.numtevstages; stage++) {
        TevStageCfg* sc = &s_cfg.stage[stage];
        u32 s2 = stage >> 1, odd = stage & 1;
        u32 order = s_bp[0x28 + s2];
        u32 cc = s_bp[0xC0 + stage * 2];
        u32 ac = s_bp[0xC1 + stage * 2];
        sc->cc = cc; sc->ac = ac;

        sc->texcoordSel = odd ? bits(order, 15, 3) : bits(order, 3, 3);
        sc->texmap      = odd ? bits(order, 12, 3) : bits(order, 0, 3);
        sc->enable      = odd ? bits(order, 18, 1) : bits(order, 6, 1);
        sc->colorchan   = odd ? bits(order, 19, 3) : bits(order, 7, 3);
        if (sc->texcoordSel >= s_cfg.numtexgens) sc->texcoordSel = 0;

        sc->tevind = s_bp[0x10 + stage];

        sc->tswap_id = bits(ac, 2, 2);
        sc->rswap_id = bits(ac, 0, 2);

        /* konst for this stage (kcsel/kasel, BPMemory AllTevKSels) — resolved
         * once, not just its selector (see TevStageCfg comment). */
        u32 ksel = s_bp[0xF6 + s2];
        u32 kc = odd ? bits(ksel, 14, 5) : bits(ksel, 4, 5);
        u32 ka = odd ? bits(ksel, 19, 5) : bits(ksel, 9, 5);
        { s16 r, g, b, a;
          konst_lookup(&s_tev_w[0], kc, &r, &g, &b, &a);
          sc->stage_konst.r = r; sc->stage_konst.g = g; sc->stage_konst.b = b;
          konst_lookup(&s_tev_w[0], ka, &r, &g, &b, &a); sc->stage_konst.a = a; }

        sc->argA = bits(cc, 12, 4); sc->argB = bits(cc, 8, 4);
        sc->argC = bits(cc, 4, 4);  sc->argD = bits(cc, 0, 4);
        sc->aargA = bits(ac, 13, 3); sc->aargB = bits(ac, 10, 3);
        sc->aargC = bits(ac, 7, 3);  sc->aargD = bits(ac, 4, 3);

        sc->c16 = bits(cc, 16, 2); sc->c18 = bits(cc, 18, 1);
        sc->c19 = bits(cc, 19, 1); sc->c20 = bits(cc, 20, 2); sc->c22 = bits(cc, 22, 2);
        sc->a16 = bits(ac, 16, 2); sc->a18 = bits(ac, 18, 1);
        sc->a19 = bits(ac, 19, 1); sc->a20 = bits(ac, 20, 2); sc->a22 = bits(ac, 22, 2);
    }

    for (u32 unit = 0; unit < 8; unit++) {
        TexUnitCfg* tc = &s_cfg.tex[unit];
        u32 ti0 = tx_image0(unit);
        tc->image0_raw = ti0;
        tc->fmt = bits(ti0, 20, 4);
        tc->w1  = (int)bits(ti0, 0, 10);
        tc->h1  = (int)bits(ti0, 10, 10);

        u32 ti3 = tx_image3(unit);
        tc->image3_raw = ti3;
        tc->img_base = (ti3 & 0x00ffffffu) << 5;
        tc->phys = tc->img_base & 0x1FFFFFFFu;
        tc->valid = (s_cpu && s_cpu->ram && tc->phys < s_cpu->ram_size);
        tc->src     = tc->valid ? s_cpu->ram + tc->phys : NULL;
        tc->src_len = tc->valid ? s_cpu->ram_size - tc->phys : 0u;

        /* TX_SETTLUT (BPMemory.h TexTLUT, regs 0x98+unit / 0xB8+(unit-4)):
         * palette location inside modeled TMEM (tmem_offset<<9) + entry
         * format. Read for every unit; only C4/C8/C14X2 decodes consume it.
         * The pointer can never leave the 1MB TMEM array — max reachable
         * byte is 0x3FF<<9 + 16383*2+1 < 1MB (Dolphin's own static bound). */
        u32 tlut_reg = (unit < 4u) ? s_bp[0x98 + unit] : s_bp[0xB8 + (unit - 4u)];
        tc->tlut    = gcn_gx_tmem() + ((tlut_reg & 0x3FFu) << 9);
        tc->tlutfmt = bits(tlut_reg, 10, 2);

        u32 mode0 = tx_mode0(unit), mode1 = tx_mode1(unit);
        tc->wrap_s = bits(mode0, 0, 2);
        tc->wrap_t = bits(mode0, 2, 2);
        tc->mipmap_filter = bits(mode0, 5, 2);
        tc->magf = bits(mode0, 4, 1);
        tc->minf = bits(mode0, 7, 1);
        tc->lod_edge = (int)bits(mode0, 8, 1);
        { int bias = (int)sext(bits(mode0, 9, 8), 8); tc->lod_bias_half = bias >> 1; }
        tc->minlod = bits(mode1, 0, 8);
        tc->maxlod = bits(mode1, 8, 8);
    }

    /* Fused specialized per-pixel path selection (perf task; see the big
     * fused_pixel_A/B/C comment block near tev_draw for the derivation this
     * checks against). Runs on EVERY draw, independent of GCN_GX_TEV_CENSUS
     * (that knob is diagnostic-only; this is the actual perf feature).
     * GCN_GX_NO_FUSED=1 forces s_cfg.fused to stay NULL unconditionally — the
     * task's same-binary A/B exactness proof (fused vs general, same
     * executable, XFB hash must match either way) toggles this and nothing
     * else. FULL field compare: fused_common_match() covers every
     * draw-global field the fused math folds (counts, z, blend/dest-alpha
     * state including dither/logic/subtract/factors, the alpha-test word,
     * swaptab[0]'s actual contents); fused_stage_match() covers each stage's
     * cc/ac/enable/colorchan exactly (an exact cc/ac word match already pins
     * every arg/bias/op/clamp/scale/dest/swap-id selector the fold depends
     * on, since those are exactly the bits they were extracted from above).
     * stage_konst's VALUE is deliberately never compared for config B's
     * stage1 (aargB==6/StageKonst.a) — the fold reads it live every draw
     * (s_cfg.stage[1].stage_konst.a), it is never assumed to be any
     * particular number, so there is nothing about it to pin in a signature
     * beyond the cc/ac match already pinning WHICH selector case applies. */
    /* GX-MT: per-draw carry-freedom verdict (see draw_parallel_ok's proof
     * comment above) — gates every fork this draw's triangles could take. */
    s_cfg.parallel_ok = draw_parallel_ok();

    if (s_no_fused < 0) s_no_fused = getenv("GCN_GX_NO_FUSED") ? 1 : 0;
    s_cfg.fused = NULL;
    s_cfg.fused_needs_ras_rgb = 0;
    if (!s_no_fused && fused_common_match()) {
        if (s_cfg.numtevstages == 0 &&
            fused_stage_match(0, 0x00F8CFu, 0x00F670u, 1, 0)) {
            s_cfg.fused = fused_pixel_A;
        } else if (s_cfg.numtevstages == 0 &&
                   fused_stage_match(0, 0x18F28Fu, 0x08F670u, 1, 0)) {
            s_cfg.fused = fused_pixel_C;
        } else if (s_cfg.numtevstages == 1 &&
                   fused_stage_match(0, 0x18F28Fu, 0x08F670u, 1, 0) &&
                   fused_stage_match(1, 0x08FC0Fu, 0x08F870u, 0, 0)) {
            s_cfg.fused = fused_pixel_B;
        } else if (s_cfg.numtevstages == 0 &&
                   fused_stage_match(0, 0x08FCAFu, 0x08F2F0u, 1, 0)) {
            s_cfg.fused = fused_pixel_D;   /* D1 boot-animation config, see its derivation */
            s_cfg.fused_needs_ras_rgb = 1; /* reads RasColor.rgb -- see the field's comment */
        } else if (s_cfg.numtevstages == 0 &&
                   fused_stage_match(0, 0x18428Fu, 0x08F770u, 1, 0)) {
            s_cfg.fused = fused_pixel_E;   /* D1 residuals-sweep config, see its derivation */
        } else if (s_cfg.numtevstages == 1 &&
                   fused_stage_match(0, 0x18428Fu, 0x08F770u, 1, 0) &&
                   fused_stage_match(1, 0x08FC0Fu, 0x08F870u, 0, 0)) {
            s_cfg.fused = fused_pixel_G;
        } else if (s_cfg.numtevstages == 1 &&
                   fused_stage_match(0, 0x00F8CFu, 0x00F670u, 1, 0) &&
                   fused_stage_match(1, 0x08FC0Fu, 0x08F870u, 0, 0)) {
            s_cfg.fused = fused_pixel_H;
        } else if (s_cfg.numtevstages == 1 &&
                   fused_stage_match(0, 0x00F8CFu, 0x00F770u, 1, 0) &&
                   fused_stage_match(1, 0x08FC0Fu, 0x08F870u, 0, 0)) {
            s_cfg.fused = fused_pixel_I;
        }
    } else if (!s_no_fused && fused_common_match_notex()) {
        if (s_cfg.numtevstages == 0 &&
            fused_stage_match(0, 0x00AFFFu, 0x00BFF0u, 1, 0)) {
            s_cfg.fused = fused_pixel_F;   /* D1 residuals-sweep config, see its derivation */
            s_cfg.fused_needs_ras_rgb = 1; /* reads RasColor.rgb -- see the field's comment */
        } else if (s_cfg.numtevstages == 1 &&
                   fused_stage_match(0, 0x00AFFFu, 0x00BFF0u, 1, 0) &&
                   fused_stage_match(1, 0x08FC0Fu, 0x08F870u, 0, 0)) {
            s_cfg.fused = fused_pixel_J;
            s_cfg.fused_needs_ras_rgb = 1;
        }
    } else if (!s_no_fused && gpu_program_Y_match()) {
        /* Y/Z: at=0x3F0000, not 0x7F0000 -- neither fused_common_match() nor
         * fused_common_match_notex() covers this word, so Y/Z self-check
         * their own full precondition set (see the derivation comment above
         * gpu_program_Y_match). Y is checked first since its extra
         * blend_enable==1 test is a strict narrowing of Z's. */
        s_cfg.fused = fused_pixel_Y;
        s_cfg.fused_needs_ras_rgb = 1;   /* reads RasColor.rgb -- see gx_raster.c ~3774 */
    } else if (!s_no_fused && gpu_program_Z_match()) {
        s_cfg.fused = fused_pixel_Z;
        s_cfg.fused_needs_ras_rgb = 1;
    } else if (!s_no_fused && gpu_program_AA_match()) {
        /* AA: da_enable==1 and sf=6(DstAlpha)/df=1(One), neither of which
         * fused_common_match()/_notex() (da_enable==0, sf=4/df=5) permit --
         * self-checks its own full precondition set. */
        s_cfg.fused = fused_pixel_AA;
        s_cfg.fused_needs_ras_rgb = 1;
    }
    s_cfg.program_id = compute_program_id();

    /* GCN_GX_TEV_CENSUS (see the block comment above this function). */
    if (s_tev_census < 0) s_tev_census = getenv("GCN_GX_TEV_CENSUS") ? 1 : 0;
    s_census_cur = -1;
    if (s_tev_census) {
        u32 h = 2166136261u;                     /* FNV-1a over shape fields */
        #define CEN_MIX(v) do { h = (h ^ (u32)(v)) * 16777619u; } while (0)
        CEN_MIX(s_cfg.numtevstages); CEN_MIX(s_cfg.numtexgens);
        CEN_MIX(s_cfg.numcolchans);
        CEN_MIX(s_cfg.zt_enable); CEN_MIX(s_cfg.zt_early); CEN_MIX(s_cfg.zt_func);
        CEN_MIX(s_zt_upd); CEN_MIX(s_bm_cu); CEN_MIX(s_bm_au);
        CEN_MIX(s_cfg.bm_blend_enable); CEN_MIX(s_cfg.bm_logic_enable);
        CEN_MIX(s_cfg.bm_dither); CEN_MIX(s_cfg.bm_src_factor);
        CEN_MIX(s_cfg.bm_dst_factor); CEN_MIX(s_cfg.bm_subtract);
        CEN_MIX(s_cfg.bm_logic_mode);
        CEN_MIX(s_cfg.da_enable); CEN_MIX(s_cfg.da_alpha);
        CEN_MIX(s_bp[0xF3]);                     /* alpha-test word */
        for (u32 st = 0; st <= s_cfg.numtevstages; st++) {
            const TevStageCfg* sc = &s_cfg.stage[st];
            CEN_MIX(sc->cc); CEN_MIX(sc->ac);    /* full combiner words */
            CEN_MIX(sc->enable); CEN_MIX(sc->colorchan);
        }
        #undef CEN_MIX
        int free_slot = -1;
        for (int i = 0; i < GX_CENSUS_MAX; i++) {
            if (s_census[i].used && s_census[i].hash == h) { s_census_cur = i; break; }
            if (!s_census[i].used && free_slot < 0) free_slot = i;
        }
        if (s_census_cur < 0 && free_slot >= 0) {
            s_census_cur = free_slot;
            s_census[free_slot].used = 1;
            s_census[free_slot].hash = h;
            s_census[free_slot].program_id = s_cfg.program_id;
            fprintf(stderr, "[gx-census] NEW config #%d hash=%08x prog=%u stages=%u texgens=%u "
                    "colchans=%u ztest=%u/%u/func%u zupd=%u cu=%u/%u blend=%u logic=%u/%u sub=%u "
                    "dither=%u sf=%u df=%u da=%u/%u at=0x%06X",
                    free_slot, h, s_cfg.program_id, s_cfg.numtevstages + 1u, s_cfg.numtexgens,
                    s_cfg.numcolchans, s_cfg.zt_enable, s_cfg.zt_early, s_cfg.zt_func,
                    s_zt_upd, s_bm_cu, s_bm_au, s_cfg.bm_blend_enable, s_cfg.bm_logic_enable,
                    s_cfg.bm_logic_mode, s_cfg.bm_subtract, s_cfg.bm_dither,
                    s_cfg.bm_src_factor, s_cfg.bm_dst_factor,
                    s_cfg.da_enable, s_cfg.da_alpha, s_bp[0xF3]);
            for (u32 st = 0; st <= s_cfg.numtevstages; st++)
                fprintf(stderr, " | st%u cc=%06X ac=%06X en=%u chan=%u",
                        st, s_cfg.stage[st].cc, s_cfg.stage[st].ac,
                        s_cfg.stage[st].enable, s_cfg.stage[st].colorchan);
            fprintf(stderr, "\n");
            fflush(stderr);
        }
        if (s_census_cur < 0 && free_slot < 0) {
            /* Table full and this shape has never been seen: it has no
             * bucket to tally into. Count it instead of dropping it. */
            s_census_overflow_draws++;
            int seen = 0;
            for (int i = 0; i < s_census_overflow_shapes; i++)
                if (s_census_overflow_hashes[i] == h) { seen = 1; break; }
            if (!seen && s_census_overflow_shapes < GX_CENSUS_OVERFLOW_MAX)
                s_census_overflow_hashes[s_census_overflow_shapes++] = h;
        }
        if (s_census_cur >= 0) s_census[s_census_cur].draws++;
    }
}

static u64 debug_hash_bytes(const u8* p, u32 len) {
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; p && i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* Exact size of GX's tiled base-level texture storage. Every listed block is
 * 32 bytes except RGBA8's split AR/GB block, which is 64. Unknown formats are
 * left at zero and remain visible through the raw register fields. */
static u32 debug_texture_bytes(const TexUnitCfg* tc) {
    u32 bw = 0, bh = 0, block_bytes = 32;
    switch (tc->fmt) {
    case 0: case 8: case 14: bw = 8; bh = 8; break; /* I4/C4/CMPR */
    case 1: case 2: case 9:  bw = 8; bh = 4; break; /* I8/IA4/C8 */
    case 3: case 4: case 5: case 10:
        bw = 4; bh = 4; break;                       /* 16-bit formats */
    case 6:
        bw = 4; bh = 4; block_bytes = 64; break;    /* RGBA8 */
    default:
        return 0;
    }
    u64 width = (u32)tc->w1 + 1u;
    u64 height = (u32)tc->h1 + 1u;
    u64 blocks_x = (width + bw - 1u) / bw;
    u64 blocks_y = (height + bh - 1u) / bh;
    u64 bytes = blocks_x * blocks_y * block_bytes;
    if (bytes > tc->src_len) bytes = tc->src_len;
    if (bytes > 0xffffffffull) bytes = 0xffffffffull;
    return (u32)bytes;
}

static void debug_capture_draw(u32 prim, u32 vat, const u8* verts,
                               u32 nverts, u32 vstride) {
    s_debug_pending_index = -1;
    if (!s_tev_census || s_census_cur < 0 ||
        s_census_cur >= GX_CENSUS_MAX)
        return;

    GxDebugDraw* d = &s_debug_pending;
    memset(d, 0, sizeof *d);
    d->alpha_min = 255u;
    d->last_tex_alpha_min = 255u;
    d->valid = 1;
    d->frame = gcn_gx_frame_count();
    d->cpu_pc = s_cpu ? s_cpu->pc : 0u;
    d->dl = gcn_gx_current_dl();
    d->prim = prim;
    d->vat = vat;
    d->nverts = nverts;
    d->vstride = vstride;
    {
        u64 bytes = (u64)nverts * vstride;
        if (bytes > 0xffffffffull)
            bytes = 0xffffffffull;
        d->vertex_bytes = (u32)bytes;
        d->vertex_hash = debug_hash_bytes(verts, d->vertex_bytes);
        d->vertex_head_len = d->vertex_bytes < sizeof d->vertex_head ?
                             d->vertex_bytes : (u32)sizeof d->vertex_head;
        d->vertex_tail_len = d->vertex_bytes < sizeof d->vertex_tail ?
                             d->vertex_bytes : (u32)sizeof d->vertex_tail;
        if (d->vertex_head_len)
            memcpy(d->vertex_head, verts, d->vertex_head_len);
        if (d->vertex_tail_len)
            memcpy(d->vertex_tail,
                   verts + d->vertex_bytes - d->vertex_tail_len,
                   d->vertex_tail_len);
    }
    d->census_hash = s_census[s_census_cur].hash;
    d->program_id = s_cfg.program_id;
    d->genmode = s_bp[0x00];
    d->alpha_test = s_bp[0xF3];
    d->zmode = s_bp[0x40];
    d->blendmode = s_bp[0x41];
    d->dstalpha = s_bp[0x42];
    d->pecontrol = s_bp[0x43];
    d->numtexgens = s_cfg.numtexgens;
    d->numcolchans = s_cfg.numcolchans;
    d->numtevstages = s_cfg.numtevstages + 1u;
    for (u32 reg = 0; reg < 4u; ++reg) {
        const TColor* tev_reg = &s_tev_w[0].Reg[reg];
        const TColor* tev_konst = &s_tev_w[0].Konst[reg];
        d->bp_tev_ra[reg] = s_bp[0xE0u + reg * 2u];
        d->bp_tev_bg[reg] = s_bp[0xE1u + reg * 2u];
        d->tev_reg[reg][0] = tev_reg->r;
        d->tev_reg[reg][1] = tev_reg->g;
        d->tev_reg[reg][2] = tev_reg->b;
        d->tev_reg[reg][3] = tev_reg->a;
        d->tev_konst[reg][0] = tev_konst->r;
        d->tev_konst[reg][1] = tev_konst->g;
        d->tev_konst[reg][2] = tev_konst->b;
        d->tev_konst[reg][3] = tev_konst->a;
    }

    u32 active_textures = 0;
    for (u32 stage = 0; stage < d->numtevstages && stage < 16u; ++stage) {
        const TevStageCfg* sc = &s_cfg.stage[stage];
        u32 order = s_bp[0x28 + (stage >> 1)];
        d->stage[stage].order = order;
        d->stage[stage].texcoord = sc->texcoordSel;
        d->stage[stage].texmap = sc->texmap;
        d->stage[stage].enable = sc->enable;
        d->stage[stage].colorchan = sc->colorchan;
        d->stage[stage].cc = sc->cc;
        d->stage[stage].ac = sc->ac;
        d->stage[stage].tevind = sc->tevind;
        d->stage[stage].ksel = s_bp[0xF6u + (stage >> 1)];
        d->stage[stage].kcsel =
            (stage & 1u) ? bits(d->stage[stage].ksel, 14, 5) :
                           bits(d->stage[stage].ksel, 4, 5);
        d->stage[stage].kasel =
            (stage & 1u) ? bits(d->stage[stage].ksel, 19, 5) :
                           bits(d->stage[stage].ksel, 9, 5);
        d->stage[stage].konst[0] = sc->stage_konst.r;
        d->stage[stage].konst[1] = sc->stage_konst.g;
        d->stage[stage].konst[2] = sc->stage_konst.b;
        d->stage[stage].konst[3] = sc->stage_konst.a;
        if (sc->enable && sc->texmap < 8u)
            active_textures |= 1u << sc->texmap;
    }

    for (u32 unit = 0; unit < 8u; ++unit) {
        const TexUnitCfg* tc = &s_cfg.tex[unit];
        GxDebugTexture* t = &d->tex[unit];
        t->mode0 = tx_mode0(unit);
        t->mode1 = tx_mode1(unit);
        t->image0 = tc->image0_raw;
        t->image3 = tc->image3_raw;
        t->tlut = s_bp[(unit < 4u ? 0x98u : 0xB8u) + (unit & 3u)];
        t->fmt = tc->fmt;
        t->width = (u32)tc->w1 + 1u;
        t->height = (u32)tc->h1 + 1u;
        t->phys = tc->phys;
        t->active = (active_textures & (1u << unit)) != 0;
        t->valid = tc->valid;
        if (t->active && t->valid) {
            t->bytes = debug_texture_bytes(tc);
            t->hash = debug_hash_bytes(tc->src, t->bytes);
            t->sample_len = t->bytes < sizeof t->sample ?
                            t->bytes : (u32)sizeof t->sample;
            memcpy(t->sample, tc->src, t->sample_len);
        }
    }
    s_debug_pending_index = s_census_cur;
    s_debug_pending_pixels_before = s_census[s_census_cur].pixels;
}

static void debug_copy_recent(GxDebugRecent* recent,
                              const GxDebugDraw* pending,
                              u64 sequence) {
    memset(recent, 0, sizeof *recent);
    recent->sequence = sequence;
    recent->frame = pending->frame;
    recent->cpu_pc = pending->cpu_pc;
    recent->dl = pending->dl;
    recent->census_hash = pending->census_hash;
    recent->prim = pending->prim;
    recent->nverts = pending->nverts;
    recent->vstride = pending->vstride;
    recent->vertex_hash = pending->vertex_hash;
    recent->pixels = pending->pixels;
    recent->alpha_tested = pending->alpha_tested;
    recent->alpha_rejected = pending->alpha_rejected;
    recent->alpha_sum = pending->alpha_sum;
    recent->alpha_min = pending->alpha_min;
    recent->alpha_max = pending->alpha_max;
    recent->blend_inputs = pending->blend_inputs;
    recent->z_rejected = pending->z_rejected;
    recent->color_writes = pending->color_writes;
    recent->triangles_submitted = pending->triangles_submitted;
    recent->triangles_trivial_rejected =
        pending->triangles_trivial_rejected;
    recent->triangles_culled = pending->triangles_culled;
    recent->triangles_clipped = pending->triangles_clipped;
    recent->triangles_rasterized = pending->triangles_rasterized;
    recent->bbox_area_sum = pending->bbox_area_sum;
    recent->bbox_valid = pending->bbox_valid;
    recent->bbox_minx = pending->bbox_minx;
    recent->bbox_miny = pending->bbox_miny;
    recent->bbox_maxx = pending->bbox_maxx;
    recent->bbox_maxy = pending->bbox_maxy;
    recent->largest_triangle_area = pending->largest_triangle_area;
    for (u32 v = 0; v < 3u; ++v)
        memcpy(recent->largest_screen[v],
               pending->largest_triangle[v].screen,
               sizeof recent->largest_screen[v]);
    recent->alpha_test = pending->alpha_test;
    recent->zmode = pending->zmode;
    recent->blendmode = pending->blendmode;
    recent->pecontrol = pending->pecontrol;
}

static void debug_finalize_draw(void) {
    int index = s_debug_pending_index;
    if (index < 0 || index >= GX_CENSUS_MAX)
        return;
    GxDebugDraw* pending = &s_debug_pending;
    pending->pixels = s_census[index].pixels - s_debug_pending_pixels_before;
    if (pending->frame > s_debug_recent_latest_frame)
        s_debug_recent_latest_frame = pending->frame;
    /* Keep the ring focused on draws that can explain a missing large scene
     * element. The title submits thousands of tiny object draws in one GX
     * frame; retaining all of them evicts the preceding frame before TCP can
     * inspect it. Preserve every small, zero-pixel primitive (the hidden
     * rejected-sky case), plus every draw with material raster coverage, but
     * only when BP color-update is enabled. Depth-only prepasses are already
     * represented by the per-config snapshots above. */
    if (bits(pending->blendmode, 3, 1) &&
        pending->triangles_submitted != 0u &&
        ((pending->pixels == 0u && pending->triangles_submitted <= 8u) ||
         pending->bbox_area_sum >= 8192u ||
         pending->largest_triangle_area >= 4096u)) {
        u64 sequence = ++s_debug_recent_sequence;
        GxDebugRecent* recent =
            &s_debug_recent[s_debug_recent_head % GX_DEBUG_RECENT_MAX];
        debug_copy_recent(recent, pending, sequence);
        s_debug_recent_head++;
        if (s_debug_recent_count < GX_DEBUG_RECENT_MAX)
            s_debug_recent_count++;
        if (pending->bbox_area_sum >= 8192u ||
            pending->largest_triangle_area >= 4096u ||
            pending->pixels >= 4096u) {
            GxDebugRecent* large =
                &s_debug_large[s_debug_large_head % GX_DEBUG_LARGE_MAX];
            debug_copy_recent(large, pending, sequence);
            s_debug_large_head++;
            if (s_debug_large_count < GX_DEBUG_LARGE_MAX)
                s_debug_large_count++;
            if (pending->frame > s_debug_large_latest_frame)
                s_debug_large_latest_frame = pending->frame;
        }
    }
    GxDebugDraw* saved = &s_debug_draw[index];
    /* Retain the draw with the greatest actual pixel contribution for each
     * shading configuration. A tie favors the latest observation so moving
     * texture contents remain inspectable. This makes the late TCP snapshot
     * useful for scene coverage instead of preserving an arbitrary tiny tail
     * draw that happened to share the same TEV program. */
    if (!saved->valid || pending->pixels >= saved->pixels) {
        *saved = *pending;
        saved->sequence = ++s_debug_draw_sequence;
    }
    s_debug_pending_index = -1;
}

static int debug_json_append(char* out, size_t cap, int n,
                             const char* fmt, ...) {
    if (!out || cap == 0 || n < 0 || (size_t)n >= cap)
        return n;
    va_list ap;
    va_start(ap, fmt);
    int wrote = vsnprintf(out + n, cap - (size_t)n, fmt, ap);
    va_end(ap);
    if (wrote < 0)
        return n;
    if ((size_t)wrote >= cap - (size_t)n)
        return (int)cap - 1;
    return n + wrote;
}

static int debug_json_append_recent(char* out, size_t cap, int n,
                                    const GxDebugRecent* r) {
    return debug_json_append(out, cap, n,
        "{\"sequence\":%llu,\"frame\":%llu,\"pc\":%u,\"dl\":%u,"
        "\"hash\":\"%08x\",\"prim\":%u,\"nverts\":%u,\"vstride\":%u,"
        "\"vertex_hash\":\"%016llx\",\"pixels\":%llu,"
        "\"alpha_tested\":%llu,\"alpha_rejected\":%llu,"
        "\"alpha_min\":%u,\"alpha_max\":%u,\"alpha_sum\":%llu,"
        "\"blend_inputs\":%llu,\"z_rejected\":%llu,\"color_writes\":%llu,"
        "\"triangles\":[%u,%u,%u,%u,%u],"
        "\"bbox\":[%s,%d,%d,%d,%d,%llu],"
        "\"largest\":{\"area\":%u,\"screen\":["
        "[%.9g,%.9g,%.9g],[%.9g,%.9g,%.9g],[%.9g,%.9g,%.9g]]},"
        "\"alpha_test\":%u,\"zmode\":%u,\"blendmode\":%u,"
        "\"pecontrol\":%u}",
        (unsigned long long)r->sequence,
        (unsigned long long)r->frame, r->cpu_pc, r->dl,
        r->census_hash, r->prim, r->nverts, r->vstride,
        (unsigned long long)r->vertex_hash,
        (unsigned long long)r->pixels,
        (unsigned long long)r->alpha_tested,
        (unsigned long long)r->alpha_rejected,
        r->alpha_min, r->alpha_max,
        (unsigned long long)r->alpha_sum,
        (unsigned long long)r->blend_inputs,
        (unsigned long long)r->z_rejected,
        (unsigned long long)r->color_writes,
        r->triangles_submitted, r->triangles_trivial_rejected,
        r->triangles_culled, r->triangles_clipped,
        r->triangles_rasterized,
        r->bbox_valid ? "true" : "false",
        r->bbox_minx, r->bbox_miny, r->bbox_maxx, r->bbox_maxy,
        (unsigned long long)r->bbox_area_sum,
        r->largest_triangle_area,
        r->largest_screen[0][0], r->largest_screen[0][1],
        r->largest_screen[0][2], r->largest_screen[1][0],
        r->largest_screen[1][1], r->largest_screen[1][2],
        r->largest_screen[2][0], r->largest_screen[2][1],
        r->largest_screen[2][2], r->alpha_test, r->zmode,
        r->blendmode, r->pecontrol);
}

int gx_raster_debug_draw_state_json(char* out, size_t cap) {
    if (!out || cap < 2)
        return -1;
    static const char hex[] = "0123456789abcdef";
    int n = debug_json_append(out, cap, 0,
        "{\"ok\":true,\"kind\":\"gx_draw_state\",\"sequence\":%llu,"
        "\"configs\":[", (unsigned long long)s_debug_draw_sequence);
    int emitted = 0;
    for (int i = 0; i < GX_CENSUS_MAX; ++i) {
        const GxDebugDraw* d = &s_debug_draw[i];
        if (!d->valid)
            continue;
        n = debug_json_append(out, cap, n,
            "%s{\"index\":%d,\"hash\":\"%08x\",\"sequence\":%llu,"
            "\"frame\":%llu,\"pc\":%u,\"dl\":%u,\"prim\":%u,\"vat\":%u,"
            "\"nverts\":%u,\"vstride\":%u,\"vertex_bytes\":%u,"
            "\"vertex_hash\":\"%016llx\",\"vertex_head\":\"",
            emitted++ ? "," : "", i, d->census_hash,
            (unsigned long long)d->sequence,
            (unsigned long long)d->frame, d->cpu_pc, d->dl, d->prim, d->vat,
            d->nverts, d->vstride, d->vertex_bytes,
            (unsigned long long)d->vertex_hash);
        for (u32 b = 0; b < d->vertex_head_len && (size_t)n + 2u < cap; ++b) {
            out[n++] = hex[d->vertex_head[b] >> 4];
            out[n++] = hex[d->vertex_head[b] & 15u];
        }
        n = debug_json_append(out, cap, n,
            "\",\"vertex_tail\":\"");
        for (u32 b = 0; b < d->vertex_tail_len && (size_t)n + 2u < cap; ++b) {
            out[n++] = hex[d->vertex_tail[b] >> 4];
            out[n++] = hex[d->vertex_tail[b] & 15u];
        }
        n = debug_json_append(out, cap, n,
            "\",\"program\":%u,\"pixels\":%llu,"
            "\"pixel_flow\":{\"alpha_tested\":%llu,\"alpha_rejected\":%llu,"
            "\"alpha_min\":%u,\"alpha_max\":%u,\"alpha_sum\":%llu,"
            "\"last_tex_alpha_min\":%u,\"last_tex_alpha_max\":%u,"
            "\"last_tex_alpha_sum\":%llu,"
            "\"blend_inputs\":%llu,\"z_rejected\":%llu,"
            "\"color_writes\":%llu,\"output_rgba_sum\":[%llu,%llu,%llu,%llu],"
            "\"efb_rgba_sum\":[%llu,%llu,%llu,%llu]},"
            "\"triangles\":{\"submitted\":%u,\"trivial_rejected\":%u,"
            "\"culled\":%u,\"clipped\":%u,\"rasterized\":%u},"
            "\"bbox\":{\"valid\":%s,\"minx\":%d,\"miny\":%d,"
            "\"maxx\":%d,\"maxy\":%d,\"area_sum\":%llu},"
            "\"largest_triangle\":{\"area\":%u,\"vertices\":[",
            d->program_id, (unsigned long long)d->pixels,
            (unsigned long long)d->alpha_tested,
            (unsigned long long)d->alpha_rejected,
            d->alpha_min, d->alpha_max,
            (unsigned long long)d->alpha_sum,
            d->last_tex_alpha_min, d->last_tex_alpha_max,
            (unsigned long long)d->last_tex_alpha_sum,
            (unsigned long long)d->blend_inputs,
            (unsigned long long)d->z_rejected,
            (unsigned long long)d->color_writes,
            (unsigned long long)d->output_rgba_sum[0],
            (unsigned long long)d->output_rgba_sum[1],
            (unsigned long long)d->output_rgba_sum[2],
            (unsigned long long)d->output_rgba_sum[3],
            (unsigned long long)d->efb_rgba_sum[0],
            (unsigned long long)d->efb_rgba_sum[1],
            (unsigned long long)d->efb_rgba_sum[2],
            (unsigned long long)d->efb_rgba_sum[3],
            d->triangles_submitted, d->triangles_trivial_rejected,
            d->triangles_culled, d->triangles_clipped,
            d->triangles_rasterized, d->bbox_valid ? "true" : "false",
            d->bbox_minx, d->bbox_miny, d->bbox_maxx, d->bbox_maxy,
            (unsigned long long)d->bbox_area_sum, d->largest_triangle_area);
        for (u32 v = 0; v < 3u; ++v) {
            const float* obj = d->largest_triangle[v].obj;
            const float* mv = d->largest_triangle[v].mv;
            const float* clip = d->largest_triangle[v].clip;
            const float* screen = d->largest_triangle[v].screen;
            n = debug_json_append(out, cap, n,
                "%s{\"pos_mtx\":%u,\"obj\":[%.9g,%.9g,%.9g],"
                "\"mv\":[%.9g,%.9g,%.9g],"
                "\"clip\":[%.9g,%.9g,%.9g,%.9g],"
                "\"screen\":[%.9g,%.9g,%.9g],"
                "\"normal\":[[%.9g,%.9g,%.9g],[%.9g,%.9g,%.9g],"
                "[%.9g,%.9g,%.9g]],"
                "\"color\":[[%u,%u,%u,%u],[%u,%u,%u,%u]],"
                "\"texcoord\":[",
                v ? "," : "", d->largest_triangle[v].pos_mtx,
                obj[0], obj[1], obj[2], mv[0], mv[1], mv[2],
                clip[0], clip[1], clip[2], clip[3],
                screen[0], screen[1], screen[2],
                d->largest_triangle[v].normal[0][0],
                d->largest_triangle[v].normal[0][1],
                d->largest_triangle[v].normal[0][2],
                d->largest_triangle[v].normal[1][0],
                d->largest_triangle[v].normal[1][1],
                d->largest_triangle[v].normal[1][2],
                d->largest_triangle[v].normal[2][0],
                d->largest_triangle[v].normal[2][1],
                d->largest_triangle[v].normal[2][2],
                d->largest_triangle[v].color[0][0],
                d->largest_triangle[v].color[0][1],
                d->largest_triangle[v].color[0][2],
                d->largest_triangle[v].color[0][3],
                d->largest_triangle[v].color[1][0],
                d->largest_triangle[v].color[1][1],
                d->largest_triangle[v].color[1][2],
                d->largest_triangle[v].color[1][3]);
            for (u32 tc = 0; tc < 8u; ++tc) {
                n = debug_json_append(out, cap, n,
                    "%s[%.9g,%.9g,%.9g]", tc ? "," : "",
                    d->largest_triangle[v].texcoord[tc][0],
                    d->largest_triangle[v].texcoord[tc][1],
                    d->largest_triangle[v].texcoord[tc][2]);
            }
            n = debug_json_append(out, cap, n, "]}");
        }
        n = debug_json_append(out, cap, n,
            "]},\"genmode\":%u,\"alpha_test\":%u,\"zmode\":%u,"
            "\"blendmode\":%u,\"dstalpha\":%u,\"pecontrol\":%u,\"texgens\":%u,"
            "\"colchans\":%u,\"tev_registers\":[",
            d->genmode, d->alpha_test, d->zmode, d->blendmode,
            d->dstalpha, d->pecontrol, d->numtexgens, d->numcolchans);
        for (u32 reg = 0; reg < 4u; ++reg) {
            n = debug_json_append(out, cap, n,
                "%s{\"index\":%u,\"bp_ra\":%u,\"bp_bg\":%u,"
                "\"reg\":[%d,%d,%d,%d],\"konst\":[%d,%d,%d,%d]}",
                reg ? "," : "", reg, d->bp_tev_ra[reg], d->bp_tev_bg[reg],
                d->tev_reg[reg][0], d->tev_reg[reg][1],
                d->tev_reg[reg][2], d->tev_reg[reg][3],
                d->tev_konst[reg][0], d->tev_konst[reg][1],
                d->tev_konst[reg][2], d->tev_konst[reg][3]);
        }
        n = debug_json_append(out, cap, n, "],\"stages\":[");
        for (u32 st = 0; st < d->numtevstages && st < 16u; ++st) {
            n = debug_json_append(out, cap, n,
                "%s{\"index\":%u,\"order\":%u,\"texcoord\":%u,"
                "\"texmap\":%u,\"enable\":%u,\"colorchan\":%u,"
                "\"cc\":%u,\"ac\":%u,\"tevind\":%u,"
                "\"ksel\":%u,\"kcsel\":%u,\"kasel\":%u,"
                "\"konst\":[%d,%d,%d,%d]}",
                st ? "," : "", st, d->stage[st].order,
                d->stage[st].texcoord, d->stage[st].texmap,
                d->stage[st].enable, d->stage[st].colorchan,
                d->stage[st].cc, d->stage[st].ac,
                d->stage[st].tevind, d->stage[st].ksel,
                d->stage[st].kcsel, d->stage[st].kasel,
                d->stage[st].konst[0], d->stage[st].konst[1],
                d->stage[st].konst[2], d->stage[st].konst[3]);
        }
        n = debug_json_append(out, cap, n, "],\"textures\":[");
        int tex_emitted = 0;
        for (u32 unit = 0; unit < 8u; ++unit) {
            const GxDebugTexture* t = &d->tex[unit];
            if (!t->active)
                continue;
            n = debug_json_append(out, cap, n,
                "%s{\"unit\":%u,\"valid\":%s,\"fmt\":%u,"
                "\"width\":%u,\"height\":%u,\"phys\":%u,\"bytes\":%u,"
                "\"hash\":\"%016llx\",\"mode0\":%u,\"mode1\":%u,"
                "\"image0\":%u,\"image3\":%u,\"tlut\":%u,\"sample\":\"",
                tex_emitted++ ? "," : "", unit,
                t->valid ? "true" : "false", t->fmt, t->width, t->height,
                t->phys, t->bytes, (unsigned long long)t->hash,
                t->mode0, t->mode1, t->image0, t->image3, t->tlut);
            for (u32 b = 0; b < t->sample_len && (size_t)n + 2u < cap; ++b) {
                out[n++] = hex[t->sample[b] >> 4];
                out[n++] = hex[t->sample[b] & 15u];
            }
            n = debug_json_append(out, cap, n, "\"}");
        }
        n = debug_json_append(out, cap, n, "]}");
    }
    {
        u64 recent_frame = 0;
        for (u32 i = 0; i < s_debug_recent_count; ++i) {
            const GxDebugRecent* r =
                &s_debug_recent[(s_debug_recent_head - s_debug_recent_count + i) %
                                GX_DEBUG_RECENT_MAX];
            if (r->frame < s_debug_recent_latest_frame &&
                r->frame > recent_frame)
                recent_frame = r->frame;
        }
        if (recent_frame == 0)
            recent_frame = s_debug_recent_latest_frame;
        n = debug_json_append(out, cap, n,
            "],\"recent_frame\":%llu,\"recent\":[",
            (unsigned long long)recent_frame);
        int recent_emitted = 0;
        for (u32 i = 0; i < s_debug_recent_count; ++i) {
            const GxDebugRecent* r =
                &s_debug_recent[(s_debug_recent_head - s_debug_recent_count + i) %
                                GX_DEBUG_RECENT_MAX];
            if (r->frame != recent_frame)
                continue;
            if (recent_emitted++)
                n = debug_json_append(out, cap, n, ",");
            n = debug_json_append_recent(out, cap, n, r);
        }
        u64 large_frame = 0;
        for (u32 i = 0; i < s_debug_large_count; ++i) {
            const GxDebugRecent* r =
                &s_debug_large[(s_debug_large_head - s_debug_large_count + i) %
                               GX_DEBUG_LARGE_MAX];
            if (r->frame < s_debug_large_latest_frame &&
                r->frame > large_frame)
                large_frame = r->frame;
        }
        if (large_frame == 0)
            large_frame = s_debug_large_latest_frame;
        n = debug_json_append(out, cap, n,
            "],\"large_frame\":%llu,\"large\":[",
            (unsigned long long)large_frame);
        int large_emitted = 0;
        for (u32 i = 0; i < s_debug_large_count; ++i) {
            const GxDebugRecent* r =
                &s_debug_large[(s_debug_large_head - s_debug_large_count + i) %
                               GX_DEBUG_LARGE_MAX];
            if (r->frame != large_frame)
                continue;
            if (large_emitted++)
                n = debug_json_append(out, cap, n, ",");
            n = debug_json_append_recent(out, cap, n, r);
        }
        n = debug_json_append(out, cap, n, "]}\n");
    }
    return n;
}

/* Shared subset of build_draw_cfg() the EFB-copy path needs: pixel_format /
 * color_update / alpha_update / z-update back GetPixelColor & friends, used by
 * BOTH gx_raster_draw (s_pf/s_zt_upd/s_bm_cu/s_bm_au, set above) and here —
 * "gx_raster_efb_copy entry for its own config" (CLAUDE.md gx-raster task). */
static void build_efb_cfg(void) {
    s_pf     = pixel_format();
    s_zt_upd = zm_update_enable();
    s_bm_cu  = bm_color_update();
    s_bm_au  = bm_alpha_update();
}

/* GCN_GX_STATS: gx_raster_draw's actual body, wrapped below by the public
 * entry point so the vtx/clip-vs-triangle split (see the big comment near the
 * top of the file) has a single measurement point regardless of which of this
 * function's several early-return paths (bad primitive, vat<3 verts,
 * load_vertex failure, normal completion) is taken. */
static int position_triangle_survives(const OutVtx* v0, const OutVtx* v1,
                                      const OutVtx* v2) {
    if ((calc_clip_mask(v0) & calc_clip_mask(v1) & calc_clip_mask(v2)) != 0)
        return 0;
    int backface = is_backface(v0, v1, v2);
    u32 cull = s_cfg.cullmode;
    return backface ? (cull != 2 && cull != 3) :
                      (cull != 1 && cull != 3);
}

static void transform_vertex_rest(const InVtx* in, OutVtx* out) {
    tf_normal(in, out);
    tf_color(in, out);
    tf_texcoord(in, out);
}

/* Exact late-menu XF state. Both R/S texgens source transformed normal, so
 * their submitted texcoord index is dead. S is unlit register color; R uses
 * one lit color channel. Guard every word that makes those reductions true. */
static int rs_exact_xf(u32 program) {
    u32 expected_color = program == 18u ? 0x00000506u : 0x00000500u;
    return (program == 18u || program == 19u) &&
           gm_numcolchans() == 1u && (s_xf[0x103f] & 0xfu) == 1u &&
           s_xf[0x1012] == 0x00000001u &&
           s_xf[0x1040] == 0x00000084u && s_xf[0x1050] == 0x0000003Du &&
           s_xf[0x100e] == expected_color && s_xf[0x1010] == 0x00000500u;
}

typedef struct {
    float normal[9];
    float tex[12];
    float post[12];
    float s_scale, t_scale;
} RsTransform;

static void prepare_rs_transform(const GxCpState* cp, u8 pos_mtx,
                                 RsTransform* tf) {
    get_normmat(pos_mtx, tf->normal);
    get_texmat((u8)bits(cp->matrix_index_a, 6, 6), tf->tex);
    get_postmat(61u, tf->post);   /* exact post0 word 0x3D, normalize=0 */
    tf->s_scale = (float)(bits(s_bp[0x30], 0, 16) + 1u);
    tf->t_scale = (float)(bits(s_bp[0x31], 0, 16) + 1u);
}

static void transform_rs_vertex_rest(const InVtx* in, OutVtx* out,
                                     u32 program, const RsTransform* tf) {
    mul_vec3_mat33(in->normal[0], tf->normal, out->normal[0]);
    normalize3(out->normal[0]);
    if (program == 19u) {
        u32 matc = s_xf[0x100c];
        out->color[0][0] = (u8)(matc >> 24);
        out->color[0][1] = (u8)(matc >> 16);
        out->color[0][2] = (u8)(matc >> 8);
        out->color[0][3] = (u8)matc;
    } else {
        tf_color_n(in, out, 1u);
    }
    /* Exact tg0=0x84: Regular, source=normal, ABC1, ST projection. Exact
     * dual/post0=0x3D: post matrix 61, no normalization. */
    float tmp[3];
    mul_vec3_mat24(out->normal[0], tf->tex, tmp);
    mul_vec3_mat34(tmp, tf->post, out->texCoords[0]);
    if (out->texCoords[0][2] == 0.0f) {
        float x = out->texCoords[0][0] / 2.0f;
        float y = out->texCoords[0][1] / 2.0f;
        out->texCoords[0][0] = x < -1 ? -1 : x > 1 ? 1 : x;
        out->texCoords[0][1] = y < -1 ? -1 : y > 1 ? 1 : y;
    }
    out->texCoords[0][0] *= tf->s_scale;
    out->texCoords[0][1] *= tf->t_scale;
}

static void gx_raster_draw_impl(const GxCpState* cp, u32 prim, u32 vat,
                    const u8* verts, u32 nverts, u32 vstride) {
    if (s_draw_stats && prim < 8u)
        s_draw_shapes[prim][nverts < 16u ? nverts : 16u]++;
    /* prim 0/1 = GX_DRAW_QUADS / GX_DRAW_QUADS_2 (SetupUnit.cpp:34-40 routes
     * both to SetupQuad — Dolphin itself treats QUADS_2 as a non-standard
     * alias, not a distinct assembly). prim 4 = GX_DRAW_TRIANGLE_FAN.
     * prim 5 = GX_DRAW_LINES (SetupLine: every vertex pair -> ProcessLine).
     * Triangles/strip/linestrip/points stay trapped until observed. */
    s_trap_cp = cp; s_trap_vat = vat; s_trap_prim = prim;

    int is_quad = (prim == 0 || prim == 1);
    int is_line = (prim == 5);
    int is_tri = (prim == 2);
    int is_tri_strip = (prim == 3);
    int is_line_strip = (prim == 6);
    if (!is_quad && prim != 4 && !is_line && !is_tri &&
        !is_tri_strip && !is_line_strip) {
        TRAPF(nonfan, "unsupported primitive (prim %u opcode-class, vat %u, "
              "%u verts) pc=0x%08X vcd_lo=0x%08X vcd_hi=0x%08X",
              prim, vat, nverts, s_cpu ? s_cpu->pc : 0u, cp->vtx_desc_lo, cp->vtx_desc_hi);
        return;
    }
    /* VAT index != 0 is fully handled — every VAT field below is read from
     * vat_g0/g1/g2[vat] — but log the first occurrence once with its format
     * words so the inventory records which VATs the firmware exercises. */
    if (vat != 0) {
        static int vat_note = 0;
        if (!vat_note) {
            vat_note = 1;
            fprintf(stderr, "gx_raster: note: first VAT!=0 draw (vat %u, prim %u, %u verts) "
                    "pc=0x%08X vcd_lo=0x%08X vcd_hi=0x%08X vat_g0=0x%08X vat_g1=0x%08X "
                    "vat_g2=0x%08X — handled\n",
                    vat, prim, nverts, s_cpu ? s_cpu->pc : 0u, cp->vtx_desc_lo,
                    cp->vtx_desc_hi, cp->vat_g0[vat], cp->vat_g1[vat], cp->vat_g2[vat]);
        }
    }
    if (nverts < ((is_line || is_line_strip) ? 2u : 3u)) return;

    /* BP register loads are separate FIFO commands, so no BP-derived state
     * can change between two draws unless gx_on_bp() advanced the generation.
     * The late IPL menus issue thousands of tiny draws under one unchanged
     * state; decoding all stages, textures, swaps, scissor and carry analysis
     * for each was pure repetition. TEV census mode deliberately rebuilds so
     * its per-config draw counters retain their diagnostic meaning. */
    advance_texel_cache_generation();
    tev_load_registers(&s_tev_w[0]);
    if (s_cfg_bp_generation != s_bp_generation || s_tev_census == 1) {
        ++s_cfg_cache_misses;
        recompute_scissor();
        build_draw_cfg();
        s_cfg_bp_generation = s_bp_generation;
    } else {
        ++s_cfg_cache_hits;
    }
    debug_capture_draw(prim, vat, verts, nverts, vstride);

    /* Late-menu programs R/S submit millions of 3/4-vertex fans, most wholly
     * clipped or culled. Position/cull depends only on tf_position output, so
     * defer normal/lighting/color/texcoord work until a triangle is known to
     * survive. Surviving triangles still use the unchanged clipping path. */
    u32 gpu_program = fused_program_id();
    if (prim == 4u && (nverts == 3u || nverts == 4u) &&
        (gpu_program == 18u || gpu_program == 19u)) {
        InVtx in[4];
        OutVtx out[4];
        PositionTransform position_tf;
        int positions_ok = 1;
        for (u32 i = 0; i < nverts; ++i) {
            if (!load_indexed_float_position_only(
                    cp, vat, verts + (u64)i * vstride, vstride, &in[i])) {
                positions_ok = 0;
                break;
            }
            if (i == 0u)
                prepare_position_transform(in[i].posMtx, &position_tf);
            else if (in[i].posMtx != in[0].posMtx) {
                positions_ok = 0;
                break;
            }
            memset(&out[i], 0, sizeof out[i]);
            tf_position_prepared(&in[i], &out[i], &position_tf);
        }
        if (positions_ok) {
            int keep0 = position_triangle_survives(&out[0], &out[1], &out[2]);
            int keep1 = nverts == 4u &&
                        position_triangle_survives(&out[0], &out[2], &out[3]);
            u32 needed = (keep0 ? 0x7u : 0u) | (keep1 ? 0xDu : 0u);
            int exact_xf = rs_exact_xf(gpu_program);
            RsTransform rs_tf;
            if (exact_xf && needed != 0u)
                prepare_rs_transform(cp, in[0].posMtx, &rs_tf);
            for (u32 i = 0; i < nverts; ++i) {
                if (needed & (1u << i)) {
                    const u8* vertex = verts + (u64)i * vstride;
                    if (exact_xf && load_rs_vertex_rest(
                            cp, vat, vertex, vstride, &in[i])) {
                        transform_rs_vertex_rest(&in[i], &out[i], gpu_program,
                                                 &rs_tf);
                    } else {
                        if (!load_vertex(cp, vat, vertex, vstride, &in[i]))
                            return;
                        transform_vertex_rest(&in[i], &out[i]);
                    }
                }
            }
            if (keep0)
                process_triangle(&s_tev_w[0], &out[0], &out[1], &out[2]);
            if (keep1)
                process_triangle(&s_tev_w[0], &out[0], &out[2], &out[3]);
            return;
        }
    }

    /* Lists and strips have no persistent fan anchor.  A three-slot ring is
     * sufficient for all of them and preserves the strip's alternating
     * winding: even triangles are (n-2,n-1,n), odd are (n-2,n,n-1). */
    if (is_tri || is_tri_strip || is_line_strip) {
        OutVtx ring[3];
        for (u32 i = 0; i < nverts; i++) {
            InVtx in;
            OutVtx* cur = &ring[i % 3u];
            if (!load_vertex(cp, vat, verts + (u64)i * vstride, vstride, &in))
                return;
            memset(cur, 0, sizeof *cur);
            tf_position(&in, cur);
            tf_normal(&in, cur);
            tf_color(&in, cur);
            tf_texcoord(&in, cur);

            if (is_line_strip) {
                if (i != 0u)
                    process_line(&s_tev_w[0], &ring[(i - 1u) % 3u], cur);
            } else if (is_tri) {
                if (i % 3u == 2u)
                    process_triangle(&s_tev_w[0], &ring[(i - 2u) % 3u],
                                     &ring[(i - 1u) % 3u], cur);
            } else if (i >= 2u) {
                OutVtx* a = &ring[(i - 2u) % 3u];
                OutVtx* b = &ring[(i - 1u) % 3u];
                if (i & 1u)
                    process_triangle(&s_tev_w[0], a, cur, b);
                else
                    process_triangle(&s_tev_w[0], a, b, cur);
            }
        }
        return;
    }

    /* SetupUnit vertex assembly (SetupUnit.cpp:12-130): v0 stays in store[0]
     * for the whole call (its slot is never reassigned by either path); only
     * pv[1]/pv[2] and the write pointer are juggled per triangle. */
    OutVtx store[3];
    OutVtx* pv[3] = { &store[0], &store[1], &store[2] };
    OutVtx* writep = pv[0];
    u32 counter = 0;

    for (u32 i = 0; i < nverts; i++) {
        InVtx in;
        if (!load_vertex(cp, vat, verts + (u64)i * vstride, vstride, &in)) return;

        memset(writep, 0, sizeof *writep);        /* GetVertex() memsets */
        tf_position(&in, writep);
        tf_normal(&in, writep);
        tf_color(&in, writep);
        tf_texcoord(&in, writep);

        if (is_line) {
            /* SetupLine (SetupUnit.cpp:132-145): every vertex PAIR becomes
             * one line; the write pointer snaps back to slot 0 after each. */
            if (counter < 1) { counter++; writep = pv[counter]; continue; }
            process_line(&s_tev_w[0], pv[0], pv[1]);
            counter = 0;
            writep = pv[0];
        } else if (is_quad) {
            /* SetupQuad (SetupUnit.cpp:62-79): triangle 1 = (v0,v1,v2), then
             * triangle 2 = (v0,v2,v3) — the VertPointer/counter/write-pointer
             * state returns to its initial layout every 4 vertices (traced by
             * hand against the transcribed steps below), so additional quads
             * in the same draw call repeat identically. */
            if (counter < 2) { counter++; writep = pv[counter]; continue; }
            process_triangle(&s_tev_w[0], pv[0], pv[1], pv[2]);
            counter++;
            counter &= 3;
            writep = &store[counter & 1];
            OutVtx* temp = pv[1];
            pv[1] = pv[2];
            pv[2] = temp;
        } else {
            /* SetupTriFan (SetupUnit.cpp:114-130). */
            if (counter < 2) { counter++; writep = pv[counter]; continue; }
            process_triangle(&s_tev_w[0], pv[0], pv[1], pv[2]);
            counter++;
            pv[1] = pv[2];
            pv[2] = &store[2 - (counter & 1)];
            writep = pv[2];
        }
    }
}

/* GCN_GX_STATS: total gx_raster_draw_impl wall time minus the triangle-scan/
 * pixel time already accumulated into s_tsc_tri by draw_triangle (before/after
 * subtraction — same technique gx.c uses to isolate its own DECODE bucket from
 * the nested DRAW/EFB ones) gives the vertex load+transform+clip time exactly,
 * for every early-return path uniformly (see gx_raster_draw_impl's comment). */
void gx_raster_draw(const GxCpState* cp, u32 prim, u32 vat,
                    const u8* verts, u32 nverts, u32 vstride) {
    if (s_draw_stats < 0)
        s_draw_stats = getenv("GCN_GX_STATS") ? 1 : 0;
    /* GCN_GX_PIXEL_STATS: own cached getenv, resolved here (the one entry
     * point every pixel-level call below is reachable from) rather than
     * piggybacked on s_draw_stats — see the big comment near its statics for
     * why the two knobs must never share a branch. */
    if (s_pixel_stats < 0)
        s_pixel_stats = getenv("GCN_GX_PIXEL_STATS") ? 1 : 0;

    /* GX-MT: resolve thread count + spawn workers once, AFTER the stats knobs
     * above (it must see them to force serial under measurement modes). */
    if (s_mt_threads < 0)
        gx_mt_resolve();

    if (!s_draw_stats) {
        gx_raster_draw_impl(cp, prim, vat, verts, nverts, vstride);
        debug_finalize_draw();
        return;
    }

    u64 tri_before = s_tsc_tri;
    u64 t0 = __rdtsc();
    gx_raster_draw_impl(cp, prim, vat, verts, nverts, vstride);
    debug_finalize_draw();
    u64 total = __rdtsc() - t0;
    u64 tri_delta = s_tsc_tri - tri_before;
    s_tsc_vtx += (total > tri_delta) ? (total - tri_delta) : 0;
    s_draw_calls_stat++;
}

/* Expose the accumulators above to gx.c, which prints them as a
 * "[gx-draw-stats]" line at the same cadence as its own "[gx-stats]" summary
 * (gcn_gx_tick). All zero if GCN_GX_STATS was never set (draw_calls==0 tells
 * the caller there is nothing meaningful to print yet, same convention as
 * gx.c's own tot>0 guard). */
void gx_raster_get_draw_stats(u64* tsc_vtx, u64* tsc_tri, u64* pixels_shaded, u64* draw_calls) {
    if (tsc_vtx) *tsc_vtx = s_tsc_vtx;
    if (tsc_tri) *tsc_tri = s_tsc_tri;
    if (pixels_shaded) *pixels_shaded = s_pixels_shaded;
    if (draw_calls) *draw_calls = s_draw_calls_stat;
}

void gx_raster_get_config_cache_stats(u64* hits, u64* misses) {
    if (hits) *hits = s_cfg_cache_hits;
    if (misses) *misses = s_cfg_cache_misses;
}

void gx_raster_print_draw_shape_stats(void) {
    if (!s_draw_stats) return;
    fprintf(stderr, "[gx-draw-shapes]");
    for (u32 prim = 0; prim < 8u; ++prim)
        for (u32 nv = 0; nv <= 16u; ++nv)
            if (s_draw_shapes[prim][nv])
                fprintf(stderr, " p%u/n%u%s=%llu", prim,
                        nv, nv == 16u ? "+" : "",
                        (unsigned long long)s_draw_shapes[prim][nv]);
    fprintf(stderr, "\n");
}

/* Sibling getter for the EFB-bucket copy-vs-clear split (see s_tsc_efb_clear's
 * comment near the top of the file). *tsc_efb_clear is the clear-only slice
 * of gx.c's own GX_STAT_EFB accumulator; the caller derives the copy-encode
 * slice as (GX_STAT_EFB total - *tsc_efb_clear). Zero if GCN_GX_STATS was
 * never set. */
void gx_raster_get_efb_clear_stats(u64* tsc_efb_clear, u64* efb_clear_calls) {
    if (tsc_efb_clear) *tsc_efb_clear = s_tsc_efb_clear;
    if (efb_clear_calls) *efb_clear_calls = s_efb_clear_calls;
}

/* Sibling getter for GCN_GX_PIXEL_STATS (own knob, see the big comment near
 * its statics). gx.c prints these as a "[gx-pixel-stats]" line at the same
 * 2^20-tick cadence as "[gx-stats]"/"[gx-draw-stats]". All zero if
 * GCN_GX_PIXEL_STATS was never set (out->shaded == 0 tells the caller there is
 * nothing meaningful to print, same tot>0-style guard as the other two). */
void gx_raster_get_pixel_stats(GxPixelStats* out) {
    if (!out) return;
    out->tsc_block = s_tsc_block;
    out->tsc_slope = s_tsc_slope;
    out->tsc_tex   = s_tsc_tex;
    out->tsc_comb  = s_tsc_comb;
    out->tsc_blend = s_tsc_blend;
    out->tex_calls = s_ps_tex_calls;
    out->tex_linear = s_ps_tex_linear;
    out->tex_point  = s_ps_tex_point;
    out->earlyz_rejected = s_ps_earlyz_rejected;
    out->shaded = s_ps_shaded;
    out->blend_writes = s_ps_blend_writes;
    out->texel_cache_hits   = s_ps_texel_cache_hits;
    out->texel_cache_misses = s_ps_texel_cache_misses;
}

/* GCN_GX_TEV_CENSUS: dump the per-config draw/pixel counters (see the census
 * comment near s_census). Called from gx.c's shared stats cadence; no-op
 * unless the knob is on and at least one draw has been censused. */
/* Frame boundary hook for the per-frame coverage anomaly detector (called
 * from gx.c at every accepted GXSetDrawDone, on the decode thread). Compares
 * this frame's accumulated bbox-area sum against the rolling median of the
 * last 32 frames; +/-33% deviation logs the frame with its top-8 draws. */
int gx_raster_frame_anomaly_mark(u64 frame) {
    int anomalous = 0;
    u64 sum = s_fa_frame_area;
    /* rolling median of the last 32 sums (insertion copy — 32 elems) */
    u64 sorted[32];
    u32 n = s_fa_hist_n < 32u ? s_fa_hist_n : 32u;
    for (u32 i = 0; i < n; i++) sorted[i] = s_fa_hist[i];
    for (u32 i = 1; i < n; i++) {
        u64 key = sorted[i]; u32 j = i;
        while (j > 0 && sorted[j - 1] > key) { sorted[j] = sorted[j - 1]; j--; }
        sorted[j] = key;
    }
    u64 med = n ? sorted[n / 2] : sum;
    if (n >= 8u && med && (sum > med + med / 3u || sum + med / 3u < med)) {
        /* The caller's draw-log dump is reserved for EXTREME deviation: the
         * flood/drop corruption runs 3-57x off median, while the menu's own
         * glyph-cascade animation legitimately swings within ~1.6x — dumping
         * on the mild band burned the whole dump budget before the first
         * real flood (observed: 24/24 dumps spent by anomaly #24, flood was
         * anomaly #197). */
        anomalous = (sum > med * 3u || sum * 3u < med) ? 1 : 0;
        s_fa_anomalies++;
        /* Detection + the top-8 ring stay always-on (always-on-rings rule);
         * the verbose stderr dump is opt-in (GCN_GX_FRAMEANOM=1) now that
         * the flood bug it hunted is fixed — legit scene transitions
         * (menu zooms) trip the mild band every time and were spamming the
         * console on ordinary navigation. */
        static int s_fa_print = -1;
        if (s_fa_print < 0) s_fa_print = getenv("GCN_GX_FRAMEANOM") ? 1 : 0;
        if (s_fa_print && s_fa_anomalies <= 512u) {
            fprintf(stderr,
                "[gx-frameanom] #%llu frame=%llu area=%llu median=%llu tris=%u\n",
                (unsigned long long)s_fa_anomalies, (unsigned long long)frame,
                (unsigned long long)sum, (unsigned long long)med,
                s_fa_frame_tris);
            for (int k = 0; k < 8 && s_fa_top[k].area; k++)
                fprintf(stderr,
                    "  top[%d] area=%u prog=%u dl=%08X pmtx=(%u,%u,%u) "
                    "bbox=[%d,%d..%d,%d] "
                    "obj0=(%.2f,%.2f,%.2f) obj1=(%.2f,%.2f,%.2f) "
                    "obj2=(%.2f,%.2f,%.2f) "
                    "mv0=(%.2f,%.2f,%.2f) mv1=(%.2f,%.2f,%.2f) "
                    "mv2=(%.2f,%.2f,%.2f) w=(%.4f,%.4f,%.4f)\n",
                    k, s_fa_top[k].area, s_fa_top[k].prog, s_fa_top[k].dl,
                    s_fa_top[k].pidx[0], s_fa_top[k].pidx[1], s_fa_top[k].pidx[2],
                    s_fa_top[k].minx, s_fa_top[k].miny,
                    s_fa_top[k].maxx, s_fa_top[k].maxy,
                    s_fa_top[k].op[0][0], s_fa_top[k].op[0][1], s_fa_top[k].op[0][2],
                    s_fa_top[k].op[1][0], s_fa_top[k].op[1][1], s_fa_top[k].op[1][2],
                    s_fa_top[k].op[2][0], s_fa_top[k].op[2][1], s_fa_top[k].op[2][2],
                    s_fa_top[k].mv[0][0], s_fa_top[k].mv[0][1], s_fa_top[k].mv[0][2],
                    s_fa_top[k].mv[1][0], s_fa_top[k].mv[1][1], s_fa_top[k].mv[1][2],
                    s_fa_top[k].mv[2][0], s_fa_top[k].mv[2][1], s_fa_top[k].mv[2][2],
                    s_fa_top[k].w[0], s_fa_top[k].w[1], s_fa_top[k].w[2]);
            fflush(stderr);
        }
    }
    s_fa_hist[s_fa_hist_n % 32u] = sum;
    s_fa_hist_n++;
    s_fa_frame_area = 0;
    s_fa_frame_tris = 0;
    memset(s_fa_top, 0, sizeof s_fa_top);
    return anomalous;
}

void gx_raster_print_census(void) {
    if (s_tev_census != 1) return;
    u64 total_px = 0, total_fused = 0;
    for (int i = 0; i < GX_CENSUS_MAX; i++)
        if (s_census[i].used) { total_px += s_census[i].pixels; total_fused += s_census[i].fused_pixels; }
    if (total_px == 0) return;
    fprintf(stderr, "[gx-census]");
    for (int i = 0; i < GX_CENSUS_MAX; i++) {
        if (!s_census[i].used) continue;
        /* fused_pixels (added for the fused-pixel-path perf task) is the
         * per-bucket coverage the task's VERIFY step asks to report: how
         * many of this bucket's shaded pixels took a fused_pixel_A/B/C
         * specialization instead of the general tev_draw(). */
        fprintf(stderr, " #%d:%08x prog=%u draws=%llu px=%llu(%.1f%%) fused=%llu(%.1f%%)",
                i, s_census[i].hash, s_census[i].program_id,
                (unsigned long long)s_census[i].draws,
                (unsigned long long)s_census[i].pixels,
                100.0 * (double)s_census[i].pixels / (double)total_px,
                (unsigned long long)s_census[i].fused_pixels,
                s_census[i].pixels
                    ? 100.0 * (double)s_census[i].fused_pixels / (double)s_census[i].pixels
                    : 0.0);
    }
    fprintf(stderr, " | total_fused=%llu/%llu(%.1f%%)\n",
            (unsigned long long)total_fused, (unsigned long long)total_px,
            100.0 * (double)total_fused / (double)total_px);
    if (s_census_overflow_draws)
        fprintf(stderr, "[gx-census] census overflow: %llu draws in %d unseen shapes "
                "(table has %d slots, all full)%s\n",
                (unsigned long long)s_census_overflow_draws, s_census_overflow_shapes,
                GX_CENSUS_MAX,
                s_census_overflow_shapes >= GX_CENSUS_OVERFLOW_MAX ? " [shape count capped]" : "");
    fflush(stderr);
}

/* GCN_GX_MT_STATS: fork/join accounting (see s_mt_stats — main-thread-only
 * counters, safe and meaningful under a real MT run, unlike the per-pixel
 * knobs which force serial). fork% of triangles, and the main thread's
 * average per-fork cost split into own-scan vs join-wait vs publish overhead. */
void gx_raster_print_mt_stats(void) {
    if (s_mt_stats != 1 || s_mt_forks == 0) return;
    u64 pub = s_mt_fork_tsc - s_mt_scan_tsc - s_mt_join_tsc;
    fprintf(stderr,
        "[gx-mt-stats] threads=%d forks=%llu serial_tris=%llu"
        "  per-fork tsc: scan=%llu join=%llu publish=%llu\n",
        s_mt_threads,
        (unsigned long long)s_mt_forks, (unsigned long long)s_mt_serial_tris,
        (unsigned long long)(s_mt_scan_tsc / s_mt_forks),
        (unsigned long long)(s_mt_join_tsc / s_mt_forks),
        (unsigned long long)(pub / s_mt_forks));
    fflush(stderr);
}

/* GCN_GX_STATS: dump the per-triangle bbox-area histogram (see s_hist_*
 * comment near the top of the file). One line, non-empty buckets only —
 * bucket label 2^k means area in [2^(k-1), 2^k). Called from gx.c's shared
 * stats cadence next to the census; no-op unless GCN_GX_STATS is on and at
 * least one triangle was binned. */
void gx_raster_print_area_hist(void) {
    if (s_draw_stats != 1) return;
    u64 total_tris = 0, total_tsc = 0;
    for (int i = 0; i < GX_AREA_HIST_BUCKETS; i++) {
        total_tris += s_hist_tris[i];
        total_tsc  += s_hist_tsc[i];
    }
    if (total_tris == 0) return;
    fprintf(stderr, "[gx-area-hist]");
    for (int i = 0; i < GX_AREA_HIST_BUCKETS; i++) {
        if (!s_hist_tris[i]) continue;
        fprintf(stderr, " 2^%d:tris=%llu px=%llu tsc=%.1f%%",
                i, (unsigned long long)s_hist_tris[i],
                (unsigned long long)s_hist_pixels[i],
                total_tsc ? 100.0 * (double)s_hist_tsc[i] / (double)total_tsc : 0.0);
    }
    fprintf(stderr, " | tris=%llu\n", (unsigned long long)total_tris);
    fflush(stderr);
}

/* ============================================================================
 * EFB copy (EfbCopy.cpp + SWEfbInterface.cpp EncodeXFB). copy-then-clear per
 * BPStructs.cpp:240-395.
 * ==========================================================================*/
/* ConvertColorToYUV (SWEfbInterface.cpp:546-562). color = 0xRRGGBBAA. */
static void color_to_yuv(u32 color, int* Y, int* U, int* V) {
    int r = (int)((color >> 24) & 0xff);
    int g = (int)((color >> 16) & 0xff);
    int b = (int)((color >> 8) & 0xff);
    int y = 66 * r + 129 * g + 25 * b;
    int u = -38 * r - 74 * g + 112 * b;
    int vv = 112 * r - 94 * g - 18 * b;
    *Y = (u8)((y >> 8) + ((y >> 7) & 1));
    *U = (s8)((u >> 8) + ((u >> 7) & 1));
    *V = (s8)((vv >> 8) + ((vv >> 7) & 1));
}

/* const-u8*-taking adapter so set_pixel_alpha_only_rgba6 (u32,u8) fits the
 * same `void(*)(u32, const u8*)` shape as the color-only/alpha-color
 * formulas below — lets efb_clear_rect pick ONE function-pointer type for
 * whichever of the 3 actions this clear uses. */
static inline void set_pixel_alpha_only_rgba6_cc(u32 off, const u8* cc) {
    set_pixel_alpha_only_rgba6(off, cc[ALP_C]);
}

/* ============================================================================
 * AVX2 widening of efb_clear_rect's two SIMD passes below (color / depth) —
 * 8x32-bit lanes instead of SSE2's 4. Both passes are pure bitwise/constant
 * fills over a per-clear-rect-constant span (AND-then-OR for color, a plain
 * broadcast store for depth; see the big SSE2 comment on efb_clear_rect for
 * why this has no data-dependent per-pixel computation at all), so widening
 * the lane count changes nothing about the computed bytes — no new range
 * proof is needed the way the arithmetic EFB-copy folds below need one.
 *
 * Each helper processes only the full AVX2-wide interior [left, xr), where
 * xr = left + 8*floor((right+1-left)/8) is computed by the caller; the
 * caller then runs the existing scalar clear_px/SetPixelDepth for the
 * [xr, right] remainder (0-7 columns) — same "SIMD interior, scalar
 * remainder" split the SSE2 version already uses, just with a wider
 * interior. GCN_GX_NO_AVX2=1 (or a pre-AVX2 CPU) skips these entirely and
 * keeps the untouched SSE2 4-wide pass (which has its own scalar
 * remainder) — see gx_avx2_available's comment.
 * ==========================================================================*/
__attribute__((target("avx2")))
static void efb_clear_color_avx2(u32* efb, int left, int xr, int top, int bottom,
                                  u32 rowstride, u32 color_or) {
    const __m256i vor   = _mm256_set1_epi32((int)color_or);
    const __m256i vmask = _mm256_set1_epi32((int)0xFF000000u);
    for (int y = top; y <= bottom; y++) {
        u32 rowoff = (u32)y * rowstride;
        for (int x = left; x < xr; x += 8) {
            u32 off = rowoff + (u32)x;
            __m256i cur = _mm256_loadu_si256((const __m256i*)&efb[off]);
            _mm256_storeu_si256((__m256i*)&efb[off],
                                 _mm256_or_si256(_mm256_and_si256(cur, vmask), vor));
        }
    }
}

__attribute__((target("avx2")))
static void efb_clear_depth_avx2(u32* efb, int left, int xr, int top, int bottom,
                                  u32 rowstride, u32 depth_val) {
    const __m256i vd = _mm256_set1_epi32((int)depth_val);
    for (int y = top; y <= bottom; y++) {
        u32 rowoff = (u32)y * rowstride;
        for (int x = left; x < xr; x += 8)
            _mm256_storeu_si256((__m256i*)&efb[rowoff + (u32)x], vd);
    }
}

static void efb_clear_rect(void) {
    /* EfbCopy::ClearEfb (EfbCopy.cpp:16-34). */
    u32 ar = s_bp[0x4f], gb = s_bp[0x50];
    u32 clearColor = ((ar & 0xff) << 24) | (gb << 8) | ((ar & 0xff00) >> 8);
    u32 clearZ = s_bp[0x51];
    int left = (int)bits(s_bp[0x49], 0, 10);
    int top  = (int)bits(s_bp[0x49], 10, 10);
    int right = left + (int)bits(s_bp[0x4a], 0, 10);
    int bottom = top + (int)bits(s_bp[0x4a], 10, 10);
    if (right > (int)EFB_WIDTH - 1) right = (int)EFB_WIDTH - 1;
    if (bottom > (int)EFB_HEIGHT - 1) bottom = (int)EFB_HEIGHT - 1;
    u8 cc[4]; memcpy(cc, &clearColor, 4);

    /* Clear-parameter change census (always on, IPL flood investigation):
     * the menu's clear color/rect should be constant per screen, so every
     * CHANGE is logged with its frame. A flood frame whose background is a
     * uniform wrong color while later draws render normally means THIS
     * clear ran with garbage parameters — the log then shows the exact
     * value the guest's BP registers held (garbage color => trace the BP
     * writer; sane color+wrong placement => sequencing). */
    {
        static u32 s_prev_color = 0xDEADBEEFu;
        static int s_prev_rect[4] = {-1, -1, -1, -1};
        static u64 s_clear_changes;
        if (clearColor != s_prev_color || left != s_prev_rect[0] ||
            top != s_prev_rect[1] || right != s_prev_rect[2] ||
            bottom != s_prev_rect[3]) {
            s_clear_changes++;
            if (s_clear_changes <= 4000u)
                fprintf(stderr,
                    "[gx-clear] #%llu frame=%llu color=%08X (was %08X) "
                    "rect=[%d,%d..%d,%d] cu=%d au=%d zu=%d pf=%d z=%06X\n",
                    (unsigned long long)s_clear_changes,
                    (unsigned long long)gcn_gx_frame_count(),
                    clearColor, s_prev_color, left, top, right, bottom,
                    s_bm_cu, s_bm_au, s_zt_upd, (int)s_pf, clearZ & 0xFFFFFFu);
            s_prev_color = clearColor;
            s_prev_rect[0] = left; s_prev_rect[1] = top;
            s_prev_rect[2] = right; s_prev_rect[3] = bottom;
        }
    }

    /* Per-clear-rect pixel-store selection, hoisted OUT of the x/y loop (same
     * pattern as gx_raster_efb_copy's copy_getpx — see its big comment):
     * s_bm_cu/s_bm_au (bp 0x41, cached by build_efb_cfg) decide WHICH of the
     * 3 actions applies, and s_pf decides WHICH FORMULA that action uses —
     * both are per-clear-rect constants (bp state is provably constant for
     * the whole call, same reasoning as GetPixelColor's per-draw config
     * cache), yet SetPixelColorOnly/SetPixelAlphaColor/SetPixelAlphaOnly used
     * to re-decode s_pf via their own `switch (s_pf)` on every cleared pixel.
     * clear_px is resolved to the exact direct/rgba6 formula this clear will
     * use, once, here; NULL means "no color/alpha write this clear" — either
     * because neither cu nor au is set (matches the original: no Set* call
     * at all) or because the format is direct and the action is alpha-only
     * (SetPixelAlphaOnly's own direct-format branch is a real no-op, "RGB8/
     * Z24/RGB565 have no alpha plane"). Any s_pf outside the two known groups
     * falls back to the untouched general per-pixel path (its own switch +
     * TRAPF), so an unseen format traps exactly as loudly/correctly as
     * before this change. */
    int fmt_direct = (s_pf == PF_RGB8_Z24 || s_pf == PF_Z24 || s_pf == PF_RGB565_Z16);
    int fmt_rgba6  = (s_pf == PF_RGBA6_Z24);
    void (*clear_px)(u32, const u8*) = NULL;
    if (fmt_direct || fmt_rgba6) {
        if (s_bm_cu) {
            clear_px = s_bm_au ? (fmt_direct ? set_pixel_alpha_color_direct : set_pixel_alpha_color_rgba6)
                                : (fmt_direct ? set_pixel_color_only_direct : set_pixel_color_only_rgba6);
        } else if (s_bm_au) {
            clear_px = fmt_direct ? NULL : set_pixel_alpha_only_rgba6_cc;
        }
    }
    /* Fallback path taken only for a genuinely unrecognized s_pf AND an
     * actual color/alpha write requested (cu||au) — matches the original,
     * which never called any Set* function at all when neither cu nor au
     * was set, regardless of format. */
    int use_fallback = !fmt_direct && !fmt_rgba6 && (s_bm_cu || s_bm_au);

    /* ============================================================================
     * SSE2 clear (perf task, CLAUDE.md gx-raster residuals sweep — queued
     * since the EFB-copy SSE2 round). GCN_GX_STATS' new EFB copy-vs-clear
     * split (gx.c's "[gx-efb-stats]" line) showed the scalar clear here is
     * ~37.6% of the whole EFB bucket (~5.4% of total GX wall) on the boot
     * animation, entirely from full-viewport clears on scene transitions —
     * `left`/`top`/`right`/`bottom` are per-clear-rect CONSTANTS by this
     * point, so both writes below are pure constant fills over a contiguous
     * span, not a data-dependent per-pixel computation, which is what makes
     * this an even simpler SIMD target than the EFB-copy encode.
     *
     * Color pass: scoped to EXACTLY the fmt_direct fast path (RGB8_Z24/
     * Z24/RGB565_Z16 — same group get_pixel_color_direct/copy_getpx's SIMD
     * already special-cases, and the only group the boot animation's clears
     * are observed to use), and only when clear_px resolved to one of the
     * two direct-format setters. Both set_pixel_color_only_direct and
     * set_pixel_alpha_color_direct reduce to the IDENTICAL formula for a
     * direct pixel_format (no separate alpha plane to special-case — see
     * their definitions above), so checking clear_px against either is
     * sufficient and there is exactly one SIMD formula to write, not two:
     *   s_efb_color[off] = (s_efb_color[off] & 0xFF000000) | color_or
     * where color_or == (u32)cc-as-le-word >> 8 is the SAME per-clear-rect
     * constant clear_px would recompute from `cc` on every scalar call — cc
     * never changes mid-clear, so it is hoisted out and computed once here.
     * A straight AND-then-OR against a loaded 128-bit lane is byte-exact by
     * construction (no rounding, no lane-width truncation risk at all: it's
     * bitwise, not arithmetic), so no brute-force proof is needed here the
     * way the arithmetic TEV folds needed one — this is not a NEW formula,
     * it is the scalar formula run 4-wide. RGBA6_Z24 / any other s_pf, and
     * the neither-cu-nor-au / unrecognized-format branches, keep the exact
     * original scalar loop untouched (fmt_rgba6's bit-packing formulas do
     * not reduce to a lane-independent AND/OR the way direct's does, so
     * vectorizing them is out of scope here, same "only the observed fast
     * path" call the EFB-copy SIMD task made for copy_getpx).
     *
     * Depth pass: SetPixelDepth has NO pixel_format branch at all
     * (`s_efb_depth[off] = depth & 0x00ffffffu` unconditionally), so it is
     * always a plain masked-constant fill and always SIMD-eligible whenever
     * zmode.update_enable (s_zt_upd) is set — independent of which color
     * path (or none) this clear took.
     *
     * Color and depth are split into two SEPARATE passes over the same
     * (left..right, top..bottom) rect rather than kept fused in one x/y loop
     * like the original: s_efb_color and s_efb_depth are disjoint arrays
     * with no cross-array data dependency (a pixel's stored color never
     * depends on any depth value or vice versa), and within a single clear
     * every (x,y) location is visited exactly once, so neither the
     * intra-array write order nor the relative color-vs-depth order at a
     * given (x,y) can be observed by anything — the final byte contents of
     * both arrays are identical to the original fused loop regardless of
     * this split. This is what lets each pass pick SIMD-vs-scalar
     * independently instead of forcing a lockstep stride between two
     * formulas with different eligibility.
     *
     * GCN_GX_NO_SIMD=1 forces both passes to their scalar loops — the task's
     * same-binary A/B exactness proof (SIMD-on vs GCN_GX_NO_SIMD=1, same
     * executable, golden XFB hash must match either way), same knob the
     * EFB-copy SIMD task already established. ==========================================================================*/
    if (s_no_simd < 0) s_no_simd = getenv("GCN_GX_NO_SIMD") ? 1 : 0;

    int simd_color_ok = !s_no_simd && fmt_direct &&
                         (clear_px == set_pixel_color_only_direct ||
                          clear_px == set_pixel_alpha_color_direct);
    if (simd_color_ok) {
        u32 src; memcpy(&src, cc, 4);
        if (gx_avx2_available()) {
            int xr = left; while (xr + 8 <= right + 1) xr += 8;
            efb_clear_color_avx2(s_efb_color, left, xr, top, bottom, EFB_WIDTH, src >> 8);
            for (int y = top; y <= bottom; y++) {
                u32 rowoff = (u32)y * EFB_WIDTH;
                for (int x = xr; x <= right; x++)
                    clear_px(rowoff + (u32)x, cc);   /* scalar remainder, identical formula */
            }
        } else {
            const __m128i vor   = _mm_set1_epi32((int)(src >> 8));
            const __m128i vmask = _mm_set1_epi32((int)0xFF000000u);
            for (int y = top; y <= bottom; y++) {
                u32 rowoff = (u32)y * EFB_WIDTH;
                int x = left;
                for (; x + 4 <= right + 1; x += 4) {
                    u32 off = rowoff + (u32)x;
                    __m128i cur = _mm_loadu_si128((const __m128i*)&s_efb_color[off]);
                    _mm_storeu_si128((__m128i*)&s_efb_color[off],
                                      _mm_or_si128(_mm_and_si128(cur, vmask), vor));
                }
                for (; x <= right; x++)
                    clear_px(rowoff + (u32)x, cc);   /* scalar remainder, identical formula */
            }
        }
    } else if (clear_px) {
        for (int y = top; y <= bottom; y++)
            for (int x = left; x <= right; x++)
                clear_px((u32)x + (u32)y * EFB_WIDTH, cc);
    } else if (use_fallback) {
        for (int y = top; y <= bottom; y++) {
            for (int x = left; x <= right; x++) {
                u32 off = (u32)x + (u32)y * EFB_WIDTH;
                /* SetColor honors color_update/alpha_update (bp 0x41, cached
                 * by build_efb_cfg() into the shared s_bm_cu/s_bm_au — a
                 * per-clear-rect constant, not a per-pixel one). */
                if (s_bm_cu) {
                    if (s_bm_au) SetPixelAlphaColor(off, cc);
                    else         SetPixelColorOnly(off, cc);
                } else if (s_bm_au) {
                    SetPixelAlphaOnly(off, cc[ALP_C]);
                }
            }
        }
    }
    /* else: neither clear_px nor use_fallback -> no color/alpha write this
     * clear, matches the original (no Set* call at all in that case). */

    if (s_zt_upd) {
        /* SetDepth honors zmode.update_enable (bp 0x40, cached as s_zt_upd). */
        u32 depth_val = clearZ & 0x00ffffffu;
        if (!s_no_simd) {
            if (gx_avx2_available()) {
                int xr = left; while (xr + 8 <= right + 1) xr += 8;
                efb_clear_depth_avx2(s_efb_depth, left, xr, top, bottom, EFB_WIDTH, depth_val);
                for (int y = top; y <= bottom; y++) {
                    u32 rowoff = (u32)y * EFB_WIDTH;
                    for (int x = xr; x <= right; x++)
                        SetPixelDepth(rowoff + (u32)x, clearZ);
                }
            } else {
                const __m128i vd = _mm_set1_epi32((int)depth_val);
                for (int y = top; y <= bottom; y++) {
                    u32 rowoff = (u32)y * EFB_WIDTH;
                    int x = left;
                    for (; x + 4 <= right + 1; x += 4)
                        _mm_storeu_si128((__m128i*)&s_efb_depth[rowoff + (u32)x], vd);
                    for (; x <= right; x++)
                        SetPixelDepth(rowoff + (u32)x, clearZ);
                }
            }
        } else {
            for (int y = top; y <= bottom; y++)
                for (int x = left; x <= right; x++)
                    SetPixelDepth((u32)x + (u32)y * EFB_WIDTH, clearZ);
        }
    }
}

/* filtered YUV encode of one destination pixel column position x, EFB row y.
 * `getpx` is the per-copy-constant pixel-format formula (see
 * gx_raster_efb_copy below) — hoisted out here instead of going through
 * GetPixelColor's own `switch (s_pf)` on every call: s_pf cannot change
 * mid-copy (build_efb_cfg decodes it once, at the top of this same call —
 * see the "Per-draw config cache" comment near GetPixelColor's definition
 * for why BP state is provably constant for the whole call), yet this
 * function is invoked 3 times per output pixel column (yprev/sy/ynext taps)
 * times every column times every scanline of the copy, so the switch used to
 * re-run per tap. Bounds clamping stays here unchanged: x/y genuinely vary
 * per call and are NOT per-copy-constant. */
static u32 get_efb_color(int x, int y, u32 (*getpx)(u32)) {
    if (x < 0) x = 0;
    if (x >= (int)EFB_WIDTH) x = (int)EFB_WIDTH - 1;
    if (y < 0) y = 0;
    if (y >= (int)EFB_HEIGHT) y = (int)EFB_HEIGHT - 1;
    return getpx((u32)x + (u32)y * EFB_WIDTH);
}

/* One column of gx_raster_efb_copy's vertical-filter + YUV-encode stage,
 * pure extraction (byte-identical to the loop body it replaces) — shared by
 * the scalar (SIMD off / GCN_GX_NO_SIMD=1 / non-fast-path-format) sweep AND
 * the SIMD sweep's scalar remainder tail below, so both take the literal
 * same code path for anything not covered by a full SIMD batch. */
static inline void efb_copy_col(int x, int yprev, int sy, int ynext, u32 (*getpx)(u32),
                                 int w0, int w1, int w2, int w3, int w4, int w5, int w6,
                                 int* scanY, int* scanU, int* scanV, int i) {
    u32 c0 = get_efb_color(x, yprev, getpx);
    u32 c1 = get_efb_color(x, sy, getpx);
    u32 c2 = get_efb_color(x, ynext, getpx);
    u8 cb0[4], cb1[4], cb2[4];
    memcpy(cb0, &c0, 4); memcpy(cb1, &c1, 4); memcpy(cb2, &c2, 4);
    u8 filt[4]; filt[ALP_C] = 0;
    for (int k = BLU_C; k <= RED_C; k++) {
        int sum = cb0[k] * (w0 + w1) + cb1[k] * (w2 + w3 + w4) + cb2[k] * (w5 + w6);
        sum >>= 6; if (sum > 255) sum = 255; filt[k] = (u8)sum;
    }
    u32 fc; memcpy(&fc, filt, 4);
    int Y, U, V; color_to_yuv(fc, &Y, &U, &V);
    scanY[i] = Y; scanU[i] = U; scanV[i] = V;
}

/* One output-column-pair of the horizontal 1/4+1/2+1/4 chroma downsample +
 * YUYV pack. Pure extraction, same sharing rationale as efb_copy_col. */
static inline void efb_copy_pack(u8* row, int x, int i, const int* scanY, const int* scanU, const int* scanV) {
    int Y0 = scanY[i] + 16;
    int UV0 = 128 + ((scanU[i - 1] + (scanU[i] << 1) + scanU[i + 1]) >> 2);
    int Y1 = scanY[i + 1] + 16;
    int UV1 = 128 + ((scanV[i - 1] + (scanV[i] << 1) + scanV[i + 1]) >> 2);
    row[x * 2 + 0] = (u8)(Y0 < 0 ? 0 : Y0 > 255 ? 255 : Y0);
    row[x * 2 + 1] = (u8)(UV0 < 0 ? 0 : UV0 > 255 ? 255 : UV0);
    row[x * 2 + 2] = (u8)(Y1 < 0 ? 0 : Y1 > 255 ? 255 : Y1);
    row[x * 2 + 3] = (u8)(UV1 < 0 ? 0 : UV1 > 255 ? 255 : UV1);
}

/* ============================================================================
 * SSE2 EFB-copy scanline encode (perf task, CLAUDE.md gx-raster). Vectorizes
 * the interior of gx_raster_efb_copy's two per-scanline column loops — the
 * vertical 3-tap filter + RGB->YUV convert (efb_copy_col's body) and the
 * horizontal chroma downsample + YUYV pack (efb_copy_pack's body) — 4 EFB
 * columns (loop 1) / 4 output-pixel-pairs (loop 2) per XMM register.
 *
 * Scope: ONLY the get_pixel_color_direct pixel-format group (RGB8_Z24/Z24/
 * RGB565_Z16 — the only format the IPL boot's EFB copies ever use, see
 * copy_getpx's selection below); RGBA6_Z24 and the general-fallback formula
 * keep the untouched scalar path. get_efb_color's x/y clamps also keep the
 * scalar path: the vectorized span only ever runs where the whole scanline's
 * x range [left,right) is inside [0,EFB_WIDTH) (checked once per copy, see
 * simd_ok below), so within that span x can never hit get_efb_color's clamp
 * branch, matching the scalar formula exactly without needing to replicate
 * the clamp per lane. The first/last partial group of each loop (not a
 * multiple of 4 columns / 4 pixel-pairs) and anything outside simd_ok falls
 * through to efb_copy_col/efb_copy_pack — the identical scalar code.
 *
 * Range derivations (why 32-bit lanes throughout, and where 16-bit ops are
 * safe as an intermediate step):
 *
 *  efb_copy_col's filter sum: `cb[k]` is a u8 (0..255) tap sample; the 3
 *  weights (w0+w1), (w2+w3+w4), (w5+w6) are sums of 2-3 six-bit BP fields
 *  (0..63 each), so they bound to <=126, <=189, <=126 respectively. A SINGLE
 *  product is at most 255*189 = 48195 — exceeds a signed 16-bit lane
 *  (32767) but fits an UNSIGNED 16-bit lane (65535) exactly, so each product
 *  is computed with a 16-bit multiply (_mm_mullo_epi16 — SSE2, take the low
 *  16 bits of the true product) then zero-extended to 32-bit (the product is
 *  never negative, so zero-extension recovers the exact value). The 3-term
 *  SUM is at most 255*(126+189+126) = 112455 — needs 32-bit accumulation
 *  (paddd); >>6 and the "clamp to 255" then run in 32-bit lanes (no SSE4.1
 *  pminsd/pmaxsd available under SSE2, so the clamp is a compare+select).
 *
 *  color_to_yuv: r/g/b are u8 (0..255) filter outputs. y=66r+129g+25b maxes
 *  at 255*(66+129+25)=56100; u=-38r-74g+112b and v=112r-94g-18b both range
 *  exactly [-28560,28560] (checked at all 8 corners of {0,255}^3) — none of
 *  these fit a 16-bit lane, so the r/g PAIR of each is computed with
 *  _mm_madd_epi16 (SSE2 pmaddwd: true signed 16x16->32 multiplies, THEN a
 *  32-bit add of the pair — no 16-bit-lane truncation risk at all, unlike
 *  pmullw) against the two coefficients packed into one 16-bit-lane-pair
 *  constant; the remaining b-alone term uses a plain 16-bit multiply
 *  (individual b-coefficient products are <=28560, safely under 32768, so
 *  sign-extension after the 16-bit multiply recovers the exact signed
 *  value even for the negative b coefficient in v). All three combiner
 *  outputs then get color_to_yuv's own (y>>8)+((y>>7)&1) rounding — done in
 *  32-bit lanes with an arithmetic shift (_mm_srai_epi32, matching the
 *  scalar code's `>>` on a possibly-negative int bit-for-bit) — and its
 *  (u8)/(s8) truncating cast. That cast is a PROVEN no-op here: y>>8 maxes
 *  at 219 (+ the 0/1 rounding bit = <=220), inside u8 range with room to
 *  spare; u>>8 and v>>8 both range [-112,111] (+ rounding = [-112,112]),
 *  inside s8's [-128,127] with room to spare — so the plain computed int32
 *  IS the cast's result, no separate truncate/sign-extend step needed.
 *
 *  efb_copy_pack: scanY holds color_to_yuv's Y (proven 0..219ish, see
 *  above); Y0/Y1 = scanY[i]+16 range [16,235] — the store clamp never
 *  actually triggers but is still implemented (compare+select) for the same
 *  "identical expression, not identical-only-when-lucky" reason as the
 *  filter sum's clamp. scanU/scanV hold color_to_yuv's U/V (proven
 *  [-112,112]); the 1/4+1/2+1/4 sum U[i-1]+2U[i]+U[i+1] ranges at most
 *  [-448,448] (arithmetic >>2, matching the scalar `>>`, via _mm_srai_epi32)
 *  then +128 gives [-112,240] before the same defensive clamp. All of this
 *  fits comfortably in 32-bit lanes with no overflow anywhere, so this
 *  stage runs in plain 32-bit SIMD throughout — no 16-bit intermediate step
 *  needed at all.
 *
 * Loop 2 has no SIMD gather instruction available under SSE2, so its lane
 * layout is built from CONTIGUOUS loads + in-register shuffles instead:
 * scanY[i..i+7] loaded as two 4-lane regs already contains Y0/Y1 for 4
 * iterations in exactly the output byte order (scanY[i]=Y0 of iter0,
 * scanY[i+1]=Y1 of iter0, scanY[i+2]=Y0 of iter1, ...) with NO shuffle
 * needed; the smoothed-chroma "1/4+1/2+1/4" values are needed only at the
 * SAME stride-2 offsets, so gather_even2 (a _mm_shuffle_epi32 per input reg
 * to bring lanes {0,2} to the front, then _mm_unpacklo_epi64 to concatenate
 * the two regs' pairs) reconstructs the 4 needed values from 2 contiguous
 * loads — a shuffle-based stride-2 "gather" entirely within SSE2, at the
 * cost of pre-computing the smoothed chroma for every column (not just the
 * odd ones the scalar loop reads) in a first pass — cheap (2 adds + 1 shift
 * per column) and itself fully vectorizable with plain contiguous loads.
 * ==========================================================================*/

/* Zero/sign-extend the LOW 4 16-bit lanes of `x` to 4x32-bit lanes. */
static inline __m128i simd_zext16_lo(__m128i x) { return _mm_unpacklo_epi16(x, _mm_setzero_si128()); }
static inline __m128i simd_sext16_lo(__m128i x) { return _mm_srai_epi32(_mm_unpacklo_epi16(x, x), 16); }

/* v <- clamp(v, 0, 255), 4x32-bit lanes. No pminsd/pmaxsd under SSE2 (that's
 * SSE4.1), so this is a plain compare+select. `only_max`: caller already
 * knows v can never be negative (efb_copy_col's filter sum), so skip the
 * dead low-side compare — cheap, not a correctness-affecting shortcut (the
 * omitted compare would never fire; see the big comment above). */
static inline __m128i simd_clamp_max255(__m128i v) {
    __m128i c255 = _mm_set1_epi32(255);
    __m128i gt = _mm_cmpgt_epi32(v, c255);
    return _mm_or_si128(_mm_and_si128(gt, c255), _mm_andnot_si128(gt, v));
}
static inline __m128i simd_clamp_0_255(__m128i v) {
    __m128i zero = _mm_setzero_si128();
    v = _mm_andnot_si128(_mm_cmplt_epi32(v, zero), v);
    return simd_clamp_max255(v);
}

/* efb_copy_col's 3-tap weighted sum for one channel, 4 columns at a time.
 * a/b/c: the yprev/sy/ynext taps' channel value (4x32-bit lanes, 0..255).
 * wab/wbc/wca... see the big comment above for the range derivation. */
static inline __m128i simd_filter_sum(__m128i a, __m128i b, __m128i c, int wa, int wb, int wc) {
    __m128i ap = _mm_packs_epi32(a, a), bp = _mm_packs_epi32(b, b), cp = _mm_packs_epi32(c, c);
    __m128i pa = _mm_mullo_epi16(ap, _mm_set1_epi16((short)wa));
    __m128i pb = _mm_mullo_epi16(bp, _mm_set1_epi16((short)wb));
    __m128i pc = _mm_mullo_epi16(cp, _mm_set1_epi16((short)wc));
    __m128i sum = _mm_add_epi32(_mm_add_epi32(simd_zext16_lo(pa), simd_zext16_lo(pb)), simd_zext16_lo(pc));
    sum = _mm_srli_epi32(sum, 6);   /* logical shift == arithmetic here: sum is always >= 0 */
    return simd_clamp_max255(sum);
}

/* color_to_yuv, 4 columns at a time. R/G/B: 4x32-bit lanes, 0..255 (this
 * copy's filt[RED_C]/[GRN_C]/[BLU_C]). The scalar version computes a wide
 * `y`/`u`/`vv` intermediate and then narrows it with a `(u8)`/`(s8)` cast;
 * that cast is a proven no-op here (see the big comment), so Yv/Uv/Vv below
 * are already the final values color_to_yuv's out-params would hold —
 * no separate truncate/sign-extend step is applied on top. */
static inline void simd_color_to_yuv(__m128i R, __m128i G, __m128i B,
                                      __m128i* Yv, __m128i* Uv, __m128i* Vv) {
    __m128i Rp = _mm_packs_epi32(R, R), Gp = _mm_packs_epi32(G, G), Bp = _mm_packs_epi32(B, B);
    __m128i RG = _mm_unpacklo_epi16(Rp, Gp);   /* R0,G0,R1,G1,R2,G2,R3,G3 */

    const __m128i w_y = _mm_set1_epi32(0x00810042);          /* R:66 (even lane), G:129 (odd lane) */
    const __m128i w_u = _mm_set1_epi32((int)0xFFB6FFDAu);    /* R:-38 (even), G:-74 (odd) */
    const __m128i w_v = _mm_set1_epi32((int)0xFFA20070u);    /* R:112 (even), G:-94 (odd) */

    __m128i y_rg = _mm_madd_epi16(RG, w_y);   /* R*66 + G*129, exact (pmaddwd, no truncation) */
    __m128i u_rg = _mm_madd_epi16(RG, w_u);   /* R*-38 + G*-74 */
    __m128i v_rg = _mm_madd_epi16(RG, w_v);   /* R*112 + G*-94 */

    __m128i b25  = simd_sext16_lo(_mm_mullo_epi16(Bp, _mm_set1_epi16(25)));    /* 0..6375 */
    __m128i b112 = simd_sext16_lo(_mm_mullo_epi16(Bp, _mm_set1_epi16(112)));   /* 0..28560 */
    __m128i bm18 = simd_sext16_lo(_mm_mullo_epi16(Bp, _mm_set1_epi16(-18)));   /* -4590..0 */

    __m128i y = _mm_add_epi32(y_rg, b25);
    __m128i u = _mm_add_epi32(u_rg, b112);
    __m128i v = _mm_add_epi32(v_rg, bm18);

    const __m128i one = _mm_set1_epi32(1);
    /* (u8)/(s8) cast: proven no-op (see the big comment), so the rounded
     * int32 value below IS the cast's result — no truncate/sign-extend step
     * needed on top of it. */
    *Yv = _mm_add_epi32(_mm_srai_epi32(y, 8), _mm_and_si128(_mm_srai_epi32(y, 7), one));
    *Uv = _mm_add_epi32(_mm_srai_epi32(u, 8), _mm_and_si128(_mm_srai_epi32(u, 7), one));
    *Vv = _mm_add_epi32(_mm_srai_epi32(v, 8), _mm_and_si128(_mm_srai_epi32(v, 7), one));
}

/* Stride-2 "gather" via shuffle: from lo=[v0,v1,v2,v3] and hi=[v4,v5,v6,v7],
 * return [v0,v2,v4,v6] (gather_even2) or [v1,v3,v5,v7] (gather_odd2). SSE2
 * has no gather instruction; this reconstructs it from 2 contiguous loads. */
static inline __m128i simd_gather_even2(__m128i lo, __m128i hi) {
    __m128i los = _mm_shuffle_epi32(lo, _MM_SHUFFLE(3, 1, 2, 0));   /* [v0,v2,v1,v3] */
    __m128i his = _mm_shuffle_epi32(hi, _MM_SHUFFLE(3, 1, 2, 0));
    return _mm_unpacklo_epi64(los, his);                            /* [v0,v2, v4,v6] */
}
static inline __m128i simd_gather_odd2(__m128i lo, __m128i hi) {
    __m128i los = _mm_shuffle_epi32(lo, _MM_SHUFFLE(2, 0, 3, 1));   /* [v1,v3,v0,v2] */
    __m128i his = _mm_shuffle_epi32(hi, _MM_SHUFFLE(2, 0, 3, 1));
    return _mm_unpacklo_epi64(los, his);                            /* [v1,v3, v5,v7] */
}

/* ============================================================================
 * AVX2 widening of the 3 SSE2 EFB-copy passes above — 8x32-bit lanes instead
 * of 4. This TU is compiled WITHOUT -mavx2 (see gx_avx2_available's comment
 * near the top of the file); every function below carries its own
 * __attribute__((target("avx2"))) and is only ever entered after that gate
 * passes.
 *
 * filter_sum / color_to_yuv: AVX2 has a native 32-bit lane multiply
 * (_mm256_mullo_epi32) that SSE2 lacks — that gap is exactly why the SSE2
 * code above routes every multiply through a 16-bit pack+multiply+extend
 * trick (see its own big comment for the range proof that the 16-bit
 * truncation is a mathematical no-op: filter_sum's single products are
 * <=48195, its 3-term sum <=112455; color_to_yuv's y/u/v terms are bounded
 * in magnitude by <=56100, all inside u8/s8 after the proven-no-op cast).
 * Under AVX2 the direct, ISA-native equivalent is a plain 32-bit multiply on
 * the already-32-bit lane values the caller loaded/masked — same bounded
 * ranges, so every multiply and running sum stays exact inside int32 with no
 * overflow at any step. This also means the per-128-bit-lane pack/interleave
 * problem (_mm256_packs_epi32 et al only pack within each 128-bit lane, not
 * across all 8) simply doesn't arise for these two helpers — there is no
 * packing at all, only straight 32-bit lane arithmetic. The addition order
 * changes slightly (sequential adds vs. one pmaddwd pair-multiply-add + a
 * separate add for color_to_yuv's R/G terms), which is safe here because
 * integer addition with no intermediate overflow is associative/commutative
 * — unlike the floating-point reassociation this task's rules guard
 * against, there is no rounding to reorder. Verified bit-exact against a
 * scalar reference (offline harness, not part of this build): filter_sum
 * over 20,000,000 random trials (bit widths per the range proof above) plus
 * the all-255/max-weight edge case, color_to_yuv over 20,000,000 random
 * trials plus all 8 corners of {0,255}^3 — 0 mismatches in every case.
 *
 * gather_even2/odd2 (the horizontal pack stage's stride-2 "gather"): no
 * native replacement this simple exists — AVX2's cross-lane shuffle
 * (_mm256_permutevar8x32_epi32) reads a single register, not two, so the
 * SSE2 shuffle+unpacklo_epi64 idiom is kept, widened per-128-bit-lane
 * exactly as SSE2 did it (_mm256_shuffle_epi32/_mm256_unpacklo_epi64 both
 * still operate independently per 128-bit lane under AVX2), plus ONE
 * additional cross-lane fixup (_mm256_permute4x64_epi64) after the unpack —
 * unavoidable because reassembling 8 strided values spread across two
 * 256-bit registers needs a step that SSE2's 4-wide version, operating on a
 * single 128-bit lane, never needed. This is pure data movement (no
 * arithmetic, no rounding/saturation to reason about) — it either
 * reproduces the exact source values in the right lanes or it doesn't;
 * verified against a scalar reference over 5,000,000 random trials, 0
 * mismatches (same offline harness).
 * ==========================================================================*/
__attribute__((target("avx2")))
static inline __m256i simd_clamp_max255_avx2(__m256i v) {
    __m256i c255 = _mm256_set1_epi32(255);
    __m256i gt = _mm256_cmpgt_epi32(v, c255);
    return _mm256_or_si256(_mm256_and_si256(gt, c255), _mm256_andnot_si256(gt, v));
}
__attribute__((target("avx2")))
static inline __m256i simd_clamp_0_255_avx2(__m256i v) {
    __m256i zero = _mm256_setzero_si256();
    v = _mm256_andnot_si256(_mm256_cmpgt_epi32(zero, v), v);   /* v<0 == 0>v (no cmplt in AVX2) */
    return simd_clamp_max255_avx2(v);
}

__attribute__((target("avx2")))
static inline __m256i simd_filter_sum_avx2(__m256i a, __m256i b, __m256i c, int wa, int wb, int wc) {
    __m256i pa = _mm256_mullo_epi32(a, _mm256_set1_epi32(wa));
    __m256i pb = _mm256_mullo_epi32(b, _mm256_set1_epi32(wb));
    __m256i pc = _mm256_mullo_epi32(c, _mm256_set1_epi32(wc));
    __m256i sum = _mm256_add_epi32(_mm256_add_epi32(pa, pb), pc);
    sum = _mm256_srli_epi32(sum, 6);   /* logical == arithmetic: sum always >= 0, same as SSE2 */
    return simd_clamp_max255_avx2(sum);
}

__attribute__((target("avx2")))
static inline void simd_color_to_yuv_avx2(__m256i R, __m256i G, __m256i B,
                                           __m256i* Yv, __m256i* Uv, __m256i* Vv) {
    __m256i y = _mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(R, _mm256_set1_epi32(66)),
                                                    _mm256_mullo_epi32(G, _mm256_set1_epi32(129))),
                                   _mm256_mullo_epi32(B, _mm256_set1_epi32(25)));
    __m256i u = _mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(R, _mm256_set1_epi32(-38)),
                                                    _mm256_mullo_epi32(G, _mm256_set1_epi32(-74))),
                                   _mm256_mullo_epi32(B, _mm256_set1_epi32(112)));
    __m256i v = _mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(R, _mm256_set1_epi32(112)),
                                                    _mm256_mullo_epi32(G, _mm256_set1_epi32(-94))),
                                   _mm256_mullo_epi32(B, _mm256_set1_epi32(-18)));
    const __m256i one = _mm256_set1_epi32(1);
    *Yv = _mm256_add_epi32(_mm256_srai_epi32(y, 8), _mm256_and_si256(_mm256_srai_epi32(y, 7), one));
    *Uv = _mm256_add_epi32(_mm256_srai_epi32(u, 8), _mm256_and_si256(_mm256_srai_epi32(u, 7), one));
    *Vv = _mm256_add_epi32(_mm256_srai_epi32(v, 8), _mm256_and_si256(_mm256_srai_epi32(v, 7), one));
}

/* Stride-2 gather widened to 8-wide: lo=[v0..v7], hi=[v8..v15] (each 8x32
 * lanes) -> even=[v0,v2,...,v14], odd=[v1,v3,...,v15]. See the big comment
 * above for the derivation (per-128-bit-lane shuffle+unpack, same as SSE2,
 * plus one cross-lane permute fixup). */
__attribute__((target("avx2")))
static inline __m256i simd_gather_even2_avx2(__m256i lo, __m256i hi) {
    __m256i los = _mm256_shuffle_epi32(lo, _MM_SHUFFLE(3, 1, 2, 0));
    __m256i his = _mm256_shuffle_epi32(hi, _MM_SHUFFLE(3, 1, 2, 0));
    __m256i u = _mm256_unpacklo_epi64(los, his);
    return _mm256_permute4x64_epi64(u, _MM_SHUFFLE(3, 1, 2, 0));
}
__attribute__((target("avx2")))
static inline __m256i simd_gather_odd2_avx2(__m256i lo, __m256i hi) {
    __m256i los = _mm256_shuffle_epi32(lo, _MM_SHUFFLE(2, 0, 3, 1));
    __m256i his = _mm256_shuffle_epi32(hi, _MM_SHUFFLE(2, 0, 3, 1));
    __m256i u = _mm256_unpacklo_epi64(los, his);
    return _mm256_permute4x64_epi64(u, _MM_SHUFFLE(3, 1, 2, 0));
}

/* Full AVX2 8-wide inner loop of gx_raster_efb_copy's vertical-filter +
 * RGB->YUV convert stage (the SSE2 4-wide loop's exact body, widened) —
 * extracted into its own function because __attribute__((target("avx2")))
 * is a function-level attribute, not a block-level one. Runs while
 * x+8<=right, starting at x0; returns the final x so the caller can pick up
 * with the existing SSE2 4-wide loop (for a 4-7 remainder) and then the
 * scalar tail (for <4), exactly the same "SIMD interior, scalar/narrower-SIMD
 * remainder" split as every other pass in this file. */
__attribute__((target("avx2")))
static int efb_copy_filter_yuv_avx2(const u32* rowPrev, const u32* rowSy, const u32* rowNext,
                                     int left, int right, int x0, int wab, int wcde, int wfg,
                                     int* scanY, int* scanU, int* scanV) {
    const __m256i mask8 = _mm256_set1_epi32(0xFF);
    int x = x0;
    for (; x + 8 <= right; x += 8) {
        int i = x - left + 1;
        __m256i c0 = _mm256_loadu_si256((const __m256i*)&rowPrev[x]);
        __m256i c1 = _mm256_loadu_si256((const __m256i*)&rowSy[x]);
        __m256i c2 = _mm256_loadu_si256((const __m256i*)&rowNext[x]);
        __m256i B0 = _mm256_and_si256(c0, mask8);
        __m256i G0 = _mm256_and_si256(_mm256_srli_epi32(c0, 8), mask8);
        __m256i R0 = _mm256_and_si256(_mm256_srli_epi32(c0, 16), mask8);
        __m256i B1 = _mm256_and_si256(c1, mask8);
        __m256i G1 = _mm256_and_si256(_mm256_srli_epi32(c1, 8), mask8);
        __m256i R1 = _mm256_and_si256(_mm256_srli_epi32(c1, 16), mask8);
        __m256i B2 = _mm256_and_si256(c2, mask8);
        __m256i G2 = _mm256_and_si256(_mm256_srli_epi32(c2, 8), mask8);
        __m256i R2 = _mm256_and_si256(_mm256_srli_epi32(c2, 16), mask8);
        __m256i filtB = simd_filter_sum_avx2(B0, B1, B2, wab, wcde, wfg);
        __m256i filtG = simd_filter_sum_avx2(G0, G1, G2, wab, wcde, wfg);
        __m256i filtR = simd_filter_sum_avx2(R0, R1, R2, wab, wcde, wfg);
        __m256i Yv, Uv, Vv;
        simd_color_to_yuv_avx2(filtR, filtG, filtB, &Yv, &Uv, &Vv);
        _mm256_storeu_si256((__m256i*)&scanY[i], Yv);
        _mm256_storeu_si256((__m256i*)&scanU[i], Uv);
        _mm256_storeu_si256((__m256i*)&scanV[i], Vv);
    }
    return x;
}

/* Full AVX2 8-wide inner loop of the chroma-smooth (1/4+1/2+1/4) precompute
 * — plain 3-tap add+shift, no packing at all, so this is a direct lane-count
 * widen with no reasoning needed beyond that. Runs while i+8<=limit
 * (limit==src_w+1, matching the SSE2 loop's i+4<=src_w+1); returns final i. */
__attribute__((target("avx2")))
static int efb_copy_smooth_avx2(int* smoothU, int* smoothV, const int* scanU, const int* scanV,
                                 int i0, int limit) {
    int i = i0;
    for (; i + 8 <= limit; i += 8) {
        __m256i um1 = _mm256_loadu_si256((const __m256i*)&scanU[i - 1]);
        __m256i u0  = _mm256_loadu_si256((const __m256i*)&scanU[i]);
        __m256i up1 = _mm256_loadu_si256((const __m256i*)&scanU[i + 1]);
        _mm256_storeu_si256((__m256i*)&smoothU[i],
            _mm256_add_epi32(_mm256_add_epi32(um1, _mm256_slli_epi32(u0, 1)), up1));
        __m256i vm1 = _mm256_loadu_si256((const __m256i*)&scanV[i - 1]);
        __m256i v0  = _mm256_loadu_si256((const __m256i*)&scanV[i]);
        __m256i vp1 = _mm256_loadu_si256((const __m256i*)&scanV[i + 1]);
        _mm256_storeu_si256((__m256i*)&smoothV[i],
            _mm256_add_epi32(_mm256_add_epi32(vm1, _mm256_slli_epi32(v0, 1)), vp1));
    }
    return i;
}

/* Full AVX2 8-wide (16 source columns / 8 output pixel-pairs) inner loop of
 * the horizontal chroma-downsample + YUYV pack stage — widened via
 * simd_gather_even2_avx2/odd2_avx2 above. Runs while x+16<=src_w, starting
 * at x0 (i is always x+1 in this loop, same invariant the SSE2/scalar code
 * relies on); returns final x. */
__attribute__((target("avx2")))
static int efb_copy_pack_avx2(u8* row, int x0, int src_w, const int* scanY,
                               const int* smoothU, const int* smoothV) {
    int x = x0, i = x0 + 1;
    for (; x + 16 <= src_w; i += 16, x += 16) {
        __m256i Yc_lo = _mm256_loadu_si256((const __m256i*)&scanY[i]);
        __m256i Yc_hi = _mm256_loadu_si256((const __m256i*)&scanY[i + 8]);
        __m256i Y0v = simd_gather_even2_avx2(Yc_lo, Yc_hi);
        __m256i Y1v = simd_gather_odd2_avx2(Yc_lo, Yc_hi);
        __m256i sU_lo = _mm256_loadu_si256((const __m256i*)&smoothU[i]);
        __m256i sU_hi = _mm256_loadu_si256((const __m256i*)&smoothU[i + 8]);
        __m256i sV_lo = _mm256_loadu_si256((const __m256i*)&smoothV[i]);
        __m256i sV_hi = _mm256_loadu_si256((const __m256i*)&smoothV[i + 8]);
        __m256i UV0raw = simd_gather_even2_avx2(sU_lo, sU_hi);
        __m256i UV1raw = simd_gather_even2_avx2(sV_lo, sV_hi);
        __m256i Y0c = simd_clamp_0_255_avx2(_mm256_add_epi32(Y0v, _mm256_set1_epi32(16)));
        __m256i Y1c = simd_clamp_0_255_avx2(_mm256_add_epi32(Y1v, _mm256_set1_epi32(16)));
        __m256i UV0c = simd_clamp_0_255_avx2(_mm256_add_epi32(_mm256_set1_epi32(128), _mm256_srai_epi32(UV0raw, 2)));
        __m256i UV1c = simd_clamp_0_255_avx2(_mm256_add_epi32(_mm256_set1_epi32(128), _mm256_srai_epi32(UV1raw, 2)));
        __m256i word = _mm256_or_si256(_mm256_or_si256(Y0c, _mm256_slli_epi32(UV0c, 8)),
                                        _mm256_or_si256(_mm256_slli_epi32(Y1c, 16), _mm256_slli_epi32(UV1c, 24)));
        _mm256_storeu_si256((__m256i*)(row + x * 2), word);
    }
    return x;
}

typedef struct {
    u8 r, g, b, a;
} EfbCopyTexel;

static EfbCopyTexel efb_copy_texture_sample(
        int x, int y, int left, int top, int right, int bottom,
        int clamp_top, int clamp_bottom, int half_scale, int depth,
        int intensity_yuv, int wab, int wcde, int wfg,
        u32 (*getpx)(u32)) {
    if (half_scale) {
        x = left + (x - left) * 2;
        y = top + (y - top) * 2;
    }
    x = clampi(x, 0, (int)EFB_WIDTH - 1);
    y = clampi(y, 0, (int)EFB_HEIGHT - 1);
    int yprev = y - 1;
    int ynext = y + 1;
    if (clamp_top && yprev < top) yprev = top;
    if (clamp_bottom && ynext >= bottom) ynext = bottom - 1;
    yprev = clampi(yprev, 0, (int)EFB_HEIGHT - 1);
    ynext = clampi(ynext, 0, (int)EFB_HEIGHT - 1);

    EfbCopyTexel rows[3];
    const int ys[3] = { yprev, y, ynext };
    for (int i = 0; i < 3; i++) {
        if (depth) {
            u32 z = GetPixelDepth((u32)ys[i] * EFB_WIDTH + (u32)x);
            rows[i].r = (u8)(z >> 16);
            rows[i].g = (u8)(z >> 8);
            rows[i].b = (u8)z;
            rows[i].a = 0xFFu;
        } else {
            u32 c = getpx((u32)ys[i] * EFB_WIDTH + (u32)x);
            rows[i].a = (u8)c;
            rows[i].b = (u8)(c >> 8);
            rows[i].g = (u8)(c >> 16);
            rows[i].r = (u8)(c >> 24);
        }
    }

    EfbCopyTexel out;
#define FILTER_COPY_CH(ch) \
    (u8)clampi(((int)rows[0].ch * wab + (int)rows[1].ch * wcde + \
                (int)rows[2].ch * wfg) >> 6, 0, 255)
    out.r = FILTER_COPY_CH(r);
    out.g = FILTER_COPY_CH(g);
    out.b = FILTER_COPY_CH(b);
    out.a = rows[1].a;
#undef FILTER_COPY_CH

    if (intensity_yuv) {
        int yr = 66 * out.r + 129 * out.g + 25 * out.b + 4096;
        int ur = -38 * out.r - 74 * out.g + 112 * out.b + 32768;
        int vr = 112 * out.r - 94 * out.g - 18 * out.b + 32768;
        out.r = (u8)clampi((yr >> 8) + ((yr >> 7) & 1), 0, 255);
        out.g = (u8)clampi((ur >> 8) + ((ur >> 7) & 1), 0, 255);
        out.b = (u8)clampi((vr >> 8) + ((vr >> 7) & 1), 0, 255);
    }
    return out;
}

static void efb_copy_texture_write_block(
        u8* dst, u32 fmt, int bx, int by, int out_w, int out_h,
        int left, int top, int right, int bottom, int clamp_top,
        int clamp_bottom, int half_scale, int depth, int intensity_yuv,
        int wab, int wcde, int wfg, u32 (*getpx)(u32)) {
    int bw = fmt == 0 ? 8 : (fmt == 1 || fmt == 2 || fmt == 7 ||
                            fmt == 8 || fmt == 9 || fmt == 10) ? 8 : 4;
    int bh = fmt == 0 ? 8 : (fmt == 1 || fmt == 2 || fmt == 7 ||
                            fmt == 8 || fmt == 9 || fmt == 10) ? 4 : 4;
    EfbCopyTexel px[64];
    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            int ox = bx * bw + x, oy = by * bh + y;
            int sx = left + (ox < out_w ? ox : out_w - 1);
            int sy = top + (oy < out_h ? oy : out_h - 1);
            px[y * bw + x] = efb_copy_texture_sample(
                sx, sy, left, top, right, bottom, clamp_top, clamp_bottom,
                half_scale, depth, intensity_yuv, wab, wcde, wfg, getpx);
        }
    }

    if (fmt == 0) {                              /* R4 / I4 */
        for (int i = 0; i < 64; i += 2)
            dst[i >> 1] = (u8)((px[i].r & 0xF0u) | (px[i + 1].r >> 4));
    } else if (fmt == 1 || fmt == 8 || fmt == 9 || fmt == 10 || fmt == 7) {
        for (int i = 0; i < 32; i++) {            /* R8/G8/B8/A8 */
            EfbCopyTexel p = px[i];
            dst[i] = fmt == 9 ? p.g : fmt == 10 ? p.b : fmt == 7 ? p.a : p.r;
        }
    } else if (fmt == 2) {                        /* RA4 / IA4 */
        for (int i = 0; i < 32; i++)
            dst[i] = (u8)((px[i].a & 0xF0u) | (px[i].r >> 4));
    } else if (fmt == 3 || fmt == 11 || fmt == 12) {
        for (int i = 0; i < 16; i++) {            /* RA8 / RG8 / GB8 */
            EfbCopyTexel p = px[i];
            dst[i * 2] = fmt == 11 ? p.g : fmt == 12 ? p.b : p.a;
            dst[i * 2 + 1] = fmt == 11 ? p.r : fmt == 12 ? p.g : p.r;
        }
    } else if (fmt == 4) {                        /* RGB565 */
        for (int i = 0; i < 16; i++) {
            u16 v = (u16)(((u16)(px[i].r >> 3) << 11) |
                          ((u16)(px[i].g >> 2) << 5) | (px[i].b >> 3));
            dst[i * 2] = (u8)(v >> 8); dst[i * 2 + 1] = (u8)v;
        }
    } else if (fmt == 5) {                        /* RGB5A3 */
        for (int i = 0; i < 16; i++) {
            EfbCopyTexel p = px[i];
            u16 v = p.a > 224u
                ? (u16)(0x8000u | ((u16)(p.r >> 3) << 10) |
                      ((u16)(p.g >> 3) << 5) | (p.b >> 3))
                : (u16)(((u16)(p.a >> 5) << 12) | ((u16)(p.r >> 4) << 8) |
                      ((u16)(p.g >> 4) << 4) | (p.b >> 4));
            dst[i * 2] = (u8)(v >> 8); dst[i * 2 + 1] = (u8)v;
        }
    } else {                                      /* RGBA8: AR plane, then GB */
        for (int i = 0; i < 16; i++) {
            dst[i * 2] = px[i].a; dst[i * 2 + 1] = px[i].r;
            dst[32 + i * 2] = px[i].g; dst[33 + i * 2] = px[i].b;
        }
    }
}

void gx_raster_efb_copy(const GxCpState* cp) {
    (void)cp;
    /* Lazy-init same as gx_raster_draw's own copy (line ~4153): an EFB copy
     * can be the very first GX-stats-relevant call of a run (e.g. a clear
     * with no preceding draw), so this can't rely on gx_raster_draw having
     * already resolved the knob. */
    if (s_draw_stats < 0) s_draw_stats = getenv("GCN_GX_STATS") ? 1 : 0;

    /* Own decode of the pixel_format/color_update/alpha_update/z-update quad
     * this call's GetPixelColor/efb_clear_rect calls need — see build_efb_cfg. */
    build_efb_cfg();

    /* Pixel-format decision, hoisted to ONCE PER COPY instead of once per
     * get_efb_color() call (see that function's comment): s_pf is one of a
     * fixed small set of BP-decoded values for the whole call, so pick the
     * direct formula function now instead of re-switching on s_pf 3 times per
     * output column. A live 5M/8M-tick IPL-menu boot (GCN_GX_STATS-adjacent
     * census, 2026-07-12) showed every observed EFB copy uses s_pf==2
     * (RGB565_Z16) — part of the RGB8_Z24/Z24/RGB565_Z16 group that all share
     * get_pixel_color_direct's formula — but this covers EVERY pixel_format
     * GetPixelColor itself supports, not just the one observed: RGBA6_Z24
     * gets its own direct formula, and any other/unexpected s_pf value falls
     * back to the general GetPixelColor() (switch + TRAPF), so an unseen
     * format is exactly as correct (and traps exactly the same way) as before
     * this change — nothing here assumes s_pf is fixed to the observed value,
     * only that it doesn't change mid-copy. */
    u32 (*copy_getpx)(u32) =
        (s_pf == PF_RGB8_Z24 || s_pf == PF_Z24 || s_pf == PF_RGB565_Z16) ? get_pixel_color_direct :
        (s_pf == PF_RGBA6_Z24)                                          ? get_pixel_color_rgba6 :
                                                                           GetPixelColor;

    u32 copy = s_bp[0x52];
    int clamp_top = bits(copy, 0, 1);
    int clamp_bottom = bits(copy, 1, 1);
    int scale_invert = bits(copy, 10, 1);
    int clear = bits(copy, 11, 1);
    int copy_to_xfb = bits(copy, 14, 1);
    u32 gamma = bits(copy, 7, 2);
    int half_scale = bits(copy, 9, 1);
    int intensity = bits(copy, 15, 1);
    int auto_conv = bits(copy, 16, 1);
    u32 flow = s_bp[0x53], fhigh = s_bp[0x54];
    int w0 = bits(flow, 0, 6), w1 = bits(flow, 6, 6);
    int w2 = bits(flow, 12, 6), w3 = bits(flow, 18, 6);
    int w4 = bits(fhigh, 0, 6), w5 = bits(fhigh, 6, 6);
    int w6 = bits(fhigh, 12, 6);
    if (gamma != 0) TRAP(gammacopy, "EFB copy gamma != 1.0");

    if (copy_to_xfb) {
        int left = (int)bits(s_bp[0x49], 0, 10);
        int top  = (int)bits(s_bp[0x49], 10, 10);
        int wx = (int)bits(s_bp[0x4a], 0, 10);
        int wy = (int)bits(s_bp[0x4a], 10, 10);
        int right = left + wx + 1;         /* srcRect right (exclusive) */
        int bottom = top + wy + 1;         /* srcRect bottom (exclusive) */

        u32 dest_addr = s_bp[0x4b] << 5;
        u32 dest_stride = s_bp[0x4d] << 5;  /* bytes */
        u32 yscale_reg = s_bp[0x4e];
        float yscale = scale_invert ? (yscale_reg ? 256.0f / (float)yscale_reg : 1.0f)
                                    : (float)yscale_reg / 256.0f;

        int src_w = right - left;
        int src_h = bottom - top;
        int dst_h = (int)(src_h * yscale);
        if (src_w <= 0 || src_h <= 0 || dst_h <= 0) return;

        u32 phys = dest_addr & 0x1FFFFFFFu;
        u64 last = (u64)phys + (u64)(dst_h - 1) * dest_stride + (u64)src_w * 2u;
        if (dest_stride == 0 || !s_cpu || !s_cpu->ram || last > (u64)s_cpu->ram_size) {
            TRAP(xfboob, "EFB->XFB destination out of MEM1 / zero stride");
        } else {
            /* Build yuv422 per source scanline (vertical 3-tap copy filter +
             * horizontal 1/4+1/2+1/4 chroma downsample), then nearest-neighbor
             * vertical-scale into the guest XFB. */
            static int scanY[EFB_WIDTH + 2];
            static int scanU[EFB_WIDTH + 2];
            static int scanV[EFB_WIDTH + 2];
            /* SIMD-only scratch (see the big SSE2 comment above): the
             * smoothed 1/4+1/2+1/4 chroma sum, precomputed for every column
             * (not just the odd ones the scalar loop reads — cheap, and lets
             * the pack stage below use a plain stride-2 shuffle-gather). */
            static int smoothU[EFB_WIDTH + 2];
            static int smoothV[EFB_WIDTH + 2];

            /* SIMD eligibility, decided ONCE PER COPY (not per scanline):
             * (a) not forced off via GCN_GX_NO_SIMD; (b) copy_getpx is
             * exactly get_pixel_color_direct — the only format this SIMD
             * path specializes for (see the big comment); (c) the whole
             * scanline's x range is inside [0,EFB_WIDTH), so loop 1's
             * vectorized span can never hit get_efb_color's x clamp (the
             * scalar efb_copy_col fallback still carries that clamp,
             * unconditionally, for anything that doesn't take this path). */
            if (s_no_simd < 0) s_no_simd = getenv("GCN_GX_NO_SIMD") ? 1 : 0;
            int simd_ok = !s_no_simd && copy_getpx == get_pixel_color_direct &&
                          left >= 0 && right <= (int)EFB_WIDTH;
            /* Resolved once per copy (same "per-copy-constant" discipline as
             * simd_ok/copy_getpx above), not re-checked per scanline. */
            int use_avx2 = simd_ok && gx_avx2_available();

            /* VI may snapshot this same guest address from the CPU thread.
             * Hold the XFB writer guard across every destination row so the
             * host observes either the previous field or this complete one. */
            gcn_gx_xfb_write_begin();
            for (int dy = 0; dy < dst_h; dy++) {
                int sy = top + (int)(dy / (yscale == 0 ? 1.0f : yscale) + 0.5f);
                if (sy >= bottom) sy = bottom - 1;
                int yprev = (clamp_top ? top : 0);
                if (sy - 1 > yprev) yprev = sy - 1;
                int ynext = (clamp_bottom ? bottom : (int)EFB_HEIGHT) - 1;
                if (sy + 1 < ynext) ynext = sy + 1;
                /* per-pixel filtered YUV (indices 1..src_w) */
                if (simd_ok) {
                    const u32* rowPrev = &s_efb_color[(u32)yprev * EFB_WIDTH];
                    const u32* rowSy   = &s_efb_color[(u32)sy    * EFB_WIDTH];
                    const u32* rowNext = &s_efb_color[(u32)ynext * EFB_WIDTH];
                    int wab = w0 + w1, wcde = w2 + w3 + w4, wfg = w5 + w6;
                    int x = left;
                    if (use_avx2)
                        x = efb_copy_filter_yuv_avx2(rowPrev, rowSy, rowNext, left, right, x,
                                                      wab, wcde, wfg, scanY, scanU, scanV);
                    const __m128i mask8 = _mm_set1_epi32(0xFF);
                    for (; x + 4 <= right; x += 4) {
                        int i = x - left + 1;
                        __m128i c0 = _mm_loadu_si128((const __m128i*)&rowPrev[x]);
                        __m128i c1 = _mm_loadu_si128((const __m128i*)&rowSy[x]);
                        __m128i c2 = _mm_loadu_si128((const __m128i*)&rowNext[x]);
                        __m128i B0 = _mm_and_si128(c0, mask8);
                        __m128i G0 = _mm_and_si128(_mm_srli_epi32(c0, 8), mask8);
                        __m128i R0 = _mm_and_si128(_mm_srli_epi32(c0, 16), mask8);
                        __m128i B1 = _mm_and_si128(c1, mask8);
                        __m128i G1 = _mm_and_si128(_mm_srli_epi32(c1, 8), mask8);
                        __m128i R1 = _mm_and_si128(_mm_srli_epi32(c1, 16), mask8);
                        __m128i B2 = _mm_and_si128(c2, mask8);
                        __m128i G2 = _mm_and_si128(_mm_srli_epi32(c2, 8), mask8);
                        __m128i R2 = _mm_and_si128(_mm_srli_epi32(c2, 16), mask8);
                        __m128i filtB = simd_filter_sum(B0, B1, B2, wab, wcde, wfg);
                        __m128i filtG = simd_filter_sum(G0, G1, G2, wab, wcde, wfg);
                        __m128i filtR = simd_filter_sum(R0, R1, R2, wab, wcde, wfg);
                        __m128i Yv, Uv, Vv;
                        simd_color_to_yuv(filtR, filtG, filtB, &Yv, &Uv, &Vv);
                        _mm_storeu_si128((__m128i*)&scanY[i], Yv);
                        _mm_storeu_si128((__m128i*)&scanU[i], Uv);
                        _mm_storeu_si128((__m128i*)&scanV[i], Vv);
                    }
                    for (int i = x - left + 1; x < right; i++, x++)
                        efb_copy_col(x, yprev, sy, ynext, copy_getpx, w0, w1, w2, w3, w4, w5, w6,
                                     scanY, scanU, scanV, i);
                } else {
                    for (int i = 1, x = left; x < right; i++, x++)
                        efb_copy_col(x, yprev, sy, ynext, copy_getpx, w0, w1, w2, w3, w4, w5, w6,
                                     scanY, scanU, scanV, i);
                }
                scanY[0] = scanY[1]; scanU[0] = scanU[1]; scanV[0] = scanV[1];
                scanY[src_w + 1] = scanY[src_w]; scanU[src_w + 1] = scanU[src_w]; scanV[src_w + 1] = scanV[src_w];
                u8* row = s_cpu->ram + phys + (u64)dy * dest_stride;
                if (simd_ok) {
                    /* Precompute the smoothed chroma for every column 1..src_w
                     * (padding at 0/src_w+1 already mirrors scanU/scanV
                     * above, so this needs no edge-casing of its own — see
                     * the big comment). */
                    int i = 1;
                    if (use_avx2)
                        i = efb_copy_smooth_avx2(smoothU, smoothV, scanU, scanV, i, src_w + 1);
                    for (; i + 4 <= src_w + 1; i += 4) {
                        __m128i um1 = _mm_loadu_si128((const __m128i*)&scanU[i - 1]);
                        __m128i u0  = _mm_loadu_si128((const __m128i*)&scanU[i]);
                        __m128i up1 = _mm_loadu_si128((const __m128i*)&scanU[i + 1]);
                        _mm_storeu_si128((__m128i*)&smoothU[i],
                            _mm_add_epi32(_mm_add_epi32(um1, _mm_slli_epi32(u0, 1)), up1));
                        __m128i vm1 = _mm_loadu_si128((const __m128i*)&scanV[i - 1]);
                        __m128i v0  = _mm_loadu_si128((const __m128i*)&scanV[i]);
                        __m128i vp1 = _mm_loadu_si128((const __m128i*)&scanV[i + 1]);
                        _mm_storeu_si128((__m128i*)&smoothV[i],
                            _mm_add_epi32(_mm_add_epi32(vm1, _mm_slli_epi32(v0, 1)), vp1));
                    }
                    for (; i <= src_w; i++) {
                        smoothU[i] = scanU[i - 1] + (scanU[i] << 1) + scanU[i + 1];
                        smoothV[i] = scanV[i - 1] + (scanV[i] << 1) + scanV[i + 1];
                    }

                    int x = 0;
                    i = 1;
                    if (use_avx2) {
                        x = efb_copy_pack_avx2(row, x, src_w, scanY, smoothU, smoothV);
                        i = x + 1;
                    }
                    for (; x + 8 <= src_w; i += 8, x += 8) {
                        __m128i Yc_lo = _mm_loadu_si128((const __m128i*)&scanY[i]);
                        __m128i Yc_hi = _mm_loadu_si128((const __m128i*)&scanY[i + 4]);
                        __m128i Y0v = simd_gather_even2(Yc_lo, Yc_hi);
                        __m128i Y1v = simd_gather_odd2(Yc_lo, Yc_hi);
                        __m128i sU_lo = _mm_loadu_si128((const __m128i*)&smoothU[i]);
                        __m128i sU_hi = _mm_loadu_si128((const __m128i*)&smoothU[i + 4]);
                        __m128i sV_lo = _mm_loadu_si128((const __m128i*)&smoothV[i]);
                        __m128i sV_hi = _mm_loadu_si128((const __m128i*)&smoothV[i + 4]);
                        __m128i UV0raw = simd_gather_even2(sU_lo, sU_hi);
                        __m128i UV1raw = simd_gather_even2(sV_lo, sV_hi);
                        __m128i Y0c = simd_clamp_0_255(_mm_add_epi32(Y0v, _mm_set1_epi32(16)));
                        __m128i Y1c = simd_clamp_0_255(_mm_add_epi32(Y1v, _mm_set1_epi32(16)));
                        __m128i UV0c = simd_clamp_0_255(_mm_add_epi32(_mm_set1_epi32(128), _mm_srai_epi32(UV0raw, 2)));
                        __m128i UV1c = simd_clamp_0_255(_mm_add_epi32(_mm_set1_epi32(128), _mm_srai_epi32(UV1raw, 2)));
                        __m128i word = _mm_or_si128(_mm_or_si128(Y0c, _mm_slli_epi32(UV0c, 8)),
                                                     _mm_or_si128(_mm_slli_epi32(Y1c, 16), _mm_slli_epi32(UV1c, 24)));
                        _mm_storeu_si128((__m128i*)(row + x * 2), word);
                    }
                    for (; x < src_w; i += 2, x += 2)
                        efb_copy_pack(row, x, i, scanY, scanU, scanV);
                } else {
                    for (int i = 1, x = 0; x < src_w; i += 2, x += 2)
                        efb_copy_pack(row, x, i, scanY, scanU, scanV);
                }
            }
            /* GCN_GX_XFB_HASH=1: feed exactly the bytes this copy just wrote
             * (src_w*2 valid bytes per row; dest_stride may include padding
             * the filter never touches) while still holding the writer
             * guard, then count this copy as one publication. */
            gcn_gx_xfb_hash_feed(s_cpu->ram + phys, dest_stride, (u32)src_w * 2u, (u32)dst_h);
            gcn_gx_xfb_write_end();
            gcn_gx_xfb_hash_publish_done();
        }
    } else {
        int left = (int)bits(s_bp[0x49], 0, 10);
        int top = (int)bits(s_bp[0x49], 10, 10);
        int src_w = (int)bits(s_bp[0x4a], 0, 10) + 1;
        int src_h = (int)bits(s_bp[0x4a], 10, 10) + 1;
        int right = left + src_w, bottom = top + src_h;
        int out_w = half_scale ? (src_w + 1) >> 1 : src_w;
        int out_h = half_scale ? (src_h + 1) >> 1 : src_h;
        u32 target = bits(copy, 3, 4);
        u32 fmt = target / 2u + (target & 1u) * 8u;
        int depth = s_pf == PF_Z24;
        int bw = fmt == 0 ? 8 : (fmt == 1 || fmt == 2 || fmt == 7 ||
                                fmt == 8 || fmt == 9 || fmt == 10) ? 8 : 4;
        int bh = fmt == 0 ? 8 : (fmt == 1 || fmt == 2 || fmt == 7 ||
                                fmt == 8 || fmt == 9 || fmt == 10) ? 4 : 4;
        u32 block_bytes = fmt == 6 ? 64u : 32u;
        u32 tiles_x = (u32)(out_w + bw - 1) / (u32)bw;
        u32 tiles_y = (u32)(out_h + bh - 1) / (u32)bh;
        u32 dest_addr = s_bp[0x4b] << 5;
        u32 dest_stride = s_bp[0x4d] << 5;
        u32 phys = dest_addr & 0x1FFFFFFFu;
        u64 last = (u64)phys + (tiles_y ? (u64)(tiles_y - 1u) * dest_stride : 0u) +
                   (u64)tiles_x * block_bytes;

        if (fmt > 12u || out_w <= 0 || out_h <= 0) {
            TRAPF(efbtexfmt, "invalid EFB->texture format/dimensions "
                  "(target=%u real=%u %dx%d)", target, fmt, out_w, out_h);
        } else if (dest_stride == 0 || !s_cpu || !s_cpu->ram ||
                   last > (u64)s_cpu->ram_size) {
            TRAP(efbtexoob, "EFB->texture destination out of MEM1 / zero stride");
        } else {
            static u16 seen_formats;
            if (!(seen_formats & (u16)(1u << fmt))) {
                seen_formats |= (u16)(1u << fmt);
                fprintf(stderr,
                    "gx_raster: EFB->texture format %u handled "
                    "(src=%d,%d %dx%d dst=%08X stride=%u half=%d depth=%d "
                    "intensity=%d auto=%d)\n",
                    fmt, left, top, src_w, src_h, dest_addr, dest_stride,
                    half_scale, depth, intensity, auto_conv);
            }
            int wab = w0 + w1, wcde = w2 + w3 + w4, wfg = w5 + w6;
            for (u32 by = 0; by < tiles_y; by++) {
                for (u32 bx = 0; bx < tiles_x; bx++) {
                    u8* dst = s_cpu->ram + phys + (u64)by * dest_stride +
                              (u64)bx * block_bytes;
                    efb_copy_texture_write_block(
                        dst, fmt, (int)bx, (int)by, out_w, out_h,
                        left, top, right, bottom, clamp_top, clamp_bottom,
                        half_scale, depth, intensity && auto_conv,
                        wab, wcde, wfg, copy_getpx);
                }
            }
        }
    }

    if (clear) {
        if (s_draw_stats) {
            u64 t0 = __rdtsc();
            efb_clear_rect();
            s_tsc_efb_clear += __rdtsc() - t0;
            s_efb_clear_calls++;
        } else {
            efb_clear_rect();
        }
    }
}

/* ============================================================================
 * Init
 * ==========================================================================*/
void gx_raster_init(CPUState* cpu, const u32* bp, const u32* xf) {
    s_cpu = cpu; s_bp = bp; s_xf = xf;
    s_bp_generation = 1;
    s_cfg_bp_generation = 0;
    s_cfg_cache_hits = 0;
    s_cfg_cache_misses = 0;
    memset(s_draw_shapes, 0, sizeof s_draw_shapes);
    memset(s_efb_color, 0, sizeof s_efb_color);
    memset(s_efb_depth, 0, sizeof s_efb_depth);
    memset(s_tev_w, 0, sizeof s_tev_w);
    for (int i = 0; i < GX_MT_MAX; i++)
        s_tev_w[i].wid = i;   /* indexes s_texel_cache_w/s_rb_w; see Tev.wid */
}
