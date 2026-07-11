/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Memory Interface (MI) model — a pure 16-bit register file at 0xCC004000.
 *
 * The Flipper MI block is the memory-controller face: memory-region
 * first/last-page registers, a protection type + address pair, an interrupt
 * mask/flag pair (MI_IRQMASK 0x1C, MI_IRQFLAG 0x1E), ten 32-bit split timers,
 * and two "unknown" registers. Dolphin's MemoryInterfaceManager
 * (Core/HW/MemoryInterface.cpp:17-124) models the WHOLE block as
 * DirectRead/DirectWrite<u16> pairs over a memset-zeroed MIMemStruct with ZERO
 * side effects, plus a blanket 32-bit ReadToSmaller/WriteToSmaller fallback
 * (.cpp:119-123) that splits every 4-aligned 32-bit access into its two 16-bit
 * halves. We transcribe exactly that: a plain u16 register file, no interrupt
 * modelling this increment (MI_IRQMASK/FLAG are stored but drive nothing — the
 * IPL's observed traffic is a single MI_IRQMASK write at 0xCC00401C).
 *
 * 32-bit access is split HI-half -> off, LO-half -> off+2 (big-endian), exactly
 * like vi.c — matching Dolphin's WriteToSmaller/ReadToSmaller<u32>.
 */
#ifndef GCN_MI_MI_H
#define GCN_MI_MI_H

#include "cpu/cpu.h"

#define GCN_MI_BASE  0xCC004000u
#define GCN_MI_SIZE  0x1000u        /* full MI block (Dolphin registers 0x000..0x1000) */

/* Observed register offset (Core/HW/MemoryInterface.cpp:29). */
#define GCN_MI_IRQMASK  0x1Cu
#define GCN_MI_IRQFLAG  0x1Eu

typedef struct {
    u16 reg[GCN_MI_SIZE / 2];   /* 16-bit register backing (off >> 1) */
} GcnMi;

void gcn_mi_init(GcnMi* mi);
u32  gcn_mi_read(void* user, CPUState* cpu, u32 addr, u8 size);
void gcn_mi_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size);

#endif /* GCN_MI_MI_H */
