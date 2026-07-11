/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Memory Interface (MI) model (impl). See include/mi/mi.h for scope. A plain
 * u16 register file with zero side effects, transcribed from Dolphin
 * MemoryInterfaceManager::RegisterMMIO (Core/HW/MemoryInterface.cpp:76-124):
 * every register is a DirectRead/DirectWrite<u16>, init is a memset to zero
 * (Init:60-64), and 32-bit accesses split into 16-bit halves (.cpp:119-123).
 */
#include "mi/mi.h"

#include <stdio.h>
#include <string.h>

void gcn_mi_init(GcnMi* mi) {
    /* MemoryInterfaceManager::Init:60-64 — memset the whole register struct. */
    memset(mi, 0, sizeof *mi);
}

/* ---- 16-bit register access (pure read-back; no side effects) ---- */

static u16 mi_read16(GcnMi* mi, u32 off) {
    return mi->reg[off >> 1];
}

static void mi_write16(GcnMi* mi, u32 off, u16 value) {
    mi->reg[off >> 1] = value;
}

u32 gcn_mi_read(void* user, CPUState* cpu, u32 addr, u8 size) {
    GcnMi* mi = (GcnMi*)user;
    u32 off = addr - GCN_MI_BASE;
    (void)cpu;
    switch (size) {
    case 1: {
        /* Map 8-bit reads onto the containing 16-bit register. */
        u16 v = mi_read16(mi, off & ~1u);
        return (addr & 1u) ? (v & 0xFFu) : (v >> 8);
    }
    case 2:
        return mi_read16(mi, off);
    default: /* 4: HI halfword at off, LO at off+2 (Dolphin ReadToSmaller<u32>) */
        return (mi_read16(mi, off) << 16) | mi_read16(mi, off + 2u);
    }
}

void gcn_mi_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size) {
    GcnMi* mi = (GcnMi*)user;
    u32 off = addr - GCN_MI_BASE;
    (void)cpu;
    switch (size) {
    case 1: {
        u16 v = mi_read16(mi, off & ~1u);
        if (addr & 1u) v = (u16)((v & 0xFF00u) | (value & 0xFFu));
        else           v = (u16)((v & 0x00FFu) | ((value & 0xFFu) << 8));
        mi_write16(mi, off & ~1u, v);
        return;
    }
    case 2:
        mi_write16(mi, off, (u16)value);
        return;
    default: /* 4: HI halfword at off, LO at off+2 (Dolphin WriteToSmaller<u32>) */
        mi_write16(mi, off, (u16)(value >> 16));
        mi_write16(mi, off + 2u, (u16)value);
        return;
    }
}
