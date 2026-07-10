/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal Serial Interface (SI) model.
 *
 * The GameCube SI drives the four controller ports (SICOMCSR / SIPOLL / SISR +
 * per-port input buffers) plus a few control registers. During early IPL boot
 * the ONLY SI register touched is **SIEXILK** (0xCC00643C, the SI/EXI clock
 * lock): the IPL writes it, then reads it back to confirm. This model is a
 * plain register file — writes are stored, reads return the stored value —
 * which is exactly correct for a read/write control register like SIEXILK.
 *
 * It is deliberately NOT yet a controller-polling model: SICOMCSR's
 * auto-clearing transfer bits, SISR read-only status, and per-port controller
 * input arrive when the menu polls for input (M3/M4) and the oracle shows those
 * accesses. Any SI register whose real behaviour differs from plain read-back
 * will DIVERGE from the Dolphin oracle loudly — that divergence is the signal
 * to model it properly, never a silent fake (PRINCIPLES: Runtime Boundaries).
 */
#ifndef GCN_SI_SI_H
#define GCN_SI_SI_H

#include "cpu/cpu.h"

#define GCN_SI_BASE  0xCC006400u
#define GCN_SI_SIZE  0x80u          /* SI register block (32 x u32) */

typedef struct { u32 reg[GCN_SI_SIZE / 4]; } GcnSi;

void gcn_si_init(GcnSi* si);
u32  gcn_si_read(void* user, CPUState* cpu, u32 addr, u8 size);
void gcn_si_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size);

#endif /* GCN_SI_SI_H */
