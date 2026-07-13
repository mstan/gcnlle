/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Audio Interface (AI) model. See include/ai/ai.h for scope and the Dolphin
 * source the register semantics are transcribed from.
 */
#include "ai/ai.h"
#include "debug/rings.h"

#include <string.h>

/* The dispatch loop ticks one AI (like the one VI/DSP); registered at init. */
static GcnAi* s_ai = NULL;

void gcn_ai_set_irq(GcnAi* ai, GcnAiIrqFn fn, void* user) {
    ai->irq = fn;
    ai->irq_user = user;
}

void gcn_ai_init(GcnAi* ai) {
    memset(ai, 0, sizeof *ai);
    ai->control = GCN_AI_CR_RESET_VALUE;   /* AIS=48kHz | AID=32kHz (0x42) */
    s_ai = ai;
}

/* ---- interrupt line (AudioInterface.cpp UpdateInterrupts:96-100) ----
 * INT_CAUSE_AI = AICR.AIINT & AICR.AIINTMSK. Pushed to PI on EVERY evaluation
 * (PI INTSR is W1C, so a still-asserted level must re-appear). AIINT is set by
 * gcn_ai_tick's sample-counter/AIIT compare (GenerateAudioInterrupt) and
 * cleared by a guest W1C write (write_control). */
static void ai_update_interrupts(GcnAi* ai) {
    int level = (ai->control & GCN_AI_CR_AIINT) && (ai->control & GCN_AI_CR_AIINTMSK);
    if (ai->irq)
        ai->irq(ai->irq_user, level);
    if (level != ai->irq_level) {
        gcn_ring_event(level ? GCN_EV_IRQ_RAISE : GCN_EV_IRQ_CLEAR,
                       /*source*/ 5u /* PI cause bit index of INT_CAUSE_AI=0x20 */,
                       0u, 0u);
        ai->irq_level = level;
    }
}

static void write_control(GcnAi* ai, u32 val) {
    /* R/W control bits copied straight through */
    const u32 CTRL = GCN_AI_CR_PSTAT | GCN_AI_CR_AISFR | GCN_AI_CR_AIINTMSK |
                     GCN_AI_CR_AIINTVLD | GCN_AI_CR_AIDFR;
    u32 nv = (ai->control & ~CTRL) | (val & CTRL);
    if (val & GCN_AI_CR_AIINT)    nv &= ~GCN_AI_CR_AIINT;   /* W1C interrupt   */
    nv &= ~GCN_AI_CR_SCRESET;                               /* trigger, reads 0 */
    /* PSTAT edge rebases Dolphin's timing anchor (RegisterMMIO AICR
     * ComplexWrite:250-259: "ai.m_last_cpu_time = core_timing.GetTicks()" on
     * ANY PSTAT value change, start or stop) — our equivalent is zeroing the
     * tick accumulator so a stale partial sample period never carries across
     * a play/stop transition. Both accumulators are kept zeroed together
     * regardless of which pacing model (legacy calls-counted / derived
     * cycle-accurate) the current run is actually exercising — the idle one
     * costs nothing and this keeps a mode never seeing the other's residue. */
    if ((val & GCN_AI_CR_PSTAT) != (ai->control & GCN_AI_CR_PSTAT)) {
        ai->tick_accum = 0;
        ai->cycle_accum_x2 = 0;
    }
    if (val & GCN_AI_CR_SCRESET) {
        ai->sample_counter = 0;   /* RegisterMMIO :268-275 */
        ai->tick_accum = 0;
        ai->cycle_accum_x2 = 0;
    }
    ai->control = nv;
    /* AIINT W1C ack and AIINTMSK change both move the line (AudioInterface.cpp
     * AICR write handler ends in UpdateInterrupts:277). */
    ai_update_interrupts(ai);
}

u32 gcn_ai_read(void* user, CPUState* cpu, u32 addr, u8 size) {
    (void)cpu; (void)size;
    GcnAi* ai = (GcnAi*)user;
    switch (addr - GCN_AI_BASE) {
    case GCN_AI_CONTROL:   return ai->control;
    case GCN_AI_VOLUME:    return ai->volume;
    case GCN_AI_SAMPLECNT: return ai->sample_counter;  /* live while PSTAT=1, gcn_ai_tick */
    case GCN_AI_INTTIMING: return ai->int_timing;
    default:               return 0;
    }
}

