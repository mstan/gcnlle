/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Audio Interface (AI) model (Flipper AI block, 0xCC006C00).
 *
 * The AI drives streaming (DVD/"AIS") audio and the audio-DMA ("AID") sample
 * clock. During boot the IPL reads the control register once and writes it back
 * unchanged (a read-modify-write that leaves the power-on configuration in
 * place):
 *   R  AICR -> 0x00000042   (AISFR=48kHz | AIDFR=32kHz — the power-on value)
 *   W  AICR  = 0x00000042
 *
 * Register + bit semantics are transcribed from Dolphin's AudioInterface
 * (oracle/dolphin/.../AudioInterface.{h,cpp}), our independent oracle:
 *   - Init() zeroes AICR then sets AIS rate = 48kHz (AISFR=1) and AID rate =
 *     32kHz (AIDFR=1) => 0x42. That is the reset value we model.
 *   - AICR reads are a direct read of the register; writes update the
 *     AIINTMSK/AIINTVLD/AISFR/AIDFR/PSTAT bits, clear AIINT on write-1 (W1C),
 *     and reset the sample counter when SCRESET is written (SCRESET itself is a
 *     trigger and always reads back 0).
 *   - The sample counter (0x08) free-runs off the CPU clock only while PSTAT=1.
 *     The boot path never starts playback, so it reads its stored base (0). If a
 *     later path polls it while playing, the live count will DIVERGE from the
 *     oracle loudly (we don't model AI cycle timing) — the signal to model it,
 *     never a silent fake (PRINCIPLES: Runtime Boundaries).
 */
#ifndef GCN_AI_AI_H
#define GCN_AI_AI_H

#include "cpu/cpu.h"

#define GCN_AI_BASE  0xCC006C00u
#define GCN_AI_SIZE  0x20u

/* Register offsets (YAGCD / Dolphin AudioInterface). */
#define GCN_AI_CONTROL    0x00u       /* AICR: control/status              */
#define GCN_AI_VOLUME     0x04u       /* AIVR: streaming volume (l/r)      */
#define GCN_AI_SAMPLECNT  0x08u       /* AISCNT: sample counter            */
#define GCN_AI_INTTIMING  0x0Cu       /* AIIT: interrupt timing            */

/* AICR bits (Dolphin AudioInterface::AICR). */
#define GCN_AI_CR_PSTAT    0x01u       /* sample counter / playback enable  */
#define GCN_AI_CR_AISFR    0x02u       /* AIS frequency (0=32kHz 1=48kHz)   */
#define GCN_AI_CR_AIINTMSK 0x04u       /* interrupt enable                  */
#define GCN_AI_CR_AIINT    0x08u       /* interrupt status (W1C)            */
#define GCN_AI_CR_AIINTVLD 0x10u       /* interrupt-timing validity         */
#define GCN_AI_CR_SCRESET  0x20u       /* reset sample counter (trigger)    */
#define GCN_AI_CR_AIDFR    0x40u       /* AID frequency (0=48kHz 1=32kHz)   */

/* Power-on AICR: AIS=48kHz (AISFR) | AID=32kHz (AIDFR). */
#define GCN_AI_CR_RESET_VALUE  (GCN_AI_CR_AISFR | GCN_AI_CR_AIDFR)   /* 0x42 */

/* Level change on the AI->PI interrupt line (level: 1 assert, 0 deassert). */
typedef void (*GcnAiIrqFn)(void* user, int level);

typedef struct {
    u32 control;        /* AICR                                            */
    u32 volume;         /* AIVR                                            */
    u32 sample_counter; /* AISCNT base                                     */
    u32 int_timing;     /* AIIT                                            */
    int irq_level;      /* last AI->PI line level (edge detect for the ring) */
    GcnAiIrqFn irq;     /* sink for the AI interrupt line (boot.c -> PI) */
    void*      irq_user;
} GcnAi;

/* INTERRUPT LINE (Dolphin AudioInterface.cpp UpdateInterrupts:96-99):
 * INT_CAUSE_AI = AICR.AIINT & AICR.AIINTMSK. We model the AICR interrupt
 * status/mask bits (AIINT is W1C, AIINTMSK/AIINTVLD are plain R/W — see ai.c
 * write_control) and wire this line into PI. DELIBERATELY DEFERRED (loud
 * divergence, never a silent fake — PRINCIPLES: Runtime Boundaries): the
 * sample-counter / interrupt-timing machinery that SETS AIINT
 * (GenerateAudioInterrupt when AISCNT crosses AIIT while PSTAT=1) is NOT
 * modeled, so AIINT is never asserted this milestone and the line stays low.
 * The boot path never starts AI playback, so nothing should raise it; a path
 * that polls a playing AISCNT/AIINT will diverge loudly from the oracle — the
 * signal to model the counter, not to fake it here. */
void gcn_ai_init(GcnAi* ai);
/* Register the AI interrupt-line sink (boot.c trampoline into PI INT_CAUSE_AI). */
void gcn_ai_set_irq(GcnAi* ai, GcnAiIrqFn fn, void* user);
u32  gcn_ai_read(void* user, CPUState* cpu, u32 addr, u8 size);
void gcn_ai_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size);

#endif /* GCN_AI_AI_H */
