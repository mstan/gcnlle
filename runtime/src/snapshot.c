/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SNAPSHOT_RESUME pass A (docs/SNAPSHOT_RESUME.md) — SAVE side only. See
 * include/debug/snapshot.h for the format and the entry point's contract.
 *
 * Every field written here is cited against the SAVE-side machine-state
 * survey (the implementation plan this pass follows); RESET-classified
 * state (native-code bitmaps, interpreter capture tables, rings, GX
 * draw-config/census/texel caches, gx_vulkan.c's entire GxVk struct) is
 * deliberately absent — it is reconstructed by the existing (or a future)
 * reset call at restore, never captured here.
 */
#include "debug/snapshot.h"
#include "debug/debug_server.h"
#include "dispatch/dispatch.h"
#include "memory/memory.h"
#include "gx/gx.h"
#include "gx/gx_raster.h"
#include "gx/gx_vulkan.h"
#include "util/crc32.h"
#include "dsp_lle_c.h"
#include "cpu/native_code.h"
#include "cpu/title_module.h"
#include "debug/rings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GCN_BUILD_GIT_SHA
#define GCN_BUILD_GIT_SHA "unknown"
#endif

/* ---- growable byte buffer -------------------------------------------- */

typedef struct {
    u8*    data;
    size_t len;
    size_t cap;
    int    ok;
} SnapBuf;

static void sb_init(SnapBuf* b) { b->data = NULL; b->len = 0; b->cap = 0; b->ok = 1; }

static void sb_free(SnapBuf* b) {
    free(b->data);
    b->data = NULL; b->len = 0; b->cap = 0;
}

static void sb_reserve(SnapBuf* b, size_t extra) {
    if (!b->ok) return;
    if (b->len + extra <= b->cap) return;
    size_t ncap = b->cap ? b->cap : (size_t)65536;
    while (ncap < b->len + extra) ncap *= 2;
    u8* nd = (u8*)realloc(b->data, ncap);
    if (!nd) { b->ok = 0; return; }
    b->data = nd;
    b->cap = ncap;
}

static void sb_put(SnapBuf* b, const void* p, size_t n) {
    sb_reserve(b, n);
    if (!b->ok) return;
    if (p && n) memcpy(b->data + b->len, p, n);
    else if (n) memset(b->data + b->len, 0, n); /* NULL p: zero-fill (absent device) */
    b->len += n;
}

static void sb_u8 (SnapBuf* b, u8  v) { sb_put(b, &v, sizeof v); }
static void sb_u16(SnapBuf* b, u16 v) { sb_put(b, &v, sizeof v); }
static void sb_u32(SnapBuf* b, u32 v) { sb_put(b, &v, sizeof v); }
static void sb_u64(SnapBuf* b, u64 v) { sb_put(b, &v, sizeof v); }
static void sb_f64(SnapBuf* b, f64 v) { sb_put(b, &v, sizeof v); }
static void sb_bool(SnapBuf* b, int v) { sb_u8(b, v ? 1 : 0); }

/* ---- read cursor (pass B restore) -------------------------------------
 * Mirrors SnapBuf for reading: a bounds-checked cursor over a fixed byte
 * range (one section's payload). Reading past the end is clamped and marks
 * `ok = 0` rather than reading out of bounds — a truncated/corrupt section
 * degrades to zeros instead of crashing. */
typedef struct {
    const u8* data;
    size_t    len;
    size_t    pos;
    int       ok;
} SnapCur;

static void cr_init(SnapCur* c, const u8* data, size_t len) {
    c->data = data; c->len = len; c->pos = 0; c->ok = 1;
}

static void cr_get(SnapCur* c, void* out, size_t n) {
    if (c->pos + n > c->len) {
        if (out && n) memset(out, 0, n);
        c->ok = 0;
        c->pos = c->len;
        return;
    }
    if (out && n) memcpy(out, c->data + c->pos, n);
    c->pos += n;
}

/* Like cr_get but for large fixed-destination buffers (RAM/ARAM/TMEM/EFB)
 * where `n` may legitimately be huge — same clamp-and-zero-fill behavior,
 * just without the fixed-size intermediate that cr_u32/etc. use. */
static void cr_bytes(SnapCur* c, void* out, size_t n) { cr_get(c, out, n); }

static u8  cr_u8 (SnapCur* c) { u8  v = 0; cr_get(c, &v, sizeof v); return v; }
static u16 cr_u16(SnapCur* c) { u16 v = 0; cr_get(c, &v, sizeof v); return v; }
static u32 cr_u32(SnapCur* c) { u32 v = 0; cr_get(c, &v, sizeof v); return v; }
static u64 cr_u64(SnapCur* c) { u64 v = 0; cr_get(c, &v, sizeof v); return v; }
static int cr_bool(SnapCur* c) { return cr_u8(c) != 0; }

/* ---- section tags (order here == on-disk order; see snapshot.h) ------ */

enum {
    GCN_SNAP_SEC_CPU_FIXED   = 1,
    GCN_SNAP_SEC_DISPATCH    = 2,
    GCN_SNAP_SEC_MEM1        = 3,
    GCN_SNAP_SEC_LOCKED_L1   = 4,
    GCN_SNAP_SEC_MEM2        = 5,
    GCN_SNAP_SEC_EXI         = 6,
    GCN_SNAP_SEC_MEMCARD_A   = 7,
    GCN_SNAP_SEC_MEMCARD_B   = 8,
    GCN_SNAP_SEC_VI          = 9,
    GCN_SNAP_SEC_SI          = 10,
    GCN_SNAP_SEC_PI          = 11,
    GCN_SNAP_SEC_MI          = 12,
    GCN_SNAP_SEC_AI          = 13,
    GCN_SNAP_SEC_DI          = 14,
    GCN_SNAP_SEC_ARAM        = 15,
    GCN_SNAP_SEC_DSP_CORE    = 16,
    GCN_SNAP_SEC_DSP_DEVICE  = 17,
    GCN_SNAP_SEC_GX_REGS     = 18,
    GCN_SNAP_SEC_TMEM        = 19,
    GCN_SNAP_SEC_EFB         = 20,
    /* SNAPSHOT_RESUME pass C: OPTIONAL section (the versioned TOC lets a
     * reader skip tags it doesn't recognize/care about — v1 pass-A/B blobs
     * simply lack this tag, and gcn_snapshot_load already treats a missing
     * FIND_SEC result as "section absent", not an error). Cumulative XFB
     * hash-chain state (gcn_gx_xfb_hash_get_state) + the IPL frame counter
     * (gcn_gx_frame_count), captured whenever GCN_GX_XFB_HASH=1 was set at
     * capture time — the production-tier suffix-chain-equality proof needs
     * the resumed run to CONTINUE the chain from here, not restart it. */
    GCN_SNAP_SEC_XFB_HASH    = 21,
    GCN_SNAP_SEC_COUNT_MAX   = 22   /* array sizing only; not written */
};

typedef struct {
    u32    tag;
    SnapBuf buf;
} SnapSection;

/* ---- per-device section writers --------------------------------------
 * Each writes ONLY the SAVE-classified fields (survey §7); pointer/callback
 * fields (irq/irq_user, persist hooks, borrowed device pointers, FILE*) and
 * RESET-classified caches are never touched here — see the per-field
 * comments below for the exact citation. */

/* CPUState: every DolRecomp-required field plus the runtime-only bookkeeping
 * fields that are genuinely guest-visible timing state (cycles,
 * cycle_deadline, halted, halt_reason). Excluded: ram/ram_size/mem2/mem2_size
 * (raw pointers — bytes captured separately as MEM1/MEM2 sections, see
 * gcn_snapshot_save), external_read/write/read32/write32/
 * instruction_fallback/host_call/external_user_data (host function
 * pointers — RESET, rebind at restore), rom_window/rom_window_size (RESET,
 * reconstructed by re-descrambling at restore). locked_cache_tag/valid ARE
 * included here (they are the guest-visible L1 TAG state); the 256 KiB L1
 * cache CONTENT backing store is a separate heap buffer (memory.c's
 * s_l1_cache) captured in its own section via gcn_mem_locked_l1. */
static void write_cpu_fixed(SnapBuf* b, const CPUState* cpu) {
    sb_put(b, cpu->gpr, sizeof cpu->gpr);
    sb_put(b, cpu->fpr, sizeof cpu->fpr);
    sb_put(b, cpu->ps1, sizeof cpu->ps1);
    sb_u32(b, cpu->pc); sb_u32(b, cpu->lr); sb_u32(b, cpu->ctr);
    sb_u32(b, cpu->cr); sb_u32(b, cpu->xer); sb_u32(b, cpu->fpscr);
    sb_u32(b, cpu->msr); sb_u32(b, cpu->srr0); sb_u32(b, cpu->srr1);
    sb_u32(b, cpu->dar); sb_u32(b, cpu->dsisr); sb_u32(b, cpu->ear);
    sb_u32(b, cpu->hid2);
    sb_u64(b, cpu->timebase);
    sb_put(b, cpu->sr, sizeof cpu->sr);
    sb_put(b, cpu->gqr, sizeof cpu->gqr);
    sb_put(b, cpu->spr, sizeof cpu->spr);
    sb_u32(b, cpu->exception);
    sb_u32(b, cpu->program_exception);
    sb_u32(b, cpu->tlb_last_vps);
    sb_u32(b, cpu->tlb_last_index);
    sb_u32(b, cpu->tlb_invalidate_count);
    sb_u32(b, cpu->external_addr);
    sb_u32(b, cpu->external_value);
    sb_u8(b, cpu->external_rid);
    sb_u8(b, cpu->external_read_count);
    sb_u8(b, cpu->external_write_count);
    sb_u32(b, cpu->reserve_addr);
    sb_bool(b, cpu->reserve_valid);
    sb_put(b, cpu->locked_cache_tag, sizeof cpu->locked_cache_tag);
    for (u32 i = 0; i < 512; i++) sb_bool(b, cpu->locked_cache_valid[i]);
    sb_u64(b, cpu->cycles);
    sb_u64(b, cpu->cycle_deadline);
    sb_bool(b, cpu->halted);
    sb_u32(b, (u32)cpu->halt_reason);
}

/* dispatch.c's own timing-model residue (dispatch.h). Not part of CPUState —
 * losing it perturbs derived-cycle-accuracy timing after restore. */
static void write_dispatch(SnapBuf* b) {
    u64 device_cycles = 0, prev_cycles = 0, tb_remainder = 0, dsp_remainder = 0;
    gcn_dispatch_timing_get(&device_cycles, &prev_cycles, &tb_remainder, &dsp_remainder);
    sb_u64(b, device_cycles);
    sb_u64(b, prev_cycles);
    sb_u64(b, tb_remainder);
    sb_u64(b, dsp_remainder);
}

/* EXI: channels[3], IPL ROM window byte offset bookkeeping is NOT included
 * (rom/rom_size/rom_base/owned_rom are RESET — the descrambled ROM is
 * reconstructed at restore, exi.h:112-120), sram[0x40], rtc (GcnRtc — all
 * three fields, rtc.h:18-22; NOTE per rtc.c's own doc this must be restored
 * via direct field assignment, never gcn_exi_sync_rtc_from_host), the
 * per-channel mid-transaction protocol state (op/rom_offset/dev_pos/
 * prev_cs/have_cmd), dev_present[3], irq_level. Excluded: card[3] (borrowed
 * pointers — rebind at restore), irq/irq_user, persist/persist_user,
 * card_persist/card_persist_user (host callbacks — RESET, rebind). */
static void write_exi(SnapBuf* b, const GcnExi* exi_in) {
    GcnExi zero; if (!exi_in) memset(&zero, 0, sizeof zero);
    const GcnExi* exi = exi_in ? exi_in : &zero;
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) {
        sb_u32(b, exi->channels[i].csr);
        sb_u32(b, exi->channels[i].mar);
        sb_u32(b, exi->channels[i].len);
        sb_u32(b, exi->channels[i].cr);
        sb_u32(b, exi->channels[i].data);
    }
    sb_put(b, exi->sram, sizeof exi->sram);
    sb_u32(b, exi->rtc.counter);
    sb_u64(b, exi->rtc.anchor_cycles);
    sb_u32(b, (u32)exi->rtc.running);
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) sb_u32(b, (u32)exi->op[i]);
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) sb_u32(b, exi->rom_offset[i]);
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) sb_u32(b, exi->dev_pos[i]);
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) sb_u8(b, exi->prev_cs[i]);
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) sb_u32(b, (u32)exi->have_cmd[i]);
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) sb_u8(b, exi->dev_present[i]);
    sb_u32(b, (u32)exi->irq_level);
}

