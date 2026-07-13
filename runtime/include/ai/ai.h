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
 * while PSTAT=1, at the AISFR-selected rate. Two pacing models coexist here —
 * see dispatch.c's derived-cycle-accuracy A/B knob (GCN_CYCLES_UNIFORM) for
 * which one a given run takes:
 *
 * LEGACY (gcn_ai_tick_legacy, calls-counted): before the recompiler emitted
 * real per-instruction cycle counts, the dispatch loop had no per-block cycle
 * count to pace against, only a call. So the threshold was derived by folding
 * the fixed per-block device-cycle constant (GCN_CORE_CYCLES_PER_BLOCK = 96,
 * dispatch.c) into a call count:
 *   48 kHz: 486,000,000 / 48000   = 10125    cycles/sample -> /96 ~= 105 ticks/sample
 *   32 kHz: 486,000,000 / 32000   = 15187.5  cycles/sample -> /96 ~= 158 ticks/sample
 * i.e. the legacy threshold is really floor(10125/96)=105 and floor(15187.5/96)
 * =158 CALLS, not cycles — an approximation of the true cycles/sample rate
 * (105*96=10080 and 158*96=15168, both short of the true 10125/15187.5). This
 * function and GCN_AI_TICKS_PER_SAMPLE_* are kept byte-for-bit unmodified
 * (see gcn_ai_tick_legacy in ai.c) so GCN_CYCLES_UNIFORM=1 / pre-cycle-count
 * generated code reproduce the pre-derived-accuracy golden XFB hashes exactly.
 *
 * DERIVED (gcn_ai_tick(cycles), cycle-accurate): now that the dispatch loop
 * has a real per-block Gekko-core-cycle delta (CPUState.cycles, cpu.h), pace
 * AISCNT against the TRUE cycles/sample rate directly instead of the
 * 96-cycles/call-derived approximation above:
 *   48 kHz: 486,000,000 / 48000   = 10125    cycles/sample  (exact integer)
 *   32 kHz: 486,000,000 / 32000   = 15187.5  cycles/sample  (exact half-cycle)
 * 32 kHz isn't integral, so the derived accumulator (GcnAi.cycle_accum_x2)
 * and its threshold are carried in x2 fixed point (doubled) so the .5 is
 * represented exactly, never rounded and never dropped — same "never lose or
 * double-charge a cycle" remainder-carry discipline as dsp.c's /6 divide:
 *   48 kHz: 10125    cycles/sample -> x2 threshold = 20250 (GCN_AI_CYCLES_PER_SAMPLE_48KHZ_X2)
 *   32 kHz: 15187.5  cycles/sample -> x2 threshold = 30375 (GCN_AI_CYCLES_PER_SAMPLE_32KHZ_X2)
 *
 * NOTE: because 96 doesn't evenly divide 10125 (10125/96 = 105.46875), feeding
 * a flat 96 cycles/call through the DERIVED threshold would first cross 10125
 * on the 106th call (96*105=10080 < 10125 <= 96*106=10176) — ONE BLOCK LATER
 * than the legacy path's 105th-call fire. The two models are deliberately
 * NOT unified into one cycle-threshold function for exactly this reason: doing
 * so would make the "byte-identical legacy fallback" requirement impossible to
 * satisfy from cycle math alone. Keeping gcn_ai_tick_legacy as a wholly
 * separate, untouched function is what makes GCN_CYCLES_UNIFORM=1 an EXACT
 * reproduction rather than a close approximation.
 *
 * Nominal timing only — the poll-collapsed oracle diff (--counter-polls) is
 * insensitive to the exact rate; what matters is that AISCNT ADVANCES and
 * AIINT PACES the guest. */
#define GCN_AI_TICKS_PER_SAMPLE_48KHZ  105u   /* legacy: calls, not cycles */
#define GCN_AI_TICKS_PER_SAMPLE_32KHZ  158u   /* legacy: calls, not cycles */

#define GCN_AI_CYCLES_PER_SAMPLE_48KHZ_X2  20250u  /* derived: 2 * 10125 exact */
#define GCN_AI_CYCLES_PER_SAMPLE_32KHZ_X2  30375u  /* derived: 2 * 15187.5 exact */

/* Level change on the AI->PI interrupt line (level: 1 assert, 0 deassert). */
typedef void (*GcnAiIrqFn)(void* user, int level);

typedef struct {
    u32 control;        /* AICR                                            */
    u32 volume;         /* AIVR                                            */
    u32 sample_counter; /* AISCNT                                          */
    u32 int_timing;     /* AIIT                                            */
    u32 tick_accum;     /* gcn_ai_tick_legacy accumulator (calls-counted)  */
    u64 cycle_accum_x2; /* gcn_ai_tick accumulator (x2 fixed-point cycles) */
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
/* DERIVED cycle-accurate pacing: advance AISCNT by the real elapsed Gekko-core
 * cycle delta (dispatch.c's ctx->cycles - prev_cycles for the block just run;
 * same clock domain the 486 MHz derivation above uses directly, no scaling).
 * Called once per executed block from dispatch.c, after gcn_dsp_tick — s_ai
 * singleton pattern, see ai.c. No-op while PSTAT=0 (IsPlaying() gate,
 * AudioInterface.cpp:142/323-326). Used only when dispatch.c is in derived
 * mode (a real per-block cycle count is available and GCN_CYCLES_UNIFORM is
 * not set) — see the ai.h derivation comment above for why this is a distinct
 * function from gcn_ai_tick_legacy rather than a unified cycle threshold. */
void gcn_ai_tick(u32 cycles);
/* LEGACY calls-counted pacing: byte-for-bit the original gcn_ai_tick body
 * (one call == one implicit 96-cycle tick, thresholds GCN_AI_TICKS_PER_SAMPLE_*
 * in ai.h). Called once per executed block from dispatch.c ONLY in fallback
 * mode (delta==0 / GCN_CYCLES_UNIFORM=1) so that mode reproduces the
 * pre-derived-cycle-accuracy golden XFB hashes bit-for-bit — see dispatch.c. */
void gcn_ai_tick_legacy(void);

#endif /* GCN_AI_AI_H */
