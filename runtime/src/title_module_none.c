/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu/title_module.h"

bool gcn_title_module_call(CPUState* cpu, u32 address) {
    (void)cpu;
    (void)address;
    return false;
}

void gcn_title_module_icbi(u32 address, u32 size) {
    (void)address;
    (void)size;
}