/* Memory card: the full backing image (up to 16 Mbit / 2 MiB), plus every
 * per-transaction field (memcard.h:96-117) and `dirty`. No FILE pointer or
 * HANDLE exists anywhere in GcnMemcard (survey-confirmed) — nothing to
 * exclude on that front. A card slot that is empty/absent writes present=0 and no
 * image bytes (0-length). */
static void write_memcard(SnapBuf* b, const GcnMemcard* mc) {
    if (!mc || !mc->present) {
        sb_bool(b, 0);            /* present */
        sb_u32(b, 0);             /* size_bytes */
        return;
    }
    sb_bool(b, 1);
    sb_u32(b, mc->size_bytes);
    sb_u16(b, mc->size_mbits);
    sb_u16(b, mc->card_id);
    sb_u32(b, (u32)mc->command);
    sb_u32(b, mc->position);
    sb_u32(b, mc->address);
    sb_u32(b, (u32)mc->status);
    sb_put(b, mc->prog_buf, sizeof mc->prog_buf);
    sb_u32(b, (u32)mc->interrupt_switch);
    sb_u32(b, (u32)mc->interrupt_set);
    sb_u32(b, (u32)mc->dirty);
    sb_put(b, mc->data, mc->size_bytes);
}

/* VI: reg[128], half_line_count, now_cycles/last_cycles/
 * ticks_last_line_start, irq_level. Excluded (RESET): tphl_cache/tphl_key
 * (pure memo of two raw inputs, vi.c:54-59 — safe to recompute), irq/
 * si_poll_hook/field_hook + their user pointers (host callbacks). */
static void write_vi(SnapBuf* b, const GcnVi* vi_in) {
    GcnVi zero; if (!vi_in) memset(&zero, 0, sizeof zero);
    const GcnVi* vi = vi_in ? vi_in : &zero;
    sb_put(b, vi->reg, sizeof vi->reg);
    sb_u32(b, vi->half_line_count);
    sb_u64(b, vi->now_cycles);
    sb_u64(b, vi->last_cycles);
    sb_u64(b, vi->ticks_last_line_start);
    sb_u32(b, (u32)vi->irq_level);
}

/* SI: ch[4] (out/in_hi/in_lo/connected/rdst/norep/mode — real hardware
 * latches, si.h:167-178), poll/comcsr/sisr/exilk, iobuf[128], irq_level,
 * next_poll_halfline. `input` (the debug-injected pad report) is
 * deliberately NOT saved — task instruction: reset to power-on-neutral at
 * restore (GCN_SNAPSHOT_FLAG_SI_INPUT_RESET_NEUTRAL records this liberty in
 * the header). Excluded: irq/irq_user (host callback). */
static void write_si(SnapBuf* b, const GcnSi* si_in) {
    GcnSi zero; if (!si_in) memset(&zero, 0, sizeof zero);
    const GcnSi* si = si_in ? si_in : &zero;
    for (u32 i = 0; i < GCN_SI_CHANNELS; i++) {
        sb_u32(b, si->ch[i].out);
        sb_u32(b, si->ch[i].in_hi);
        sb_u32(b, si->ch[i].in_lo);
        sb_u8(b, si->ch[i].connected);
        sb_u8(b, si->ch[i].rdst);
        sb_u8(b, si->ch[i].norep);
        sb_u8(b, si->ch[i].mode);
    }
    sb_u32(b, si->poll);
    sb_u32(b, si->comcsr);
    sb_u32(b, si->sisr);
    sb_u32(b, si->exilk);
    sb_put(b, si->iobuf, sizeof si->iobuf);
    sb_u32(b, (u32)si->irq_level);
    sb_u32(b, si->next_poll_halfline);
}

/* PI: reg[0x40], intsr (the live interrupt-cause word). Caller must have
 * quiesced the GX/DSP worker threads before this is read (intsr is
 * cross-thread __atomic_* state, pi.c:46-51) — gcn_snapshot_save's caller
 * contract (parked dispatcher boundary) already guarantees this: the park
 * happens after gcn_gx_pipeline_drain, which joins the GX worker's queued
 * work, and the DSP worker (if GCN_DSP_THREAD=1) is drained by every MMIO
 * observation this capture performs (gcn_dsp_flush et al). Excluded:
 * fifo_reset_hook/reset_drive_hook (host callbacks). */
static void write_pi(SnapBuf* b, const GcnPi* pi_in) {
    GcnPi zero; if (!pi_in) memset(&zero, 0, sizeof zero);
    const GcnPi* pi = pi_in ? pi_in : &zero;
    sb_put(b, pi->reg, sizeof pi->reg);
    sb_u32(b, __atomic_load_n(&pi->intsr, __ATOMIC_ACQUIRE));
}

/* MI: reg[0x800] — plain POD register file, zero side effects, nothing to
 * exclude (mi.h's own doc: "zero side effects"). */
