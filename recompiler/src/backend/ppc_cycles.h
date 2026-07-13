#ifndef DOLRECOMP_PPC_CYCLES_H
#define DOLRECOMP_PPC_CYCLES_H

#include "../common/types.h"
#include "../frontend/decoder.h"

/* Per-opcode retired-instruction cycle cost, transcribed from Dolphin's
 * PPCTables.cpp GekkoOPTemplate num_cycles field (master @ 2026-07-12) — see
 * ppc_cycles.c for the full citation. This is the ORACLE's own timing model:
 * do not "improve" individual values from a hardware datasheet. */
u32 dr_ppc_num_cycles(PPCOpcode op);

#endif /* DOLRECOMP_PPC_CYCLES_H */
