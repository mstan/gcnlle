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
 *     32kHz (AIDFR=1) => 0x42. That is the reset value we model
 *     (AudioInterface.cpp Init():193-205).
 *   - AICR reads are a direct read of the register; writes update the
 *     AIINTMSK/AIINTVLD/AISFR/AIDFR/PSTAT bits, clear AIINT on write-1 (W1C),
 *     and reset the sample counter when SCRESET is written (SCRESET itself is a
 *     trigger and always reads back 0) — RegisterMMIO AICR ComplexWrite:211-278.
 *   - AIINTVLD is stored as a plain R/W passthrough bit only. Its header comment
 *     (AudioInterface.h:76-78) describes a *hypothetical* hardware gate ("controls
 *     whether AIINT is affected by the Interrupt Timing register matching the
 *     sample counter"), but IncreaseSampleCount (AudioInterface.cpp:108-123), the
 *     ONLY place Dolphin sets AIINT from the counter/timing compare, never reads
 *     AIINTVLD. Transcribed faithfully: AIINTVLD does NOT gate the compare here
 *     either — Dolphin (our oracle) doesn't implement the documented gate, so
 *     neither do we (PRINCIPLES: Dolphin is the independent oracle — diff its
 *     actual behavior, not the register-bit folklore).
 *   - The sample counter (0x08, AISCNT) free-runs while PSTAT=1, advancing at the
 *     AISFR-selected rate (32/48 kHz) regardless of whether any DVD/DTK stream
 *     is actually producing samples: IncreaseSampleCount/Update (:108-155) gate
 *     only on IsPlaying() (m_control.PSTAT == 1, :323-326) — there is no check of
 *     DVDInterface/DTK stream state anywhere in this file ("Everything else
 *     relating to AID happens in DSP.cpp" per the file banner, :31-36 — AIS
 *     counting is self-contained cycle accounting). So a guest that sets PSTAT=1
 *     with NO DISC INSERTED (our DI model, di.h) still gets an advancing AISCNT
 *     in Dolphin, and so does ours: fidelity here means "advance unconditionally
 *     on PSTAT=1", not "advance only if a stream exists".
 *   - When AISCNT crosses AIIT (the interrupt-timing register, 0x0C) while
 *     PSTAT=1, AIINT latches (GenerateAudioInterrupt, :102-106) via the
 *     wraparound-safe compare in IncreaseSampleCount:116 — reduces to exact
 *     equality (sample_counter == int_timing) at our one-sample-per-tick
 *     granularity (see gcn_ai_tick below).
 *   - PSTAT edge / SCRESET / an AISCNT write all rebase Dolphin's timing anchor
 *     (m_last_cpu_time = now, RegisterMMIO :250-259, :268-275, :297-304); we
 *     mirror that by resetting the tick accumulator on the same three events.
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

/* Sample-counter pacing (AudioInterface.cpp IncreaseSampleCount/Update:108-155):
 * AISCNT advances by one AIS sample every "cycles per sample" Gekko core cycles
 * while PSTAT=1, at the AISFR-selected rate. Same derivation style as the DSP
 * AID block pacing (dsp.h GCN_DSP_AID_TICKS_PER_BLOCK): 486 MHz nominal core
 * clock / rate = core cycles/sample, divided by the per-block core-cycle budget
 * the dispatch loop advances every device tick (GCN_CORE_CYCLES_PER_BLOCK = 96,
 * dispatch.c) to get the tick-accumulator threshold gcn_ai_tick counts against
 * (one call per executed block, mirroring gcn_dsp_tick's dsp_aid_tick):
 *   48 kHz: 486,000,000 / 48000   = 10125    cycles/sample -> /96 ~= 105 ticks/sample
 *   32 kHz: 486,000,000 / 32000   = 15187.5  cycles/sample -> /96 ~= 158 ticks/sample
 * Nominal timing only — the poll-collapsed oracle diff (--counter-polls) is
 * insensitive to the exact rate; what matters is that AISCNT ADVANCES and
 * AIINT PACES the guest. */
#define GCN_AI_TICKS_PER_SAMPLE_48KHZ  105u
#define GCN_AI_TICKS_PER_SAMPLE_32KHZ  158u

/* Level change on the AI->PI interrupt line (level: 1 assert, 0 deassert). */
typedef void (*GcnAiIrqFn)(void* user, int level);

typedef struct {
    u32 control;        /* AICR                                            */
    u32 volume;         /* AIVR                                            */
    u32 sample_counter; /* AISCNT                                          */
    u32 int_timing;     /* AIIT                                            */
    u32 tick_accum;     /* gcn_ai_tick accumulator (sample-period pacing)  */
    int irq_level;      /* last AI->PI line level (edge detect for the ring) */
    GcnAiIrqFn irq;     /* sink for the AI interrupt line (boot.c -> PI) */
    void*      irq_user;
} GcnAi;

/* INTERRUPT LINE (Dolphin AudioInterface.cpp UpdateInterrupts:96-100):
 * INT_CAUSE_AI = AICR.AIINT & AICR.AIINTMSK. We model the AICR interrupt
 * status/mask bits (AIINT is W1C, AIINTMSK/AIINTVLD are plain R/W — see ai.c
 * write_control), the sample counter + interrupt-timing compare that SETS
 * AIINT (gcn_ai_tick, transcribing IncreaseSampleCount/GenerateAudioInterrupt),
 * and wire the resulting line into PI. */
void gcn_ai_init(GcnAi* ai);
/* Register the AI interrupt-line sink (boot.c trampoline into PI INT_CAUSE_AI). */
void gcn_ai_set_irq(GcnAi* ai, GcnAiIrqFn fn, void* user);
u32  gcn_ai_read(void* user, CPUState* cpu, u32 addr, u8 size);
void gcn_ai_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size);
/* Advance the AI sample counter one tick (called once per executed block from
 * dispatch.c, after gcn_dsp_tick — s_ai singleton pattern, see ai.c). No-op
 * while PSTAT=0 (IsPlaying() gate, AudioInterface.cpp:142/323-326). */
void gcn_ai_tick(void);

#endif /* GCN_AI_AI_H */