static void write_mi(SnapBuf* b, const GcnMi* mi_in) {
    GcnMi zero; if (!mi_in) memset(&zero, 0, sizeof zero);
    const GcnMi* mi = mi_in ? mi_in : &zero;
    sb_put(b, mi->reg, sizeof mi->reg);
}

/* AI: control/volume/sample_counter/int_timing, BOTH pacing accumulators
 * (tick_accum legacy + cycle_accum_x2 derived — ai.h's own doc explains why
 * both must travel regardless of which mode is live), irq_level. Excluded:
 * irq/irq_user (host callback). */
static void write_ai(SnapBuf* b, const GcnAi* ai_in) {
    GcnAi zero; if (!ai_in) memset(&zero, 0, sizeof zero);
    const GcnAi* ai = ai_in ? ai_in : &zero;
    sb_u32(b, ai->control);
    sb_u32(b, ai->volume);
    sb_u32(b, ai->sample_counter);
    sb_u32(b, ai->int_timing);
    sb_u32(b, ai->tick_accum);
    sb_u64(b, ai->cycle_accum_x2);
    sb_u32(b, (u32)ai->irq_level);
}

/* DI: disr/dicvr/cmdbuf[3]/dimar/dilength/dicr/diimmbuf, drive_state,
 * error_code, enable_dtk, THE deferred-completion pair cmd_pending/
 * pending_intr (di.h:231-234 — the literal "queued disc command" state a
 * capture between the DICR write and the next gcn_di_tick would otherwise
 * lose), irq_level, disc_size, disc_present. Excluded: disc_file (FILE* —
 * di.h:246, the confirmed hazard; restore must re-fopen the same path, never
 * byte-copy the handle), irq/irq_user (host callback). The mounted disc
 * PATH itself is not tracked anywhere in GcnDi (di.c owns only the open
 * FILE*), so pass A cannot record it for a future restore to re-open — flag
 * for pass B: either di.c needs to remember the path string, or the
 * snapshot caller must supply it out-of-band (e.g. GCN_DISC at restore
 * time, matching how it's supplied at boot). */
static void write_di(SnapBuf* b, const GcnDi* di_in) {
    GcnDi zero;
    if (!di_in) { memset(&zero, 0, sizeof zero); zero.disc_file = NULL; }
    const GcnDi* di = di_in ? di_in : &zero;
    sb_u32(b, di->disr);
    sb_u32(b, di->dicvr);
    sb_put(b, di->cmdbuf, sizeof di->cmdbuf);
    sb_u32(b, di->dimar);
    sb_u32(b, di->dilength);
    sb_u32(b, di->dicr);
    sb_u32(b, di->diimmbuf);
    sb_u8(b, di->drive_state);
    sb_u32(b, di->error_code);
    sb_u8(b, di->enable_dtk);
    sb_u8(b, di->cmd_pending);
    sb_u8(b, di->pending_intr);
    sb_u32(b, (u32)di->irq_level);
    sb_u64(b, di->disc_size);
    sb_u32(b, (u32)di->disc_present);
}

/* GX register files: BP[256] + XF[gcn_gx_xf_words()] (gx.c's GcnGx mirror,
 * gcn_gx_bp/gcn_gx_xf accessors) followed by GcnCp.reg[] and GcnPe.reg[]
 * plus PE's token/enable/signal state. TMEM and the EFB planes are their
 * own sections (large, and gx_raster.h already exposes read-only pointers
 * for them). CP/PE per-field decoded-enable ints (gp_read_enable etc.) are
 * NOT separately stored: they are pure decodes of CTRL, already present in
 * reg[] (cp.c's write path recomputes them from reg[GCN_CP_CTRL] on
 * restore-equivalent re-evaluation) — but pass A stores them anyway since
 * they are real struct fields and the cost is negligible, to avoid betting
 * pass B's restore path on re-deriving them correctly on the first attempt.
 * Excluded: GcnCp/GcnPe's irq/irq_user (host callbacks); the GcnGx buf/
 * buf_len staging and CP-state mirror are transient decode-in-progress
 * state that gcn_gx_confirm_drained already asserts is empty before this
 * is ever called, so there is nothing live to capture there. */
static void write_gx_regs(SnapBuf* b, const GcnCp* cp_in, const GcnPe* pe_in) {
    sb_put(b, gcn_gx_bp(), 256 * sizeof(u32));
    sb_u32(b, gcn_gx_xf_words());
    sb_put(b, gcn_gx_xf(), (size_t)gcn_gx_xf_words() * sizeof(u32));

    GcnCp cp_zero; if (!cp_in) memset(&cp_zero, 0, sizeof cp_zero);
    const GcnCp* cp = cp_in ? cp_in : &cp_zero;
    sb_put(b, cp->reg, sizeof cp->reg);
    sb_u32(b, (u32)cp->gp_read_enable);
    sb_u32(b, (u32)cp->bp_enable);
    sb_u32(b, (u32)cp->hi_wm_int);
    sb_u32(b, (u32)cp->lo_wm_int);
    sb_u32(b, (u32)cp->gp_link_enable);
    sb_u32(b, (u32)cp->bp_int);
    sb_u32(b, (u32)cp->hi_wm);
    sb_u32(b, (u32)cp->lo_wm);
    sb_u32(b, (u32)cp->breakpoint);
    sb_u32(b, (u32)cp->irq_level);

    GcnPe pe_zero; if (!pe_in) memset(&pe_zero, 0, sizeof pe_zero);
    const GcnPe* pe = pe_in ? pe_in : &pe_zero;
    sb_put(b, pe->reg, sizeof pe->reg);
    sb_u16(b, pe->token);
    sb_u32(b, (u32)pe->token_enable);
    sb_u32(b, (u32)pe->finish_enable);
    sb_u32(b, (u32)pe->signal_token_interrupt);
    sb_u32(b, (u32)pe->signal_finish_interrupt);
    sb_u32(b, (u32)pe->token_level);
    sb_u32(b, (u32)pe->finish_level);
}

/* ---- assembly ---------------------------------------------------------- */

static int write_section(SnapBuf* out, u32 tag, const SnapBuf* payload) {
    if (!out->ok || !payload->ok) return 0;
    sb_u32(out, tag);
    sb_u64(out, (u64)payload->len);
    sb_put(out, payload->data, payload->len);
    return out->ok;
}

