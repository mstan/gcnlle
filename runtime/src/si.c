/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal SI model — a plain register file over the SI block. See si.h for the
 * scope (SIEXILK read-back now; controller polling later, oracle-driven).
 */
#include "si/si.h"

#include <string.h>

void gcn_si_init(GcnSi* si) {
    memset(si, 0, sizeof *si);
}

static u32 si_index(u32 addr) {
    return (addr - GCN_SI_BASE) >> 2;   /* bus guarantees addr in [base, base+size) */
}

u32 gcn_si_read(void* user, CPUState* cpu, u32 addr, u8 size) {
    (void)cpu; (void)size;
    return ((GcnSi*)user)->reg[si_index(addr)];
}

void gcn_si_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size) {
    (void)cpu; (void)size;
    ((GcnSi*)user)->reg[si_index(addr)] = value;
}