void gcn_ai_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size) {
    (void)cpu; (void)size;
    GcnAi* ai = (GcnAi*)user;
    switch (addr - GCN_AI_BASE) {
    case GCN_AI_CONTROL:   write_control(ai, value); break;
    case GCN_AI_VOLUME:    ai->volume = value; break;
    case GCN_AI_SAMPLECNT:
        /* Rebase the timing anchor on a direct counter write, mirroring
         * RegisterMMIO AI_SAMPLE_COUNTER ComplexWrite:297-304
         * ("ai.m_last_cpu_time = core_timing.GetTicks()"). */
        ai->sample_counter = value;
        ai->tick_accum = 0;
        ai->cycle_accum_x2 = 0;
        break;
    case GCN_AI_INTTIMING: ai->int_timing = value; break;
    default: break;
    }
}

/* ---- sample counter / AIINT pacing (AudioInterface.cpp IncreaseSampleCount /
 * GenerateAudioInterrupt:102-123) ----
 * Shared by both pacing models below: advances AISCNT by exactly one sample
 * and re-runs Dolphin's wraparound-safe compare:
 *   old = sample_counter(before) + 1
 *   new = sample_counter(before) + amount     (amount == 1 here)
 *   fire if (int_timing - old) <= (new - old)
 * With amount == 1, (new - old) == 0, so this reduces to firing exactly when
 * int_timing == sample_counter(after) — i.e. AIINT latches the tick AISCNT
 * reaches AIIT, and (per ai.h) unconditionally of AIINTVLD, exactly as
 * Dolphin's IncreaseSampleCount does. */
static void ai_advance_sample(GcnAi* ai) {
    u32 old = ai->sample_counter + 1;          /* IncreaseSampleCount:113 */
    ai->sample_counter += 1;                   /* amount = 1 sample/tick  */
    if ((ai->int_timing - old) <= (ai->sample_counter - old)) {
        ai->control |= GCN_AI_CR_AIINT;        /* GenerateAudioInterrupt():102-106 */
        ai_update_interrupts(ai);
    }
}

/* LEGACY calls-counted pacing (ai.h derivation comment). Called once per
 * executed block from dispatch.c's fallback path (after gcn_dsp_tick), same
 * singleton-tick pattern as gcn_vi_tick/gcn_dsp_tick. No-op while PSTAT=0
 * (IsPlaying() gate). While playing, accumulates ONE CALL (not cycles) until
 * ticks_per_sample calls elapse, then fires one sample — byte-for-bit the
 * pre-derived-cycle-accuracy body, unmodified, so GCN_CYCLES_UNIFORM=1
 * reproduces the pre-existing golden XFB hashes exactly. */
void gcn_ai_tick_legacy(void) {
    GcnAi* ai = s_ai;
    if (!ai || !(ai->control & GCN_AI_CR_PSTAT))
        return;   /* Update():142 / IsPlaying():323-326 */

    u32 ticks_per_sample = (ai->control & GCN_AI_CR_AISFR)
                               ? GCN_AI_TICKS_PER_SAMPLE_48KHZ
                               : GCN_AI_TICKS_PER_SAMPLE_32KHZ;
    if (++ai->tick_accum < ticks_per_sample)
        return;
    ai->tick_accum = 0;
    ai_advance_sample(ai);
}

/* DERIVED cycle-accurate pacing (ai.h derivation comment). `cycles` is the
 * real elapsed Gekko-core-cycle delta for the block just run (dispatch.c's
 * ctx->cycles - prev_cycles) — the same 486 MHz clock domain the x2
 * thresholds below are derived against, so no additional scaling is applied
 * (mirroring gcn_dsp_tick's PPC-cycle passthrough, dsp.c). Accumulates in x2
 * fixed point (never rounds, never drops a half-cycle) and loops so a
 * larger-than-one-sample delta (a big block, or a caller batching several
 * blocks' worth of cycles) still fires every sample it crossed, not just one —
 * unlike the legacy per-call counter, cycle deltas aren't bounded to "one tick
 * per call" so this must not assume a single crossing per invocation. */
void gcn_ai_tick(u32 cycles) {
    GcnAi* ai = s_ai;
    if (!ai || !(ai->control & GCN_AI_CR_PSTAT))
        return;   /* Update():142 / IsPlaying():323-326 */

    u32 threshold_x2 = (ai->control & GCN_AI_CR_AISFR)
                            ? GCN_AI_CYCLES_PER_SAMPLE_48KHZ_X2
                            : GCN_AI_CYCLES_PER_SAMPLE_32KHZ_X2;
    ai->cycle_accum_x2 += (u64)cycles * 2u;
    while (ai->cycle_accum_x2 >= threshold_x2) {
        ai->cycle_accum_x2 -= threshold_x2;
        ai_advance_sample(ai);
    }
}
