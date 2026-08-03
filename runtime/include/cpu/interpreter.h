/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Native-first Gekko interpreter fallback.
 *
 * The dispatch loop calls this only after every registered native candidate
 * rejects the live PC. The fallback records an append-only content identity
 * before executing the instruction.
 */
#ifndef GCN_CPU_INTERPRETER_H
#define GCN_CPU_INTERPRETER_H

#include "cpu/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

int  gcn_interpreter_note_native_miss(CPUState* cpu);
int  gcn_interpreter_step(CPUState* cpu);
void gcn_interpreter_shutdown(void);
u64  gcn_interpreter_instruction_count(void);
u64  gcn_interpreter_unique_miss_count(void);

/* Observability: how many times the native-miss page-CRC memo actually
 * hashed a page (vs. served a cached identity). See miss_page_crc
 * (interpreter.c) for the memo's exactness contract. */
u64  gcn_interpreter_page_crc_recomputes(void);

/* Test-only: force the counted-cache-loop batching fast path (interpreter.c)
 * on or off so a test can diff batched vs. unbatched execution of the same
 * synthetic loop. Defaults to enabled; production code never calls this. */
void gcn_interpreter_set_cache_loop_batch_enabled(bool enabled);

#ifdef __cplusplus
}
#endif

#endif

