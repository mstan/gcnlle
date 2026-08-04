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
    GCN_SNAP_SEC_COUNT_MAX   = 21   /* array sizing only; not written */
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
