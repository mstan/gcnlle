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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>      /* getenv — GCN_GX_STATS cached read, see below */
#include <string.h>
#include <x86intrin.h>   /* __rdtsc — GCN_GX_STATS attribution only */

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
 * per draw (and one per pixel) when off. */
#define GX_CENSUS_MAX 16
typedef struct { int used; u32 hash; u64 draws, pixels; } GxCensusEntry;
static int s_tev_census = -1;
static GxCensusEntry s_census[GX_CENSUS_MAX];
static int s_census_cur = -1;   /* index of the current draw's config, else -1 */
static u64 s_ps_texel_cache_hits;   /* per-draw texel cache: decode_texel calls it satisfied */
static u64 s_ps_texel_cache_misses; /* ...calls that had to fall through to a real decode */

/* ---- one-time trap logging ------------------------------------------------ */
static int trap_once(int* flag, const char* what) {
    if (*flag) return 0;
    *flag = 1;
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
        if (!field) { \
            field = 1; \
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
    int tevind_active;   /* bits(tevind,9,2)!=0 || bits(tevind,7,2)!=0 (decode
                           * only; the TRAP itself still fires at its original
                           * per-pixel-per-stage call site in tev_draw). */

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
    int mipmapfilter_bad;       /* TX_SETMODE0 bits 5-2 != 0 */
    int lod_edge;               /* TX_SETMODE0 bit 8 */
    s32 lod_bias_half;          /* sext(TX_SETMODE0[16:9], 8) >> 1, precomputed */
    u32 minlod, maxlod;         /* TX_SETMODE1 bits 0-8 / 8-8 */
    u32 image0_raw, image3_raw; /* raw TX_SETIMAGE0/3 (for trap-log messages) */
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
} DrawCfg;

static DrawCfg s_cfg;

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
static u32 GetPixelColor(u32 off) {
    u32 src = s_efb_color[off];
    switch (s_pf) {
    case PF_RGB8_Z24:
    case PF_Z24:
    case PF_RGB565_Z16:   /* SWEfbInterface.cpp:164-166 — Dolphin itself treats this
                           * identically to RGB8_Z24 ("not supported correctly yet");
                           * transcribed as-is so we match the oracle exactly. */
        return 0xffu | ((src & 0x00ffffffu) << 8);
    case PF_RGBA6_Z24:
        return (u32)Convert6To8(src & 0x3f) |
               ((u32)Convert6To8((src >> 6) & 0x3f) << 8) |
               ((u32)Convert6To8((src >> 12) & 0x3f) << 16) |
               ((u32)Convert6To8((src >> 18) & 0x3f) << 24);
    default:
        TRAPF(pf_getcolor, "EFB GetPixelColor pixel_format %u (only RGB8_Z24=0/RGBA6_Z24=1 "
              "in scope; raw ZCOMPARE/PEControl bp[0x43]=0x%06X)",
              s_pf, s_bp[0x43]);
        return 0xffu | ((src & 0x00ffffffu) << 8);
    }
}
static void SetPixelColorOnly(u32 off, const u8* rgb) {
    u32 src; memcpy(&src, rgb, 4);
    switch (s_pf) {
    case PF_RGB8_Z24: case PF_Z24: case PF_RGB565_Z16:
        s_efb_color[off] = (s_efb_color[off] & 0xff000000u) | (src >> 8);
        break;
    case PF_RGBA6_Z24: {
        u32 val = s_efb_color[off] & 0xff00003fu;
        val |= (src >> 4) & 0x00000fc0u;   /* blue  */
        val |= (src >> 6) & 0x0003f000u;   /* green */
        val |= (src >> 8) & 0x00fc0000u;   /* red   */
        s_efb_color[off] = val;
        break;
    }
    default:
        TRAPF(pf_setcolor, "EFB SetPixelColorOnly pixel_format %u (raw bp[0x43]=0x%06X)",
              s_pf, s_bp[0x43]);
        break;
    }
}
static void SetPixelAlphaColor(u32 off, const u8* color) {
    u32 src; memcpy(&src, color, 4);
    switch (s_pf) {
    case PF_RGB8_Z24: case PF_Z24: case PF_RGB565_Z16:
        s_efb_color[off] = (s_efb_color[off] & 0xff000000u) | (src >> 8);
        break;
    case PF_RGBA6_Z24: {
        u32 val = s_efb_color[off] & 0xff000000u;
        val |= (src >> 2) & 0x0000003fu;   /* alpha */
        val |= (src >> 4) & 0x00000fc0u;   /* blue  */
        val |= (src >> 6) & 0x0003f000u;   /* green */
        val |= (src >> 8) & 0x00fc0000u;   /* red   */
        s_efb_color[off] = val;
        break;
    }
    default:
        TRAPF(pf_setalpha, "EFB SetPixelAlphaColor pixel_format %u (raw bp[0x43]=0x%06X)",
              s_pf, s_bp[0x43]);
        break;
    }
}
static void SetPixelAlphaOnly(u32 off, u8 a) {
    if (s_pf == PF_RGBA6_Z24) {
        u32 val = s_efb_color[off] & 0xffffffc0u;
        val |= (a >> 2) & 0x3f;
        s_efb_color[off] = val;
    }
    /* RGB8/Z24/RGB565 have no alpha plane — nothing to do. */
}
static void SetPixelDepth(u32 off, u32 depth) { s_efb_depth[off] = depth & 0x00ffffffu; }
static u32  GetPixelDepth(u32 off)            { return s_efb_depth[off] & 0x00ffffffu; }

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

/* Per-unit BP register base: unit 0 lives at 0x80.. (BPMemory.h:82-88). */
static u32 tx_mode0(u32 unit) { return s_bp[0x80 + unit]; }
static u32 tx_mode1(u32 unit) { return s_bp[0x84 + unit]; }
static u32 tx_image0(u32 unit){ return s_bp[0x88 + unit]; }
static u32 tx_image3(u32 unit){ return s_bp[0x94 + unit]; }

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

/* Per-texel decode (TextureDecoder_Common.cpp TexDecoder_DecodeTexel:361-639,
 * transcribed exactly for the 7 non-paletted formats the menu uses). `out` is
 * RGBA (matches the rest of the TEV pipeline's texel[4] convention). Endianness
 * note: RGB565/RGB5A3/CMPR color1/color2 explicitly Common::swap16 the raw
 * 16-bit read before decoding (so we reconstruct the true big-endian value via
 * be_u16); IA8 does NOT swap (DecodePixel_IA8 takes the raw unswapped read), so
 * on Dolphin's little-endian host alpha ends up as the FIRST transmitted byte
 * and intensity the second — reproduced here with direct byte indexing rather
 * than be_u16, to match the oracle bit-for-bit rather than the "IA" name. */
static void decode_texel(u32 fmt, const u8* src, u32 src_len, int s, int t,
                         int image_w_minus_1, u8 out[4]) {
    u32 ss = (u32)s, tt = (u32)t, iw1 = (u32)image_w_minus_1;
    switch (fmt) {
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
 * textures are small, but texmap (3 bits) + iS + iT (up to 10 bits each,
 * GX's max texture dimension is 1024) already want 23 bits for a collision-
 * free key, leaving too few spare bits to also carry a generation counter
 * that wouldn't wrap (and re-trigger the full-clear path) many times a
 * second. Two separate u32s cost 4 more bytes per entry but keep both fields
 * exact and the wraparound vanishingly rare.
 *
 * KEY: texmap<<20 | iT<<10 | iS — exact and collision-free (iS/iT < 1024,
 * texmap < 8), used only for the tag comparison, not the index.
 *
 * INDEX MIX: (iS ^ (iT<<5) ^ (texmap<<11)) & MASK. iS lands in the index's
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
    u32 key;      /* texmap<<20 | iT<<10 | iS — exact tag, see comment above */
    u32 gen;      /* draw generation this slot was last written in */
    u8  rgba[4];  /* decode_texel's exact out[4] for that (texmap,iS,iT) */
} TexelCacheEntry;

static TexelCacheEntry s_texel_cache[TEXEL_CACHE_SIZE];

/* 0 is reserved as the "slot never written since last full clear" sentinel
 * (see the wraparound handling in build_draw_cfg) — so the very first real
 * draw must not use generation 0 either; build_draw_cfg's pre-increment
 * (0 -> 1 on the first call) already guarantees that. */
static u32 s_texel_cache_gen;

static inline u32 texel_cache_key(u32 texmap, u32 iS, u32 iT) {
    return (texmap << 20) | (iT << 10) | iS;
}
static inline u32 texel_cache_index(u32 texmap, u32 iS, u32 iT) {
    return (iS ^ (iT << 5) ^ (texmap << 11)) & TEXEL_CACHE_MASK;
}

/* Cache-checked decode_texel wrapper. Hooked INSIDE tex_sample (this is its
 * only caller — decode_texel itself is left untouched and still callable
 * directly, though a repo-wide grep found no other caller) so callers of
 * decode_texel that might not share tex_sample's per-draw-frozen-texmap
 * invariant are unaffected by construction, not just by accident. */
static inline void decode_texel_cached(u32 texmap, u32 fmt, const u8* src, u32 src_len,
                                        int iS, int iT, int w1, u8 out[4]) {
    u32 key = texel_cache_key(texmap, (u32)iS, (u32)iT);
    u32 idx = texel_cache_index(texmap, (u32)iS, (u32)iT);
    TexelCacheEntry* e = &s_texel_cache[idx];

    if (e->gen == s_texel_cache_gen && e->key == key) {
        /* Hit: byte-identical to the decode_texel call this replaces (see the
         * EXACTNESS argument above) — no recompute, just copy the 4 bytes. */
        out[0] = e->rgba[0]; out[1] = e->rgba[1]; out[2] = e->rgba[2]; out[3] = e->rgba[3];
        if (s_pixel_stats) s_ps_texel_cache_hits++;
        return;
    }

    decode_texel(fmt, src, src_len, iS, iT, w1, out);
    e->key = key;
    e->gen = s_texel_cache_gen;
    e->rgba[0] = out[0]; e->rgba[1] = out[1]; e->rgba[2] = out[2]; e->rgba[3] = out[3];
    if (s_pixel_stats) s_ps_texel_cache_misses++;
}

static void tex_sample(u32 texmap, s32 s, s32 t, int linear, u8* out /*RGBA*/) {
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
        break;   /* supported */
    case TEXFMT_C4: case TEXFMT_C8: case TEXFMT_C14X2:
        TRAPF(paletted, "paletted texture format %u (C4/C8/C14X2 need a TLUT model "
              "we haven't built) — texmap %u, %ux%u, TX_SETIMAGE0=0x%06X",
              fmt, texmap, w1 + 1, h1 + 1, tc->image0_raw);
        memset(out, 0, 4); return;
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
    if (tc->mipmapfilter_bad) TRAP(mipmapfilter, "texture mipmap filter (mipmaps)");

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
        decode_texel_cached(texmap, fmt, src, src_len, iS,  iT,  w1, v00);
        decode_texel_cached(texmap, fmt, src, src_len, iS1, iT,  w1, v10);
        decode_texel_cached(texmap, fmt, src, src_len, iS,  iT1, w1, v01);
        decode_texel_cached(texmap, fmt, src, src_len, iS1, iT1, w1, v11);
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
        decode_texel_cached(texmap, fmt, src, src_len, iS, iT, w1, out);
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
typedef struct {
    TColor Reg[4];          /* Prev, Color0, Color1, Color2 */
    TColor Konst[4];        /* KonstantColors */
    TColor TexColor, RasColor, StageKonst, RawTexColor;
    s32    Position[3];
    u8     Color[2][4];     /* [chan][R,G,B,A] */
    s32    UvS[8], UvT[8];  /* s17.7 */
    s32    TexCoordS, TexCoordT;
    u8     AlphaBump;
    s32    TextureLod[16];  int TextureLinear[16];
} Tev;

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

/* GCN_GX_PIXEL_STATS BLEND bucket: everything from the moment alpha_test()
 * PASSES onward — late-Z (EmulatedZ::Late; it lives here, gated on the shaded
 * result, not in SLOPE which only ever runs early-Z) and BlendTev. Factored
 * out purely so the timed wrapper in tev_draw has one call to time regardless
 * of whether late-Z rejects (early return) or the pixel reaches BlendTev —
 * same technique as raster_pixel_prep. Behavior is unchanged from the inline
 * version this replaces. */
static void blend_stage(Tev* t, u8* output) {
    if (s_cfg.zt_enable && !s_cfg.zt_early) {
        if (!ZCompare((u16)t->Position[0], (u16)t->Position[1], (u32)t->Position[2]))
            return;
    }
    BlendTev((u16)t->Position[0], (u16)t->Position[1], output);
    if (s_pixel_stats) s_ps_blend_writes++;   /* GCN_GX_PIXEL_STATS: blend_writes counter */
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
 * rationale. */
static void tev_draw(Tev* t) {
    if (s_draw_stats) s_pixels_shaded++;    /* GCN_GX_STATS: pixels_shaded counter */
    if (s_pixel_stats) s_ps_shaded++;       /* GCN_GX_PIXEL_STATS: own shaded counter */
    if (s_census_cur >= 0) s_census[s_census_cur].pixels++;  /* GCN_GX_TEV_CENSUS */

    u64 comb_t0 = 0, tex_before = 0;
    if (s_pixel_stats) { comb_t0 = __rdtsc(); tex_before = s_tsc_tex; }

    u32 numstages = s_cfg.numtevstages;  /* actual count is +1 */
    u32 numtexgens = s_cfg.numtexgens;

    for (u32 stage = 0; stage <= numstages; stage++) {
        const TevStageCfg* sc = &s_cfg.stage[stage];
        u32 texcoordSel = sc->texcoordSel;
        u32 texmap      = sc->texmap;
        u32 enable      = sc->enable;
        u32 colorchan   = sc->colorchan;

        /* Indirect: no indirect stages in scope -> TexCoord = Uv (Tev.cpp:369-384). */
        if (sc->tevind_active) TRAP(indactive, "active indirect stage");
        t->TexCoordS = t->UvS[texcoordSel];
        t->TexCoordT = t->UvT[texcoordSel];

        if (enable) {
            u8 texel[4];
            if (numtexgens > 0) {
                /* GCN_GX_PIXEL_STATS TEX bucket: tex_sample total, timed only
                 * around the call itself (not the memset fallback above/the
                 * swap-table color assignment below). */
                if (s_pixel_stats) {
                    s_ps_tex_calls++;
                    if (t->TextureLinear[stage]) s_ps_tex_linear++; else s_ps_tex_point++;
                    u64 tex_t0 = __rdtsc();
                    tex_sample(texmap, t->TexCoordS, t->TexCoordT,
                               t->TextureLinear[stage], texel);
                    s_tsc_tex += __rdtsc() - tex_t0;
                } else {
                    tex_sample(texmap, t->TexCoordS, t->TexCoordT,
                               t->TextureLinear[stage], texel);
                }
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
            } else {
                /* AlphaBump / normalized / zero: menu uses only Color0/1; others -> 0. */
                if (colorchan != 7) TRAP(raschan, "ras color channel (alpha bump)");
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

    if (!alpha_test(output[ALP_C])) {
        /* COMB bucket boundary (fail exit): stage loop + alpha_test, minus the
         * nested TEX delta accumulated above. */
        if (s_pixel_stats) s_tsc_comb += (__rdtsc() - comb_t0) - (s_tsc_tex - tex_before);
        return;
    }

    /* COMB bucket boundary (pass exit): same subtraction, taken before
     * blend_stage() starts so BLEND's own timing below doesn't leak into it. */
    if (s_pixel_stats) {
        s_tsc_comb += (__rdtsc() - comb_t0) - (s_tsc_tex - tex_before);
        u64 blend_t0 = __rdtsc();
        blend_stage(t, output);   /* late-Z (if it lives here) + BlendTev */
        s_tsc_blend += __rdtsc() - blend_t0;
    } else {
        blend_stage(t, output);   /* late-Z (if it lives here) + BlendTev */
    }
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
} OutVtx;

typedef struct { float f0, dfdx, dfdy; s32 x0, y0; float xOff, yOff; } Slope;

static Tev    s_tev;
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
static RBPixel s_rb[BLK][BLK];

static void calc_lod(Tev* t, s32* lodp, int* linear, u32 texmap, u32 texcoord) {
    /* tx_mode0/mode1 fields are per-draw-constant; s_cfg.tex[texmap] carries
     * them (build_draw_cfg), so this no longer re-reads BP per block. */
    const TexUnitCfg* tc = &s_cfg.tex[texmap];
    float* uv00 = s_rb[0][0].Uv[texcoord];
    float* uv10 = s_rb[1][0].Uv[texcoord];
    float* uv01 = s_rb[0][1].Uv[texcoord];
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
    for (s32 yi = 0; yi < BLK; yi++) {
        for (s32 xi = 0; xi < BLK; xi++) {
            RBPixel* p = &s_rb[xi][yi];
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
    RBPixel* p = &s_rb[xi][yi];
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

static void raster_pixel(Tev* t, s32 x, s32 y, s32 xi, s32 yi) {
    if (!s_pixel_stats) {
        if (raster_pixel_prep(t, x, y, xi, yi)) tev_draw(t);   /* runs the combiner, late-Z, alpha test, then blends */
        return;
    }
    u64 t0 = __rdtsc();
    int ready = raster_pixel_prep(t, x, y, xi, yi);
    s_tsc_slope += __rdtsc() - t0;
    if (!ready) { s_ps_earlyz_rejected++; return; }
    tev_draw(t);
}

static void update_zslope(const OutVtx* v0, const OutVtx* v1, const OutVtx* v2,
                          s32 x_off, s32 y_off) {
    /* zfreeze is off for the menu; recompute each triangle. */
    s32 X1 = iround(16.0f * (v0->screenPosition[0] - x_off)) - 9;
    s32 Y1 = iround(16.0f * (v0->screenPosition[1] - y_off)) - 9;
    SlopeCtx ctx = make_ctx(v0, v1, v2, (X1 + 0xF) >> 4, (Y1 + 0xF) >> 4, x_off, y_off);
    s_ZSlope = make_slope(v0->screenPosition[2], v1->screenPosition[2], v2->screenPosition[2], &ctx);
}

static void draw_triangle_impl(Tev* t, const OutVtx* v0, const OutVtx* v1, const OutVtx* v2) {
    s32 x_off = s_scissor_xoff, y_off = s_scissor_yoff;
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

    SlopeCtx ctx = make_ctx(v0, v1, v2, (X1 + 0xF) >> 4, (Y1 + 0xF) >> 4, x_off, y_off);
    float w[3] = { 1.0f / v0->projectedPosition[3], 1.0f / v1->projectedPosition[3],
                   1.0f / v2->projectedPosition[3] };
    s_WSlope = make_slope(w[0], w[1], w[2], &ctx);
    for (u32 i = 0; i < s_cfg.numcolchans; i++)
        for (int comp = 0; comp < 4; comp++)
            s_ColorSlopes[i][comp] = make_slope(v0->color[i][comp], v1->color[i][comp],
                                                v2->color[i][comp], &ctx);
    for (u32 i = 0; i < s_cfg.numtexgens; i++) {
        s_TexSlopes[i][0] = make_slope(v0->texCoords[i][0]*w[0], v1->texCoords[i][0]*w[1], v2->texCoords[i][0]*w[2], &ctx);
        s_TexSlopes[i][1] = make_slope(v0->texCoords[i][1]*w[0], v1->texCoords[i][1]*w[1], v2->texCoords[i][1]*w[2], &ctx);
        s_TexSlopes[i][2] = make_slope(v0->texCoords[i][2]*w[0], v1->texCoords[i][2]*w[1], v2->texCoords[i][2]*w[2], &ctx);
    }

    s32 C1 = DY12 * X1 - DX12 * Y1;
    s32 C2 = DY23 * X2 - DX23 * Y2;
    s32 C3 = DY31 * X3 - DX31 * Y3;
    if (DY12 < 0 || (DY12 == 0 && DX12 > 0)) C1++;
    if (DY23 < 0 || (DY23 == 0 && DX23 > 0)) C2++;
    if (DY31 < 0 || (DY31 == 0 && DX31 > 0)) C3++;

    s32 block_minx = minx & ~(BLK - 1);
    s32 block_miny = miny & ~(BLK - 1);
    for (s32 y = block_miny; y < maxy; y += BLK) {
        for (s32 x = block_minx; x < maxx; x += BLK) {
            s32 x1_ = x + BLK - 1, y1_ = y + BLK - 1;
            s32 x0 = x << 4, xx1 = x1_ << 4, y0 = y << 4, yy1 = y1_ << 4;
            int a00 = C1 + DX12 * y0 - DY12 * x0 > 0, a10 = C1 + DX12 * y0 - DY12 * xx1 > 0;
            int a01 = C1 + DX12 * yy1 - DY12 * x0 > 0, a11 = C1 + DX12 * yy1 - DY12 * xx1 > 0;
            int a = (a00) | (a10 << 1) | (a01 << 2) | (a11 << 3);
            int b00 = C2 + DX23 * y0 - DY23 * x0 > 0, b10 = C2 + DX23 * y0 - DY23 * xx1 > 0;
            int b01 = C2 + DX23 * yy1 - DY23 * x0 > 0, b11 = C2 + DX23 * yy1 - DY23 * xx1 > 0;
            int bb = (b00) | (b10 << 1) | (b01 << 2) | (b11 << 3);
            int c00 = C3 + DX31 * y0 - DY31 * x0 > 0, c10 = C3 + DX31 * y0 - DY31 * xx1 > 0;
            int c01 = C3 + DX31 * yy1 - DY31 * x0 > 0, c11 = C3 + DX31 * yy1 - DY31 * xx1 > 0;
            int cc = (c00) | (c10 << 1) | (c01 << 2) | (c11 << 3);
            if (a == 0 || bb == 0 || cc == 0) continue;

            build_block_timed(t, x, y);
            if (a == 0xF && bb == 0xF && cc == 0xF && x >= minx && x1_ < maxx && y >= miny && y1_ < maxy) {
                for (s32 iy = 0; iy < BLK; iy++)
                    for (s32 ix = 0; ix < BLK; ix++)
                        raster_pixel(t, x + ix, y + iy, ix, iy);
            } else {
                s32 CY1 = C1 + DX12 * y0 - DY12 * x0;
                s32 CY2 = C2 + DX23 * y0 - DY23 * x0;
                s32 CY3 = C3 + DX31 * y0 - DY31 * x0;
                for (s32 iy = 0; iy < BLK; iy++) {
                    s32 CX1 = CY1, CX2 = CY2, CX3 = CY3;
                    for (s32 ix = 0; ix < BLK; ix++) {
                        if (CX1 > 0 && CX2 > 0 && CX3 > 0) {
                            if (x + ix >= minx && x + ix < maxx && y + iy >= miny && y + iy < maxy)
                                raster_pixel(t, x + ix, y + iy, ix, iy);
                        }
                        CX1 -= FDY12; CX2 -= FDY23; CX3 -= FDY31;
                    }
                    CY1 += FDX12; CY2 += FDX23; CY3 += FDX31;
                }
            }
        }
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
    u64 t0 = __rdtsc();
    draw_triangle_impl(t, v0, v1, v2);
    s_tsc_tri += __rdtsc() - t0;
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
    int m = calc_clip_mask(v0) & calc_clip_mask(v1) & calc_clip_mask(v2);
    if (m != 0) return;   /* trivially rejected */

    int backface = is_backface(v0, v1, v2);
    u32 cull = s_cfg.cullmode;
    if (!backface) { if (cull == 1 || cull == 3) return; }   /* cull Back/All */
    else           { if (cull == 2 || cull == 3) return; }   /* cull Front/All */

    for (int i = 3; i < NUM_CLIPPED + 3; i++) s_verts[i] = &s_clipped[i - 3];
    int indices[NUM_CLIPPED + 3];
    for (int i = 0; i < NUM_CLIPPED + 3; i++) indices[i] = -1;
    indices[0] = 0; indices[1] = 1; indices[2] = 2;
    int numIndices = 3;

    if (backface) { s_verts[0] = v0; s_verts[1] = v2; s_verts[2] = v1; }
    else          { s_verts[0] = v0; s_verts[1] = v1; s_verts[2] = v2; }

    int mask = calc_clip_mask(s_verts[0]) | calc_clip_mask(s_verts[1]) | calc_clip_mask(s_verts[2]);
    if (mask != 0) clip_triangle(indices, &numIndices, mask);

    for (int i = 0; i + 3 <= numIndices; i += 3) {
        if (indices[i] == -1) continue;
        perspective_divide(s_verts[indices[i]]);
        perspective_divide(s_verts[indices[i + 1]]);
        perspective_divide(s_verts[indices[i + 2]]);
        draw_triangle(t, s_verts[indices[i]], s_verts[indices[i + 1]], s_verts[indices[i + 2]]);
    }
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

static void tf_position(const InVtx* in, OutVtx* out) {
    float m[12]; get_posmat(in->posMtx, m);
    mul_vec3_mat34(in->position, m, out->mvPosition);
    u32 ptype = s_xf[0x1026];      /* ProjectionType (0 persp, 1 ortho) */
    float p[6]; for (int i = 0; i < 6; i++) p[i] = xf_f(0x1020 + i);
    if (ptype == 0) {              /* Perspective (MultipleVec3Perspective) */
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

static void tf_color(const InVtx* in, OutVtx* out) {
    u32 numchan = gm_numcolchans();
    for (u32 chan = 0; chan < 2; chan++) {
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
                TRAP(ambvtx, "vertex ambient color");
                lightCol[0] = lightCol[1] = lightCol[2] = 0;
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
            if (bits(alphareg, 6, 1)) { TRAP(ambavtx, "vertex ambient alpha"); la = 0; }
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
        (void)numchan;
    }
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
        if (texgentype != 0) { TRAP(texgentype, "tex gen type != Regular"); continue; }

        float src[3];
        if (sourcerow == 0) { src[0]=in->position[0]; src[1]=in->position[1]; src[2]=in->position[2]; }
        else if (sourcerow == 1) { src[0]=out->normal[0][0]; src[1]=out->normal[0][1]; src[2]=out->normal[0][2]; }
        else if (sourcerow >= 5 && sourcerow <= 12) {
            u32 tn = sourcerow - 5;
            src[0] = out->texCoords[tn][0]; src[1] = out->texCoords[tn][1]; src[2] = 1.0f;
            /* texCoords not yet transformed for source; use input tex coords */
            src[0] = in->texCoords[tn][0]; src[1] = in->texCoords[tn][1]; src[2] = 1.0f;
        } else { TRAP(texsrcrow, "tex source row (binormal)"); src[0]=src[1]=0; src[2]=1; }

        float m[12]; get_texmat(in->texMtx[c], m);
        float* dst = out->texCoords[c];
        if (projection == 0) {   /* ST */
            if (inputform == 0) mul_vec2_mat24(src, m, dst);
            else                mul_vec3_mat24(src, m, dst);
        } else {                 /* STQ */
            if (inputform == 0) mul_vec2_mat34(src, m, dst);
            else                mul_vec3_mat34_tex(src, m, dst);
        }
        if (dualtex) {
            u32 pinfo = s_xf[0x1050 + c];
            u32 pidx = bits(pinfo, 0, 6);
            int norm = bits(pinfo, 8, 1);
            float tmp[3];
            if (norm) { tmp[0]=dst[0]; tmp[1]=dst[1]; tmp[2]=dst[2]; normalize3(tmp); }
            else { tmp[0]=dst[0]; tmp[1]=dst[1]; tmp[2]=dst[2]; }
            float pm[12]; get_postmat((u8)pidx, pm);
            mul_vec3_mat34(tmp, pm, dst);
        }
        if (dst[2] == 0.0f) {
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
        if (posfmt != 4) { TRAP(posfmt, "indexed position format != float"); return 0; }
        u32 idx = read_index(v, &off, postype);
        u32 n = poselem ? 3 : 2;
        const u8* p = array_ptr(cp, 0, idx, n * 4);
        if (!p) { TRAP(posoob, "position array out of MEM1"); return 0; }
        out->position[0] = be_f32(p);
        out->position[1] = be_f32(p + 4);
        out->position[2] = (n == 3) ? be_f32(p + 8) : 0.0f;
    }

    /* normal (Index only; short, /2^14 per FracAdjust) */
    u32 normtype = (low >> 11) & 3;
    if (normtype != VCF_NONE) {
        u32 normfmt = (g0 >> 10) & 7, normelem = (g0 >> 9) & 1;
        if (normtype == VCF_DIRECT) { TRAP(directnorm, "direct normal attribute"); return 0; }
        if (normfmt != 3) { TRAP(normfmt, "normal format != short"); return 0; }
        if (normelem != 0) { TRAP(normntb, "normal NTB (tangent/binormal)"); return 0; }
        u32 idx = read_index(v, &off, normtype);
        const u8* p = array_ptr(cp, 1, idx, 6);
        if (!p) { TRAP(normoob, "normal array out of MEM1"); return 0; }
        out->normal[0][0] = be_s16(p)     / (float)(1 << 14);
        out->normal[0][1] = be_s16(p + 2) / (float)(1 << 14);
        out->normal[0][2] = be_s16(p + 4) / (float)(1 << 14);
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
        /* VAT tex fields (only tc0 in scope). */
        u32 fmt, elem, frac;
        if (tc == 0) { elem = (g0 >> 21) & 1; fmt = (g0 >> 22) & 7; frac = (g0 >> 25) & 0x1f; }
        else { TRAP(tcgt0, "texcoord index > 0"); return 0; }
        u32 n = elem ? 2 : 1;
        float scale = ((fmt & 7u) < 4u) ? 1.0f / (float)(1u << frac) : 1.0f;
        if (tctype == VCF_DIRECT) {
            float t[2] = {0.0f, 0.0f};
            for (u32 i = 0; i < n; i++) t[i] = read_direct_elem(v, &off, fmt) * scale;
            out->texCoords[tc][0] = t[0]; out->texCoords[tc][1] = t[1];
        } else {
            if (fmt != 3) { TRAP(tcfmt, "indexed texcoord format != short"); return 0; }
            u32 idx = read_index(v, &off, tctype);
            const u8* p = array_ptr(cp, 4 + tc, idx, n * 2);
            if (!p) { TRAP(tcoob, "texcoord array out of MEM1"); return 0; }
            out->texCoords[tc][0] = be_s16(p) * scale;
            out->texCoords[tc][1] = (n == 2) ? be_s16(p + 2) * scale : 0.0f;
        }
    }

    (void)vstride;
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
static void build_draw_cfg(void) {
    /* Bump the per-draw texel cache's generation (see the big "Per-draw texel
     * cache" comment above tex_sample) — this is the ENTIRE invalidation cost
     * for a new draw: one integer increment, no memset. Skip stored-gen 0 (it
     * is the "slot never written since last full clear" sentinel); on the
     * u32 wraparound that lands exactly on 0, pay for one full-table clear
     * (a few dozen KB memset — happens once every ~4 billion draws, i.e.
     * never in practice) and resume at 1. */
    if (++s_texel_cache_gen == 0) {
        memset(s_texel_cache, 0, sizeof s_texel_cache);
        s_texel_cache_gen = 1;
    }

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

    /* indstages/ztex/fog (Tev::Draw guard, Tev.cpp:387-390): BP-genmode/ztex2/
     * fogparam3 conditions only — no per-pixel data involved — so the trap
     * check itself (not just its decode) moves here: same trap-once message,
     * evaluated once per draw instead of once per pixel. */
    if (gm_numindstages() != 0) TRAP(indstages, "indirect TEV stages");
    if (bits(s_bp[0xF5], 2, 2) != 0) TRAP(ztex, "z-texture (ztex2.op)");
    if (bits(s_bp[0xF1], 21, 3) != 0) TRAP(fog, "fog (FogParam3.fsel)");

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

        u32 tevind = s_bp[0x10 + stage];
        sc->tevind_active = (bits(tevind, 9, 2) != 0 || bits(tevind, 7, 2) != 0);

        sc->tswap_id = bits(ac, 2, 2);
        sc->rswap_id = bits(ac, 0, 2);

        /* konst for this stage (kcsel/kasel, BPMemory AllTevKSels) — resolved
         * once, not just its selector (see TevStageCfg comment). */
        u32 ksel = s_bp[0xF6 + s2];
        u32 kc = odd ? bits(ksel, 14, 5) : bits(ksel, 4, 5);
        u32 ka = odd ? bits(ksel, 19, 5) : bits(ksel, 9, 5);
        { s16 r, g, b, a;
          konst_lookup(&s_tev, kc, &r, &g, &b, &a);
          sc->stage_konst.r = r; sc->stage_konst.g = g; sc->stage_konst.b = b;
          konst_lookup(&s_tev, ka, &r, &g, &b, &a); sc->stage_konst.a = a; }

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

        u32 mode0 = tx_mode0(unit), mode1 = tx_mode1(unit);
        tc->wrap_s = bits(mode0, 0, 2);
        tc->wrap_t = bits(mode0, 2, 2);
        tc->mipmapfilter_bad = (bits(mode0, 5, 2) != 0);
        tc->magf = bits(mode0, 4, 1);
        tc->minf = bits(mode0, 7, 1);
        tc->lod_edge = (int)bits(mode0, 8, 1);
        { int bias = (int)sext(bits(mode0, 9, 8), 8); tc->lod_bias_half = bias >> 1; }
        tc->minlod = bits(mode1, 0, 8);
        tc->maxlod = bits(mode1, 8, 8);
    }

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
            fprintf(stderr, "[gx-census] NEW config #%d hash=%08x stages=%u texgens=%u "
                    "colchans=%u ztest=%u/%u/func%u zupd=%u blend=%u logic=%u/%u sub=%u "
                    "dither=%u sf=%u df=%u da=%u/%u at=0x%06X",
                    free_slot, h, s_cfg.numtevstages + 1u, s_cfg.numtexgens,
                    s_cfg.numcolchans, s_cfg.zt_enable, s_cfg.zt_early, s_cfg.zt_func,
                    s_zt_upd, s_cfg.bm_blend_enable, s_cfg.bm_logic_enable,
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
        if (s_census_cur >= 0) s_census[s_census_cur].draws++;
    }
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
static void gx_raster_draw_impl(const GxCpState* cp, u32 prim, u32 vat,
                    const u8* verts, u32 nverts, u32 vstride) {
    /* prim 0/1 = GX_DRAW_QUADS / GX_DRAW_QUADS_2 (SetupUnit.cpp:34-40 routes
     * both to SetupQuad — Dolphin itself treats QUADS_2 as a non-standard
     * alias, not a distinct assembly). prim 4 = GX_DRAW_TRIANGLE_FAN. */
    s_trap_cp = cp; s_trap_vat = vat; s_trap_prim = prim;

    int is_quad = (prim == 0 || prim == 1);
    if (!is_quad && prim != 4) {
        TRAPF(nonfan, "primitive != TRIANGLE_FAN/QUADS (prim %u opcode-class, vat %u, "
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
    if (nverts < 3) return;

    recompute_scissor();
    tev_load_registers(&s_tev);
    build_draw_cfg();

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

        if (is_quad) {
            /* SetupQuad (SetupUnit.cpp:62-79): triangle 1 = (v0,v1,v2), then
             * triangle 2 = (v0,v2,v3) — the VertPointer/counter/write-pointer
             * state returns to its initial layout every 4 vertices (traced by
             * hand against the transcribed steps below), so additional quads
             * in the same draw call repeat identically. */
            if (counter < 2) { counter++; writep = pv[counter]; continue; }
            process_triangle(&s_tev, pv[0], pv[1], pv[2]);
            counter++;
            counter &= 3;
            writep = &store[counter & 1];
            OutVtx* temp = pv[1];
            pv[1] = pv[2];
            pv[2] = temp;
        } else {
            /* SetupTriFan (SetupUnit.cpp:114-130). */
            if (counter < 2) { counter++; writep = pv[counter]; continue; }
            process_triangle(&s_tev, pv[0], pv[1], pv[2]);
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

    if (!s_draw_stats) {
        gx_raster_draw_impl(cp, prim, vat, verts, nverts, vstride);
        return;
    }

    u64 tri_before = s_tsc_tri;
    u64 t0 = __rdtsc();
    gx_raster_draw_impl(cp, prim, vat, verts, nverts, vstride);
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
void gx_raster_print_census(void) {
    if (s_tev_census != 1) return;
    u64 total_px = 0;
    for (int i = 0; i < GX_CENSUS_MAX; i++)
        if (s_census[i].used) total_px += s_census[i].pixels;
    if (total_px == 0) return;
    fprintf(stderr, "[gx-census]");
    for (int i = 0; i < GX_CENSUS_MAX; i++) {
        if (!s_census[i].used) continue;
        fprintf(stderr, " #%d:%08x draws=%llu px=%llu(%.1f%%)",
                i, s_census[i].hash,
                (unsigned long long)s_census[i].draws,
                (unsigned long long)s_census[i].pixels,
                100.0 * (double)s_census[i].pixels / (double)total_px);
    }
    fprintf(stderr, "\n");
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
    for (int y = top; y <= bottom; y++) {
        for (int x = left; x <= right; x++) {
            u32 off = (u32)x + (u32)y * EFB_WIDTH;
            /* SetColor honors color_update/alpha_update (bp 0x41, cached by
             * build_efb_cfg() into the shared s_bm_cu/s_bm_au — this is a
             * per-clear-rect constant, not a per-pixel one). */
            if (s_bm_cu) {
                if (s_bm_au) SetPixelAlphaColor(off, cc);
                else         SetPixelColorOnly(off, cc);
            } else if (s_bm_au) {
                SetPixelAlphaOnly(off, cc[ALP_C]);
            }
            /* SetDepth honors zmode.update_enable (bp 0x40, cached as s_zt_upd). */
            if (s_zt_upd) SetPixelDepth(off, clearZ);
        }
    }
}

/* filtered YUV encode of one destination pixel column position x, EFB row y. */
static u32 get_efb_color(int x, int y) {
    if (x < 0) x = 0;
    if (x >= (int)EFB_WIDTH) x = (int)EFB_WIDTH - 1;
    if (y < 0) y = 0;
    if (y >= (int)EFB_HEIGHT) y = (int)EFB_HEIGHT - 1;
    return GetPixelColor((u32)x + (u32)y * EFB_WIDTH);
}

void gx_raster_efb_copy(const GxCpState* cp) {
    (void)cp;
    /* Own decode of the pixel_format/color_update/alpha_update/z-update quad
     * this call's GetPixelColor/efb_clear_rect calls need — see build_efb_cfg. */
    build_efb_cfg();
    u32 copy = s_bp[0x52];
    int clamp_top = bits(copy, 0, 1);
    int clamp_bottom = bits(copy, 1, 1);
    int scale_invert = bits(copy, 10, 1);
    int clear = bits(copy, 11, 1);
    int copy_to_xfb = bits(copy, 14, 1);
    u32 gamma = bits(copy, 7, 2);

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
        if (gamma != 0) TRAP(gammacopy, "EFB copy gamma != 1.0");

        /* copy filter coefficients (bp 0x53 Low, 0x54 High). */
        u32 flow = s_bp[0x53], fhigh = s_bp[0x54];
        int w0 = bits(flow, 0, 6), w1 = bits(flow, 6, 6), w2 = bits(flow, 12, 6), w3 = bits(flow, 18, 6);
        int w4 = bits(fhigh, 0, 6), w5 = bits(fhigh, 6, 6), w6 = bits(fhigh, 12, 6);

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
            for (int dy = 0; dy < dst_h; dy++) {
                int sy = top + (int)(dy / (yscale == 0 ? 1.0f : yscale) + 0.5f);
                if (sy >= bottom) sy = bottom - 1;
                int yprev = (clamp_top ? top : 0);
                if (sy - 1 > yprev) yprev = sy - 1;
                int ynext = (clamp_bottom ? bottom : (int)EFB_HEIGHT) - 1;
                if (sy + 1 < ynext) ynext = sy + 1;
                /* per-pixel filtered YUV (indices 1..src_w) */
                for (int i = 1, x = left; x < right; i++, x++) {
                    u32 c0 = get_efb_color(x, yprev);
                    u32 c1 = get_efb_color(x, sy);
                    u32 c2 = get_efb_color(x, ynext);
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
                scanY[0] = scanY[1]; scanU[0] = scanU[1]; scanV[0] = scanV[1];
                scanY[src_w + 1] = scanY[src_w]; scanU[src_w + 1] = scanU[src_w]; scanV[src_w + 1] = scanV[src_w];
                u8* row = s_cpu->ram + phys + (u64)dy * dest_stride;
                for (int i = 1, x = 0; x < src_w; i += 2, x += 2) {
                    int Y0 = scanY[i] + 16;
                    int UV0 = 128 + ((scanU[i - 1] + (scanU[i] << 1) + scanU[i + 1]) >> 2);
                    int Y1 = scanY[i + 1] + 16;
                    int UV1 = 128 + ((scanV[i - 1] + (scanV[i] << 1) + scanV[i + 1]) >> 2);
                    row[x * 2 + 0] = (u8)(Y0 < 0 ? 0 : Y0 > 255 ? 255 : Y0);
                    row[x * 2 + 1] = (u8)(UV0 < 0 ? 0 : UV0 > 255 ? 255 : UV0);
                    row[x * 2 + 2] = (u8)(Y1 < 0 ? 0 : Y1 > 255 ? 255 : Y1);
                    row[x * 2 + 3] = (u8)(UV1 < 0 ? 0 : UV1 > 255 ? 255 : UV1);
                }
            }
        }
    } else {
        TRAP(efbtex, "EFB->texture copy (copy_to_xfb=0)");
    }

    if (clear) efb_clear_rect();
}

/* ============================================================================
 * Init
 * ==========================================================================*/
void gx_raster_init(CPUState* cpu, const u32* bp, const u32* xf) {
    s_cpu = cpu; s_bp = bp; s_xf = xf;
    memset(s_efb_color, 0, sizeof s_efb_color);
    memset(s_efb_depth, 0, sizeof s_efb_depth);
    memset(&s_tev, 0, sizeof s_tev);
}