int gcn_snapshot_save(const char* path, CPUState* cpu, char* why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (!path || !cpu) {
        if (why) snprintf(why, why_size, "null path/cpu");
        return GCN_SNAPSHOT_ERR_IO;
    }

    const GcnDebugCtx* ctx = gcn_debug_server_ctx();

    /* Drain, then hard-assert. gcn_gx_pipeline_drain joins the GX worker and
     * forces gx_render_flush -> gx_vulkan_resident_flush (materializing any
     * resident-Vulkan batch to guest RAM/software EFB) BEFORE the assert —
     * matching the spec's "park first, drain, then dump". */
    gcn_gx_pipeline_drain();
    {
        char drain_why[256];
        if (!gcn_gx_confirm_drained(ctx->gp, ctx->cp, drain_why, sizeof drain_why)) {
            if (why) snprintf(why, why_size, "not drained: %s", drain_why);
            return GCN_SNAPSHOT_ERR_NOT_DRAINED;
        }
    }

    GcnDspLleSnapshot dsp_core;
    int have_dsp_core = dsp_lle_save_state(&dsp_core);
    if (!have_dsp_core) memset(&dsp_core, 0, sizeof dsp_core);

    /* Build every section payload. */
    SnapSection sec[GCN_SNAP_SEC_COUNT_MAX];
    u32 nsec = 0;
    int ok = 1;

#define BEGIN_SEC(TAG) do { sb_init(&sec[nsec].buf); sec[nsec].tag = (TAG); } while (0)
#define END_SEC() do { ok = ok && sec[nsec].buf.ok; nsec++; } while (0)

    BEGIN_SEC(GCN_SNAP_SEC_CPU_FIXED);
    write_cpu_fixed(&sec[nsec].buf, cpu);
    END_SEC();

    BEGIN_SEC(GCN_SNAP_SEC_DISPATCH);
    write_dispatch(&sec[nsec].buf);
    END_SEC();

    BEGIN_SEC(GCN_SNAP_SEC_MEM1);
    sb_put(&sec[nsec].buf, cpu->ram, cpu->ram_size);
    END_SEC();

    BEGIN_SEC(GCN_SNAP_SEC_LOCKED_L1);
    {
        u32 l1_size = 0;
        u8* l1 = gcn_mem_locked_l1(cpu, &l1_size);
        sb_u32(&sec[nsec].buf, l1_size);
        sb_put(&sec[nsec].buf, l1, l1_size);
    }
    END_SEC();

    BEGIN_SEC(GCN_SNAP_SEC_MEM2);
    sb_u32(&sec[nsec].buf, cpu->mem2_size);
    sb_put(&sec[nsec].buf, cpu->mem2, cpu->mem2_size);
    END_SEC();

    BEGIN_SEC(GCN_SNAP_SEC_EXI);
    write_exi(&sec[nsec].buf, ctx->exi);
    END_SEC();

    BEGIN_SEC(GCN_SNAP_SEC_MEMCARD_A);
    write_memcard(&sec[nsec].buf, ctx->exi && ctx->exi->card[0] ? ctx->exi->card[0] : NULL);
    END_SEC();

    BEGIN_SEC(GCN_SNAP_SEC_MEMCARD_B);
    write_memcard(&sec[nsec].buf, ctx->exi && ctx->exi->card[1] ? ctx->exi->card[1] : NULL);
    END_SEC();

    BEGIN_SEC(GCN_SNAP_SEC_VI);
    write_vi(&sec[nsec].buf, ctx->vi);
    END_SEC();

    BEGIN_SEC(GCN_SNAP_SEC_SI);
    write_si(&sec[nsec].buf, ctx->si);
    END_SEC();

    BEGIN_SEC(GCN_SNAP_SEC_PI);
    write_pi(&sec[nsec].buf, ctx->pi);
    END_SEC();

    BEGIN_SEC(GCN_SNAP_SEC_MI);
    write_mi(&sec[nsec].buf, ctx->mi);
    END_SEC();

    BEGIN_SEC(GCN_SNAP_SEC_AI);
    write_ai(&sec[nsec].buf, ctx->ai);
    END_SEC();

    BEGIN_SEC(GCN_SNAP_SEC_DI);
    write_di(&sec[nsec].buf, ctx->di);
    END_SEC();

    BEGIN_SEC(GCN_SNAP_SEC_ARAM);
    {
        u8* aram = dsp_lle_aram();
        u32 aram_size = dsp_lle_aram_size();
        sb_u32(&sec[nsec].buf, aram_size);
        sb_put(&sec[nsec].buf, aram, aram_size);
    }
    END_SEC();

    BEGIN_SEC(GCN_SNAP_SEC_DSP_CORE);
    sb_put(&sec[nsec].buf, &dsp_core, sizeof dsp_core);
    END_SEC();

    BEGIN_SEC(GCN_SNAP_SEC_DSP_DEVICE);
    {
        GcnDsp dsp_zero; if (!ctx->dsp) memset(&dsp_zero, 0, sizeof dsp_zero);
        const GcnDsp* dsp = ctx->dsp ? ctx->dsp : &dsp_zero;
        sb_put(&sec[nsec].buf, dsp->reg, sizeof dsp->reg);
        sb_u16(&sec[nsec].buf, dsp->csr);
        sb_bool(&sec[nsec].buf, dsp->dma_active);
        sb_u32(&sec[nsec].buf, dsp->dma_cycles_left);
        sb_u32(&sec[nsec].buf, dsp->aid_source);
        sb_u16(&sec[nsec].buf, dsp->aid_ctrl);
        sb_u32(&sec[nsec].buf, dsp->aid_cur_addr);
        sb_u16(&sec[nsec].buf, dsp->aid_blocks_left);
        sb_u8(&sec[nsec].buf, dsp->aid_int_pending);
        sb_u32(&sec[nsec].buf, dsp->aid_accum);
        sb_u32(&sec[nsec].buf, (u32)dsp->irq_level);
    }
    END_SEC();

    BEGIN_SEC(GCN_SNAP_SEC_GX_REGS);
    write_gx_regs(&sec[nsec].buf, ctx->cp, ctx->pe);
    END_SEC();

    BEGIN_SEC(GCN_SNAP_SEC_TMEM);
    sb_put(&sec[nsec].buf, gcn_gx_tmem(), 0x100000u);
    END_SEC();

    BEGIN_SEC(GCN_SNAP_SEC_EFB);
    {
        const u32 *color = NULL, *depth = NULL;
        u32 w = 0, h = 0;
        gx_raster_efb_data(&color, &depth, &w, &h);
        sb_u32(&sec[nsec].buf, w);
        sb_u32(&sec[nsec].buf, h);
        sb_put(&sec[nsec].buf, color, (size_t)w * h * sizeof(u32));
        sb_put(&sec[nsec].buf, depth, (size_t)w * h * sizeof(u32));
    }
    END_SEC();

    BEGIN_SEC(GCN_SNAP_SEC_XFB_HASH);
    {
        /* Written unconditionally (harmless — restore only ever seeds from
         * it when GCN_GX_XFB_HASH=1 in the RESUMING process too); the chain
         * value is meaningless if hashing was off at capture (stays at the
         * FNV-1a offset basis, pubs stays 0), which restore's own
         * GCN_GX_XFB_HASH=1 gate makes harmless either way. */
        u64 chain = 0, pubs = 0;
        gcn_gx_xfb_hash_get_state(&chain, &pubs);
        sb_u64(&sec[nsec].buf, chain);
        sb_u64(&sec[nsec].buf, pubs);
        sb_u64(&sec[nsec].buf, gcn_gx_frame_count());
    }
    END_SEC();

#undef BEGIN_SEC
#undef END_SEC

    if (!ok) {
        for (u32 i = 0; i < nsec; i++) sb_free(&sec[i].buf);
        if (why) snprintf(why, why_size, "out of memory building snapshot sections");
        return GCN_SNAPSHOT_ERR_ALLOC;
    }

    /* Header: magic, version, commit, provisional content-identity hash
     * (see snapshot.h's doc comment — MEM1 CRC stand-in until a real
     * disc/DOL identity exists), liberties flags, section count, TOC. */
    u32 mem1_crc = gcn_crc32(cpu->ram, cpu->ram_size);
    char commit[GCN_SNAPSHOT_COMMIT_LEN];
    memset(commit, 0, sizeof commit);
    snprintf(commit, sizeof commit, "%s", GCN_BUILD_GIT_SHA);

    SnapBuf out; sb_init(&out);
    sb_u32(&out, GCN_SNAPSHOT_MAGIC);
    sb_u32(&out, GCN_SNAPSHOT_FORMAT_VERSION);
    sb_put(&out, commit, sizeof commit);
    sb_u32(&out, mem1_crc);
    sb_u32(&out, GCN_SNAPSHOT_FLAG_SI_INPUT_RESET_NEUTRAL);
    sb_u32(&out, nsec);

    /* TOC offsets: header is now fixed-size (everything written above plus
     * nsec * (4+8+8) TOC entries); sections follow immediately after. */
    size_t header_size = out.len + (size_t)nsec * (4 + 8 + 8);
    size_t running = header_size;
    for (u32 i = 0; i < nsec; i++) {
        sb_u32(&out, sec[i].tag);
        sb_u64(&out, (u64)running);
        sb_u64(&out, (u64)sec[i].buf.len);
        running += 4 + 8 + sec[i].buf.len;   /* per-section inline tag+length prefix too */
    }

    for (u32 i = 0; i < nsec; i++)
        write_section(&out, sec[i].tag, &sec[i].buf);

    for (u32 i = 0; i < nsec; i++) sb_free(&sec[i].buf);

    if (!out.ok) {
        sb_free(&out);
        if (why) snprintf(why, why_size, "out of memory assembling snapshot blob");
        return GCN_SNAPSHOT_ERR_ALLOC;
    }

    u32 footer_crc = gcn_crc32(out.data, out.len);
    sb_u32(&out, footer_crc);

    FILE* f = fopen(path, "wb");
    if (!f) {
        sb_free(&out);
        if (why) snprintf(why, why_size, "fopen('%s', \"wb\") failed", path);
        return GCN_SNAPSHOT_ERR_IO;
    }
    size_t blob_len = out.len;
    size_t written = fwrite(out.data, 1, blob_len, f);
    int close_ok = (fclose(f) == 0);
    sb_free(&out);
    if (written != blob_len || !close_ok) {
        remove(path);
        if (why) snprintf(why, why_size, "fwrite/fclose failed for '%s'", path);
        return GCN_SNAPSHOT_ERR_IO;
    }

    return GCN_SNAPSHOT_OK;
}

/* =========================================================================
 * SNAPSHOT_RESUME pass B — RESTORE side.
 *
 * Every read_XXX function below is the load-mirror of the matching write_XXX
 * above: same field order, same exclusions (host pointers/callbacks/FILE*
 * are never read back — they're already correctly (re)bound by boot.c's
 * normal device-construction sequence, which MUST have already run before
 * gcn_snapshot_load is called; see snapshot.h's contract comment). Sections
 * are located via the TOC (tag/offset/length), so read order does not need
 * to match write order.
 * ========================================================================= */

