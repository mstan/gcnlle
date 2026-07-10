/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Audio Interface (AI) model. See include/ai/ai.h for scope and the Dolphin
 * source the register semantics are transcribed from.
 */
#include "ai/ai.h"
#include "debug/rings.h"

#include <string.h>

void gcn_ai_set_irq(GcnAi* ai, GcnAiIrqFn fn, void* user) {
    ai->irq = fn;
    ai->irq_user = user;
}

void gcn_ai_init(GcnAi* ai) {
    memset(ai, 0, sizeof *ai);
    ai->control = GCN_AI_CR_RESET_VALUE;   /* AIS=48kHz | AID=32kHz (0x42) */
}

/* ---- interrupt line (AudioInterface.cpp UpdateInterrupts:96-99) ----
 * INT_CAUSE_AI = AICR.AIINT & AICR.AIINTMSK. Pushed to PI on EVERY evaluation
 * (PI INTSR is W1C, so a still-asserted level must re-appear). AIINT is only
 * ever cleared this milestone (the sample counter that would set it is deferred
 * — see ai.h), so the line stays low; the wiring is present so it asserts the
 * instant that machinery lands. */
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
    if (val & GCN_AI_CR_SCRESET)  ai->sample_counter = 0;
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
    case GCN_AI_SAMPLECNT: return ai->sample_counter;  /* idle: PSTAT=0 */
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
    case GCN_AI_SAMPLECNT: ai->sample_counter = value; break;
    case GCN_AI_INTTIMING: ai->int_timing = value; break;
    default: break;
    }
}
