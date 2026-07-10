/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Audio Interface (AI) model. See include/ai/ai.h for scope and the Dolphin
 * source the register semantics are transcribed from.
 */
#include "ai/ai.h"

#include <string.h>

void gcn_ai_init(GcnAi* ai) {
    memset(ai, 0, sizeof *ai);
    ai->control = GCN_AI_CR_RESET_VALUE;   /* AIS=48kHz | AID=32kHz (0x42) */
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