static void read_cpu_fixed(SnapCur* c, CPUState* cpu) {
    cr_bytes(c, cpu->gpr, sizeof cpu->gpr);
    cr_bytes(c, cpu->fpr, sizeof cpu->fpr);
    cr_bytes(c, cpu->ps1, sizeof cpu->ps1);
    cpu->pc = cr_u32(c); cpu->lr = cr_u32(c); cpu->ctr = cr_u32(c);
    cpu->cr = cr_u32(c); cpu->xer = cr_u32(c); cpu->fpscr = cr_u32(c);
    cpu->msr = cr_u32(c); cpu->srr0 = cr_u32(c); cpu->srr1 = cr_u32(c);
    cpu->dar = cr_u32(c); cpu->dsisr = cr_u32(c); cpu->ear = cr_u32(c);
    cpu->hid2 = cr_u32(c);
    cpu->timebase = cr_u64(c);
    cr_bytes(c, cpu->sr, sizeof cpu->sr);
    cr_bytes(c, cpu->gqr, sizeof cpu->gqr);
    cr_bytes(c, cpu->spr, sizeof cpu->spr);
    cpu->exception = cr_u32(c);
    cpu->program_exception = cr_u32(c);
    cpu->tlb_last_vps = cr_u32(c);
    cpu->tlb_last_index = cr_u32(c);
    cpu->tlb_invalidate_count = cr_u32(c);
    cpu->external_addr = cr_u32(c);
    cpu->external_value = cr_u32(c);
    cpu->external_rid = cr_u8(c);
    cpu->external_read_count = cr_u8(c);
    cpu->external_write_count = cr_u8(c);
    cpu->reserve_addr = cr_u32(c);
    cpu->reserve_valid = cr_bool(c);
    cr_bytes(c, cpu->locked_cache_tag, sizeof cpu->locked_cache_tag);
    for (u32 i = 0; i < 512; i++) cpu->locked_cache_valid[i] = (bool)cr_bool(c);
    cpu->cycles = cr_u64(c);
    cpu->cycle_deadline = cr_u64(c);
    cpu->halted = (bool)cr_bool(c);
    cpu->halt_reason = (int)cr_u32(c);
    /* ram/ram_size/mem2/mem2_size/rom_window/rom_window_size/the six host
     * bindings: never read here (write_cpu_fixed never wrote them either) —
     * already correct from cpu_init + boot.c's device construction. */
}

static void read_dispatch(SnapCur* c) {
    u64 device_cycles = cr_u64(c), prev_cycles = cr_u64(c);
    u64 tb_remainder = cr_u64(c), dsp_remainder = cr_u64(c);
    gcn_dispatch_timing_set(device_cycles, prev_cycles, tb_remainder, dsp_remainder);
}

static void read_exi(SnapCur* c, GcnExi* exi) {
    GcnExiChannel tmp[GCN_EXI_CHANNELS];
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) {
        tmp[i].csr = cr_u32(c);
        tmp[i].mar = cr_u32(c);
        tmp[i].len = cr_u32(c);
        tmp[i].cr = cr_u32(c);
        tmp[i].data = cr_u32(c);
    }
    u8 sram[GCN_SRAM_SIZE_BYTES]; cr_bytes(c, sram, sizeof sram);
    u32 rtc_counter = cr_u32(c);
    u64 rtc_anchor = cr_u64(c);
    u32 rtc_running = cr_u32(c);
    u32 op[GCN_EXI_CHANNELS], rom_off[GCN_EXI_CHANNELS], dev_pos[GCN_EXI_CHANNELS];
    u8 prev_cs[GCN_EXI_CHANNELS]; u32 have_cmd[GCN_EXI_CHANNELS]; u8 dev_present[GCN_EXI_CHANNELS];
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) op[i] = cr_u32(c);
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) rom_off[i] = cr_u32(c);
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) dev_pos[i] = cr_u32(c);
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) prev_cs[i] = cr_u8(c);
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) have_cmd[i] = cr_u32(c);
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) dev_present[i] = cr_u8(c);
    u32 irq_level = cr_u32(c);

    if (!exi) return;
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) exi->channels[i] = tmp[i];
    memcpy(exi->sram, sram, sizeof sram);
    /* Direct field assignment ONLY — never gcn_rtc_sample_host_local (hazard
     * #7): that call samples the HOST's current wall clock, which would
     * silently replace the captured guest time with "now" on every restore. */
    exi->rtc.counter = rtc_counter;
    exi->rtc.anchor_cycles = rtc_anchor;
    exi->rtc.running = (int)rtc_running;
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) exi->op[i] = (int)op[i];
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) exi->rom_offset[i] = rom_off[i];
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) exi->dev_pos[i] = dev_pos[i];
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) exi->prev_cs[i] = prev_cs[i];
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) exi->have_cmd[i] = (int)have_cmd[i];
    for (u32 i = 0; i < GCN_EXI_CHANNELS; i++) exi->dev_present[i] = dev_present[i];
    exi->irq_level = (int)irq_level;
    /* rom/rom_size/rom_base/owned_rom (hazard: content-addressed, already
     * reconstructed by boot.c's normal GCN_BOOT_BS1 rom-window setup, which
     * runs unconditionally as part of device construction), card[3]
     * (borrowed pointers — already correct; card CONTENTS are restored
     * separately by read_memcard), irq/irq_user/persist/persist_user/
     * card_persist/card_persist_user (host callbacks, already correctly
     * wired): none of these are touched here. */
}

/* Loads a memory card's captured image + transaction state into `mc` (must
 * already exist — boot.c's normal setup_memcard already installed a default
 * formatted card there). If the snapshot's slot was empty, `mc` is left as
 * boot.c's default construction set it up (a documented pass-B limitation:
 * a captured "truly absent" slot cannot currently be reproduced when
 * boot.c's own default-on slot-A construction always installs a card —
 * see docs/SNAPSHOT_RESUME.md's memcard survey note). */
static void read_memcard(SnapCur* c, GcnMemcard* mc) {
    int present = cr_bool(c);
    u32 size_bytes = cr_u32(c);
    if (!present || !mc) return;

    u16 size_mbits = cr_u16(c);
    u16 card_id = cr_u16(c);
    u32 command = cr_u32(c);
    u32 position = cr_u32(c);
    u32 address = cr_u32(c);
    u32 status = cr_u32(c);
    u8 prog_buf[GCN_MC_PAGE_BYTES]; cr_bytes(c, prog_buf, sizeof prog_buf);
    u32 interrupt_switch = cr_u32(c);
    u32 interrupt_set = cr_u32(c);
    u32 dirty = cr_u32(c);

    u8* buf = (u8*)malloc(size_bytes ? size_bytes : 1u);
    if (buf) cr_bytes(c, buf, size_bytes);
    else { cr_bytes(c, NULL, size_bytes); return; } /* skip payload, leave mc untouched */

    gcn_memcard_free(mc);              /* release whatever setup_memcard installed */
    gcn_memcard_init(mc, buf, size_bytes);  /* takes ownership of buf */
    mc->size_mbits = size_mbits;
    mc->card_id = card_id;
    mc->command = (int)command;
    mc->position = position;
    mc->address = address;
    mc->status = (int)status;
    memcpy(mc->prog_buf, prog_buf, sizeof prog_buf);
    mc->interrupt_switch = (int)interrupt_switch;
    mc->interrupt_set = (int)interrupt_set;
    mc->dirty = (int)dirty;
}

static void read_vi(SnapCur* c, GcnVi* vi) {
    u16 reg[GCN_VI_SIZE / 2]; cr_bytes(c, reg, sizeof reg);
    u32 half_line_count = cr_u32(c);
    u64 now_cycles = cr_u64(c), last_cycles = cr_u64(c), ticks_last_line_start = cr_u64(c);
    u32 irq_level = cr_u32(c);
    if (!vi) return;
    memcpy(vi->reg, reg, sizeof reg);
    vi->half_line_count = half_line_count;
    vi->now_cycles = now_cycles;
    vi->last_cycles = last_cycles;
    vi->ticks_last_line_start = ticks_last_line_start;
    vi->irq_level = (int)irq_level;
    /* tphl_cache/tphl_key deliberately left as gcn_vi_init set them
     * (0xFFFFFFFF key forces recompute) — pure memo of reg[], RESET not SAVE. */
}

