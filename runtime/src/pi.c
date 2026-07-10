/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Processor Interface (PI) model. See include/pi/pi.h for scope. A plain R/W
 * register file over the PI block, with the one hardware-fixed register —
 * PI_REVISION (0x2C, read-only chipset revision) — returning the console
 * revision constant instead of stored state.
 */
#include "pi/pi.h"

#include <string.h>

void gcn_pi_init(GcnPi* pi) {
    memset(pi, 0, sizeof *pi);
}

static u32 pi_index(u32 addr) {
    return (addr - GCN_PI_BASE) >> 2;   /* bus guarantees addr in [base, base+size) */
}

u32 gcn_pi_read(void* user, CPUState* cpu, u32 addr, u8 size) {
    (void)cpu; (void)size;
    u32 off = addr - GCN_PI_BASE;
    if (off == GCN_PI_REVISION)
        return GCN_PI_REVISION_RETAIL;  /* read-only chipset revision */
    return ((GcnPi*)user)->reg[pi_index(addr)];
}

void gcn_pi_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size) {
    (void)cpu; (void)size;
    ((GcnPi*)user)->reg[pi_index(addr)] = value;
}
