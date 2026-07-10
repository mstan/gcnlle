/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runtime-side producer for the differential-oracle trace (oracle/trace_format.h).
 * When $GCN_TRACE_OUT is set, gcn_boot writes a RUNTIME-producer trace stream
 * that oracle/diff.py compares (by value + order) against the Dolphin tap.
 * When unset, every hook is a no-op — zero cost on normal runs.
 *
 * M0 emits GCN_TR_MMIO events (every Flipper MMIO access), which is the stream
 * directly comparable to Dolphin's for finding the first divergence — e.g. the
 * first hardware register we read as 0 that real hardware returns non-zero.
 * EXI/retired records are added as those subsystems come online.
 */
#ifndef GCN_TRACE_H
#define GCN_TRACE_H

#include "cpu/cpu.h"

void gcn_trace_init(void);   /* opens $GCN_TRACE_OUT (if set), writes file header */
void gcn_trace_close(void);
int  gcn_trace_active(void);
void gcn_trace_mmio(u32 pc, u32 addr, u32 value, u8 size, int is_write);

#endif /* GCN_TRACE_H */