static void read_si(SnapCur* c, GcnSi* si) {
    GcnSiChannel ch[GCN_SI_CHANNELS];
    for (u32 i = 0; i < GCN_SI_CHANNELS; i++) {
        ch[i].out = cr_u32(c);
        ch[i].in_hi = cr_u32(c);
        ch[i].in_lo = cr_u32(c);
        ch[i].connected = cr_u8(c);
        ch[i].rdst = cr_u8(c);
        ch[i].norep = cr_u8(c);
        ch[i].mode = cr_u8(c);
    }
    u32 poll = cr_u32(c), comcsr = cr_u32(c), sisr = cr_u32(c), exilk = cr_u32(c);
    u8 iobuf[128]; cr_bytes(c, iobuf, sizeof iobuf);
    u32 irq_level = cr_u32(c);
    u32 next_poll_halfline = cr_u32(c);
    if (!si) return;
    for (u32 i = 0; i < GCN_SI_CHANNELS; i++) si->ch[i] = ch[i];
    si->poll = poll; si->comcsr = comcsr; si->sisr = sisr; si->exilk = exilk;
    memcpy(si->iobuf, iobuf, sizeof iobuf);
    si->irq_level = (int)irq_level;
    si->next_poll_halfline = next_poll_halfline;
    /* si->input intentionally untouched — GCN_SNAPSHOT_FLAG_SI_INPUT_RESET_
     * NEUTRAL: stays at gcn_si_init's power-on-neutral report. */
}

static void read_pi(SnapCur* c, GcnPi* pi) {
    u32 reg[GCN_PI_SIZE / 4]; cr_bytes(c, reg, sizeof reg);
    u32 intsr = cr_u32(c);
    if (!pi) return;
    memcpy(pi->reg, reg, sizeof reg);
    __atomic_store_n(&pi->intsr, intsr, __ATOMIC_RELEASE);
}

static void read_mi(SnapCur* c, GcnMi* mi) {
    u16 reg[GCN_MI_SIZE / 2]; cr_bytes(c, reg, sizeof reg);
    if (!mi) return;
    memcpy(mi->reg, reg, sizeof reg);
}

static void read_ai(SnapCur* c, GcnAi* ai) {
    u32 control = cr_u32(c), volume = cr_u32(c), sample_counter = cr_u32(c), int_timing = cr_u32(c);
    u32 tick_accum = cr_u32(c);
    u64 cycle_accum_x2 = cr_u64(c);
    u32 irq_level = cr_u32(c);
    if (!ai) return;
    ai->control = control; ai->volume = volume;
    ai->sample_counter = sample_counter; ai->int_timing = int_timing;
    ai->tick_accum = tick_accum; ai->cycle_accum_x2 = cycle_accum_x2;
    ai->irq_level = (int)irq_level;
}

/* Parses the DI section fully; applies everything EXCEPT disc_size/
 * disc_present to `di` (those two must reflect whatever disc boot.c's
 * normal GCN_DISC construction actually just mounted via a real fopen —
 * hazard #3: disc_file is never touched here, so its paired disc_size/
 * disc_present bookkeeping must come from that real re-mount, not the
 * blob). `out_disc_size` (may be NULL) always receives the CAPTURED value,
 * for the identity-gate check in gcn_snapshot_load. */
static void read_di(SnapCur* c, GcnDi* di, u64* out_disc_size) {
    u32 disr = cr_u32(c), dicvr = cr_u32(c);
    u32 cmdbuf[3]; cr_bytes(c, cmdbuf, sizeof cmdbuf);
    u32 dimar = cr_u32(c), dilength = cr_u32(c), dicr = cr_u32(c), diimmbuf = cr_u32(c);
    u8 drive_state = cr_u8(c);
    u32 error_code = cr_u32(c);
    u8 enable_dtk = cr_u8(c), cmd_pending = cr_u8(c), pending_intr = cr_u8(c);
    u32 irq_level = cr_u32(c);
    u64 disc_size = cr_u64(c);
    u32 disc_present = cr_u32(c); (void)disc_present;
    if (out_disc_size) *out_disc_size = disc_size;
    if (!di) return;
    di->disr = disr; di->dicvr = dicvr;
    memcpy(di->cmdbuf, cmdbuf, sizeof cmdbuf);
    di->dimar = dimar; di->dilength = dilength; di->dicr = dicr; di->diimmbuf = diimmbuf;
    di->drive_state = drive_state;
    di->error_code = error_code;
    di->enable_dtk = enable_dtk;
    di->cmd_pending = cmd_pending;
    di->pending_intr = pending_intr;
    di->irq_level = (int)irq_level;
    /* disc_file/disc_size/disc_present: never touched — see doc comment. */
}

static void read_gx_regs(SnapCur* c, GcnCp* cp, GcnPe* pe) {
    u32 bp[256]; cr_bytes(c, bp, sizeof bp);
    u32 xf_words = cr_u32(c);
    /* gcn_gx_set_xf itself clamps to the compiled-in GX_XF_MEM_WORDS (private
     * to gx.c); a heap buffer avoids needing that constant exposed here just
     * to size a fixed array. */
    u32* xf = (u32*)calloc(xf_words ? xf_words : 1u, sizeof(u32));
    if (xf) {
        cr_bytes(c, xf, (size_t)xf_words * sizeof(u32));
        gcn_gx_set_bp(bp);
        gcn_gx_set_xf(xf, xf_words);
        free(xf);
    } else {
        cr_bytes(c, NULL, (size_t)xf_words * sizeof(u32));
        gcn_gx_set_bp(bp);
    }

    u16 cp_reg[GCN_CP_SIZE / 2]; cr_bytes(c, cp_reg, sizeof cp_reg);
    u32 gp_read_enable = cr_u32(c), bp_enable = cr_u32(c);
    u32 hi_wm_int = cr_u32(c), lo_wm_int = cr_u32(c);
    u32 gp_link_enable = cr_u32(c), bp_int = cr_u32(c);
    u32 hi_wm = cr_u32(c), lo_wm = cr_u32(c), breakpoint = cr_u32(c);
    u32 cp_irq_level = cr_u32(c);
    if (cp) {
        memcpy(cp->reg, cp_reg, sizeof cp_reg);
        cp->gp_read_enable = (int)gp_read_enable;
        cp->bp_enable = (int)bp_enable;
        cp->hi_wm_int = (int)hi_wm_int;
        cp->lo_wm_int = (int)lo_wm_int;
        cp->gp_link_enable = (int)gp_link_enable;
        cp->bp_int = (int)bp_int;
        cp->hi_wm = (int)hi_wm;
        cp->lo_wm = (int)lo_wm;
        cp->breakpoint = (int)breakpoint;
        cp->irq_level = (int)cp_irq_level;
    }

    u16 pe_reg[GCN_PE_SIZE / 2]; cr_bytes(c, pe_reg, sizeof pe_reg);
    u16 token = cr_u16(c);
    u32 token_enable = cr_u32(c), finish_enable = cr_u32(c);
    u32 signal_token = cr_u32(c), signal_finish = cr_u32(c);
    u32 token_level = cr_u32(c), finish_level = cr_u32(c);
    if (pe) {
        memcpy(pe->reg, pe_reg, sizeof pe_reg);
        pe->token = token;
        pe->token_enable = (int)token_enable;
        pe->finish_enable = (int)finish_enable;
        pe->signal_token_interrupt = (int)signal_token;
        pe->signal_finish_interrupt = (int)signal_finish;
        pe->token_level = (int)token_level;
        pe->finish_level = (int)finish_level;
    }
}

