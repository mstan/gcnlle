/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runtime-only Gekko timing state. Keep this out of cpu/cpu.h: every generated
 * AOT shard includes that ABI header, so device-timing changes must not
 * invalidate the entire title module.
 */
#ifndef GCN_CPU_TIMING_H
#define GCN_CPU_TIMING_H

#include "cpu/cpu.h"

#define PPC_EXC_DECREMENTER    0x00000080u
#define PPC_VECTOR_DECREMENTER 0x00900u

void ppc_decrementer_reset(CPUState* cpu);
void ppc_decrementer_tick(CPUState* cpu, u32 tb_ticks);
void ppc_deliver_decrementer(CPUState* cpu);

#endif
