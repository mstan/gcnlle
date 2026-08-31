/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Overlay dispatch is opt-in: a build with no generated overlay table links
 * this instead, and every lookup simply misses so the interpreter handles
 * relocated code exactly as it did before. */
#include "cpu/overlay_module.h"

bool gcn_overlay_call(CPUState* cpu, u32 address) {
    (void)cpu;
    (void)address;
    return false;
}

void gcn_overlay_icbi(u32 address, u32 size) {
    (void)address;
    (void)size;
}