int gcn_snapshot_load(const char* path, CPUState* cpu, const GcnDebugCtx* ctx,
                      char* why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (!path || !cpu || !ctx) {
        if (why) snprintf(why, why_size, "null path/cpu/ctx");
        return GCN_SNAPSHOT_ERR_IO;
    }

    FILE* f = fopen(path, "rb");
    if (!f) {
        if (why) snprintf(why, why_size, "fopen('%s', \"rb\") failed", path);
        return GCN_SNAPSHOT_ERR_IO;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); if (why) snprintf(why, why_size, "fseek failed"); return GCN_SNAPSHOT_ERR_IO; }
    long file_len_l = ftell(f);
    if (file_len_l < 0) { fclose(f); if (why) snprintf(why, why_size, "ftell failed"); return GCN_SNAPSHOT_ERR_IO; }
    rewind(f);
    size_t file_len = (size_t)file_len_l;
    u8* blob = (u8*)malloc(file_len ? file_len : 1u);
    if (!blob) { fclose(f); if (why) snprintf(why, why_size, "out of memory reading '%s' (%zu bytes)", path, file_len); return GCN_SNAPSHOT_ERR_ALLOC; }
    size_t got = fread(blob, 1, file_len, f);
    fclose(f);
    if (got != file_len) {
        free(blob);
        if (why) snprintf(why, why_size, "short read on '%s' (%zu of %zu bytes)", path, got, file_len);
        return GCN_SNAPSHOT_ERR_IO;
    }

    /* ---- structural gate: magic/version/footer CRC32 (never bypassable —
     * a bad magic/version/CRC means the blob itself is suspect, and
     * GCN_SNAPSHOT_FORCE cannot fix a corrupt file). ---- */
    if (file_len < 8 + GCN_SNAPSHOT_COMMIT_LEN + 12 + 4) {
        free(blob);
        if (why) snprintf(why, why_size, "file too small to be a snapshot (%zu bytes)", file_len);
        return GCN_SNAPSHOT_ERR_FORMAT;
    }
    SnapCur hc; cr_init(&hc, blob, file_len);
    u32 magic = cr_u32(&hc);
    u32 version = cr_u32(&hc);
    char commit[GCN_SNAPSHOT_COMMIT_LEN]; cr_bytes(&hc, commit, sizeof commit);
    u32 content_crc = cr_u32(&hc);
    u32 flags = cr_u32(&hc);
    u32 nsec = cr_u32(&hc);
    if (magic != GCN_SNAPSHOT_MAGIC || version != GCN_SNAPSHOT_FORMAT_VERSION) {
        free(blob);
        if (why) snprintf(why, why_size,
            "bad magic/version (magic=0x%08X version=%u, expected 0x%08X/%u)",
            magic, version, GCN_SNAPSHOT_MAGIC, GCN_SNAPSHOT_FORMAT_VERSION);
        return GCN_SNAPSHOT_ERR_FORMAT;
    }
    if (file_len < 4) { free(blob); if (why) snprintf(why, why_size, "truncated footer"); return GCN_SNAPSHOT_ERR_FORMAT; }
    size_t footer_off = file_len - 4;
    u32 stored_footer_crc; memcpy(&stored_footer_crc, blob + footer_off, 4);
    u32 computed_footer_crc = gcn_crc32(blob, (u32)footer_off);
    if (stored_footer_crc != computed_footer_crc) {
        free(blob);
        if (why) snprintf(why, why_size, "footer CRC32 mismatch (stored=0x%08X computed=0x%08X)",
            stored_footer_crc, computed_footer_crc);
        return GCN_SNAPSHOT_ERR_FORMAT;
    }

    /* TOC. */
    if (nsec > GCN_SNAP_SEC_COUNT_MAX) {
        free(blob);
        if (why) snprintf(why, why_size, "implausible section_count=%u", nsec);
        return GCN_SNAPSHOT_ERR_FORMAT;
    }
    u32 toc_tag[GCN_SNAP_SEC_COUNT_MAX];
    u64 toc_off[GCN_SNAP_SEC_COUNT_MAX];
    u64 toc_len[GCN_SNAP_SEC_COUNT_MAX];
    for (u32 i = 0; i < nsec; i++) {
        toc_tag[i] = cr_u32(&hc);
        toc_off[i] = cr_u64(&hc);
        toc_len[i] = cr_u64(&hc);
    }
    if (!hc.ok) {
        free(blob);
        if (why) snprintf(why, why_size, "truncated TOC");
        return GCN_SNAPSHOT_ERR_FORMAT;
    }
    /* Section payloads are stored as {u32 tag; u64 length; payload} right at
     * each TOC offset; the TOC's own offset already points at that tag word
     * (see snapshot.h's format doc), so the payload itself starts +12 later. */
    #define FIND_SEC(TAG, OUTP, OUTLEN) do { \
        (OUTP) = NULL; (OUTLEN) = 0; \
        for (u32 _i = 0; _i < nsec; _i++) { \
            if (toc_tag[_i] == (TAG)) { \
                u64 _o = toc_off[_i] + 12u; \
                u64 _l = toc_len[_i]; \
                if (_o <= file_len && _o + _l <= file_len) { (OUTP) = blob + _o; (OUTLEN) = (size_t)_l; } \
                break; \
            } \
        } \
    } while (0)

    const u8 *p_cpu=NULL,*p_disp=NULL,*p_mem1=NULL,*p_l1=NULL,*p_mem2=NULL,*p_exi=NULL,
             *p_mca=NULL,*p_mcb=NULL,*p_vi=NULL,*p_si=NULL,*p_pi=NULL,*p_mi=NULL,
             *p_ai=NULL,*p_di=NULL,*p_aram=NULL,*p_dspcore=NULL,*p_dspdev=NULL,
             *p_gx=NULL,*p_tmem=NULL,*p_efb=NULL,*p_xfbhash=NULL;
    size_t l_cpu=0,l_disp=0,l_mem1=0,l_l1=0,l_mem2=0,l_exi=0,l_mca=0,l_mcb=0,l_vi=0,
           l_si=0,l_pi=0,l_mi=0,l_ai=0,l_di=0,l_aram=0,l_dspcore=0,l_dspdev=0,l_gx=0,
           l_tmem=0,l_efb=0,l_xfbhash=0;
    FIND_SEC(GCN_SNAP_SEC_CPU_FIXED, p_cpu, l_cpu);
    FIND_SEC(GCN_SNAP_SEC_DISPATCH, p_disp, l_disp);
    FIND_SEC(GCN_SNAP_SEC_MEM1, p_mem1, l_mem1);
    FIND_SEC(GCN_SNAP_SEC_LOCKED_L1, p_l1, l_l1);
    FIND_SEC(GCN_SNAP_SEC_MEM2, p_mem2, l_mem2);
    FIND_SEC(GCN_SNAP_SEC_EXI, p_exi, l_exi);
    FIND_SEC(GCN_SNAP_SEC_MEMCARD_A, p_mca, l_mca);
    FIND_SEC(GCN_SNAP_SEC_MEMCARD_B, p_mcb, l_mcb);
    FIND_SEC(GCN_SNAP_SEC_VI, p_vi, l_vi);
    FIND_SEC(GCN_SNAP_SEC_SI, p_si, l_si);
    FIND_SEC(GCN_SNAP_SEC_PI, p_pi, l_pi);
    FIND_SEC(GCN_SNAP_SEC_MI, p_mi, l_mi);
    FIND_SEC(GCN_SNAP_SEC_AI, p_ai, l_ai);
    FIND_SEC(GCN_SNAP_SEC_DI, p_di, l_di);
    FIND_SEC(GCN_SNAP_SEC_ARAM, p_aram, l_aram);
    FIND_SEC(GCN_SNAP_SEC_DSP_CORE, p_dspcore, l_dspcore);
    FIND_SEC(GCN_SNAP_SEC_DSP_DEVICE, p_dspdev, l_dspdev);
    FIND_SEC(GCN_SNAP_SEC_GX_REGS, p_gx, l_gx);
    FIND_SEC(GCN_SNAP_SEC_TMEM, p_tmem, l_tmem);
    FIND_SEC(GCN_SNAP_SEC_EFB, p_efb, l_efb);
    FIND_SEC(GCN_SNAP_SEC_XFB_HASH, p_xfbhash, l_xfbhash);  /* optional — v1 pass-A/B blobs lack it */
    #undef FIND_SEC

    /* `ctx` is the caller's parameter (boot.c's own GcnDebugCtx), NOT looked
     * up via gcn_debug_server_ctx() — that registry is only populated when
     * GCN_DEBUG_PORT is set, and restore must work without it. */

    /* ---- identity gate: disc-size (provisional — see snapshot.h doc). ---- */
    if (p_di) {
        SnapCur dc; cr_init(&dc, p_di, l_di);
        u64 captured_disc_size = 0;
        read_di(&dc, NULL, &captured_disc_size);
        u64 live_disc_size = ctx->di ? ctx->di->disc_size : 0;
        if (captured_disc_size != live_disc_size) {
            const char* force = getenv("GCN_SNAPSHOT_FORCE");
            int forced = force && *force && *force != '0';
            fprintf(stderr,
                "gcn snapshot: disc-size identity mismatch (captured=%llu live=%llu)%s\n",
                (unsigned long long)captured_disc_size, (unsigned long long)live_disc_size,
                forced ? " — GCN_SNAPSHOT_FORCE=1 set, proceeding (iteration-tier only)" : "");
            if (!forced) {
                free(blob);
                if (why) snprintf(why, why_size,
                    "disc-size identity mismatch (captured=%llu live=%llu); "
                    "set GCN_SNAPSHOT_FORCE=1 to override (iteration tier only)",
                    (unsigned long long)captured_disc_size, (unsigned long long)live_disc_size);
                return GCN_SNAPSHOT_ERR_IDENTITY;
            }
        }
    }
    (void)content_crc; (void)commit; (void)flags;

    /* ---- overlay: CPUState, dispatch timing, MEM1/L1/MEM2 bytes. ---- */
    if (p_cpu) { SnapCur c; cr_init(&c, p_cpu, l_cpu); read_cpu_fixed(&c, cpu); }
    if (p_disp) { SnapCur c; cr_init(&c, p_disp, l_disp); read_dispatch(&c); }
    if (p_mem1) { SnapCur c; cr_init(&c, p_mem1, l_mem1); cr_bytes(&c, cpu->ram, cpu->ram_size); }
    if (p_l1) {
        SnapCur c; cr_init(&c, p_l1, l_l1);
        u32 l1_size = cr_u32(&c);
        u32 have_size = 0;
        u8* l1 = gcn_mem_locked_l1(cpu, &have_size);
        if (l1 && have_size) cr_bytes(&c, l1, (l1_size < have_size) ? l1_size : have_size);
    }
    if (p_mem2) {
        SnapCur c; cr_init(&c, p_mem2, l_mem2);
        u32 mem2_size = cr_u32(&c);
        if (cpu->mem2 && mem2_size && mem2_size <= cpu->mem2_size)
            cr_bytes(&c, cpu->mem2, mem2_size);
    }

    /* ---- overlay: devices. ---- */
    if (p_exi) { SnapCur c; cr_init(&c, p_exi, l_exi); read_exi(&c, ctx->exi); }
    if (p_mca && ctx->exi) { SnapCur c; cr_init(&c, p_mca, l_mca); read_memcard(&c, ctx->exi->card[0]); }
    if (p_mcb && ctx->exi) { SnapCur c; cr_init(&c, p_mcb, l_mcb); read_memcard(&c, ctx->exi->card[1]); }
    if (p_vi) { SnapCur c; cr_init(&c, p_vi, l_vi); read_vi(&c, ctx->vi); }
    if (p_si) { SnapCur c; cr_init(&c, p_si, l_si); read_si(&c, ctx->si); }
    if (p_pi) { SnapCur c; cr_init(&c, p_pi, l_pi); read_pi(&c, ctx->pi); }
    if (p_mi) { SnapCur c; cr_init(&c, p_mi, l_mi); read_mi(&c, ctx->mi); }
    if (p_ai) { SnapCur c; cr_init(&c, p_ai, l_ai); read_ai(&c, ctx->ai); }
    if (p_di) { SnapCur c; cr_init(&c, p_di, l_di); read_di(&c, ctx->di, NULL); }

    /* ---- overlay: ARAM + DSP core + DSP device. ---- */
    if (p_aram) {
        SnapCur c; cr_init(&c, p_aram, l_aram);
        u32 aram_size = cr_u32(&c);
        u8* aram = dsp_lle_aram();
        u32 have = dsp_lle_aram_size();
        if (aram && have) cr_bytes(&c, aram, (aram_size < have) ? aram_size : have);
    }
    if (p_dspcore && l_dspcore >= sizeof(GcnDspLleSnapshot)) {
        GcnDspLleSnapshot dsp_core;
        memcpy(&dsp_core, p_dspcore, sizeof dsp_core);
        dsp_lle_load_state(&dsp_core);
    }
    if (p_dspdev && ctx->dsp) {
        SnapCur c; cr_init(&c, p_dspdev, l_dspdev);
        GcnDsp* dsp = ctx->dsp;
        u16 reg[GCN_DSP_SIZE / 2]; cr_bytes(&c, reg, sizeof reg);
        u16 csr = cr_u16(&c);
        int dma_active = cr_bool(&c);
        u32 dma_cycles_left = cr_u32(&c);
        u32 aid_source = cr_u32(&c);
        u16 aid_ctrl = cr_u16(&c);
        u32 aid_cur_addr = cr_u32(&c);
        u16 aid_blocks_left = cr_u16(&c);
        u8 aid_int_pending = cr_u8(&c);
        u32 aid_accum = cr_u32(&c);
        u32 irq_level = cr_u32(&c);
        memcpy(dsp->reg, reg, sizeof reg);
        dsp->csr = csr;
        dsp->dma_active = dma_active;
        dsp->dma_cycles_left = dma_cycles_left;
        dsp->aid_source = aid_source;
        dsp->aid_ctrl = aid_ctrl;
        dsp->aid_cur_addr = aid_cur_addr;
        dsp->aid_blocks_left = aid_blocks_left;
        dsp->aid_int_pending = aid_int_pending;
        dsp->aid_accum = aid_accum;
        dsp->irq_level = (int)irq_level;
    }

    /* ---- overlay: GX regs, TMEM, EFB planes. ---- */
    if (p_gx) { SnapCur c; cr_init(&c, p_gx, l_gx); read_gx_regs(&c, ctx->cp, ctx->pe); }
    if (p_tmem) {
        gcn_gx_set_tmem(p_tmem, (u32)l_tmem);
    }
    if (p_efb) {
        SnapCur c; cr_init(&c, p_efb, l_efb);
        u32 w = cr_u32(&c), h = cr_u32(&c);
        u32* color = NULL; u32* depth = NULL; u32 live_w = 0, live_h = 0;
        gx_raster_efb_data_mutable(&color, &depth, &live_w, &live_h);
        if (w == live_w && h == live_h) {
            /* Common case (EFB_WIDTH/HEIGHT are fixed compile-time constants,
             * unchanged across builds): a straight full-plane copy, color
             * then depth, matching write_gx_regs' capture order exactly. */
            if (color) cr_bytes(&c, color, (size_t)w * h * sizeof(u32));
            if (depth) cr_bytes(&c, depth, (size_t)w * h * sizeof(u32));
        } else {
            fprintf(stderr,
                "gcn snapshot: EFB dimension mismatch (captured %ux%u, live %ux%u) "
                "— skipping EFB restore\n", w, h, live_w, live_h);
        }
    }

    /* SNAPSHOT_RESUME pass C: seed the cumulative XFB hash chain/pub-count/
     * frame-count, but ONLY when GCN_GX_XFB_HASH=1 in THIS (resuming)
     * process too — seeding a chain nothing will ever feed into again is
     * pointless, and gx.c's own gx_xfb_hash_on() gate already makes
     * feed/publish_done no-ops when unset, so an unconditional seed would
     * silently do nothing useful anyway. Absent section (older pass-A/B
     * blobs) is not an error — just nothing to seed. */
    if (p_xfbhash && getenv("GCN_GX_XFB_HASH") && getenv("GCN_GX_XFB_HASH")[0] == '1') {
        SnapCur c; cr_init(&c, p_xfbhash, l_xfbhash);
        u64 chain = cr_u64(&c);
        u64 pubs = cr_u64(&c);
        u64 frames = cr_u64(&c);
        gcn_gx_xfb_hash_set_state(chain, pubs);
        gcn_gx_set_frame_count(frames);
        fprintf(stdout,
            "gcn snapshot: seeded XFB hash chain=%016llx pubs=%llu frames=%llu\n",
            (unsigned long long)chain, (unsigned long long)pubs,
            (unsigned long long)frames);
    }

    free(blob);

    /* ---- RESET-side host-memo fixups a restore needs (never a fresh boot,
     * which starts every one of these already-clean). ---- */
    /* Hazard #2: gcn_native_code_reset() would mark stale MEM1 pages
     * "valid" again (it's a blind clear, monotonic-until-reset by design) —
     * unsound here since the just-loaded RAM is NOT what any native chunk
     * was compiled against at a byte level until re-verified. Invalidate
     * the whole range instead: falls through to the interpreter/AOT
     * content-check path exactly like a real icbi would, and the
     * content-stale bitmap this sets doubles as the interpreter's
     * page-CRC-memo funnel (native_code.c's own asymmetric reset
     * comment: "All-stale, not all-clean"). */
    gcn_native_code_invalidate(GC_RAM_BASE, GC_MAIN_RAM_SIZE);
    /* AOT title-module chunk verification states: same monotonic-until-
     * invalidated shape as the bitmap above (aot_module.c's states[] never
     * un-verifies itself). gcn_title_module_icbi is the existing exposed
     * entry point (title_module.c.in) that calls gcn_aot_module_invalidate
     * on the live module singleton. */
    gcn_title_module_icbi(GC_RAM_BASE, GC_MAIN_RAM_SIZE);
    /* Rings: pure observability, always safe to fully clear. */
    gcn_rings_reset();
    /* GX draw-config-cache/census/texel-cache host memos: gx_raster_init
     * already ran once during normal device construction (zeroing the
     * config cache and the EFB planes); this covers the two gaps it
     * doesn't (hazard #12), without re-touching EFB (already overlaid
     * above with the real captured content). */
    gx_raster_restore_reset();
    /* gx_vulkan.c's entire GxVk struct (RESET, hazard confirmed in the
     * survey): nothing to do here — it is rebuilt from the just-restored
     * GX regs (BP/XF/CP/PE) via its own normal init path, which boot.c's
     * device construction already ran (unconditionally, before this
     * function was ever called) and which reads live BP/XF/TMEM lazily on
     * first use, not at init time. */

    return GCN_SNAPSHOT_OK;
}
