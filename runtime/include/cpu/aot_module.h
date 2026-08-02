/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GCN_AOT_MODULE_H
#define GCN_AOT_MODULE_H

#include "cpu/cpu.h"

typedef struct {
    u32 start;
    u32 end;
} GcnAotRange;

enum {
    GCN_AOT_UNVERIFIED = 0,
    GCN_AOT_VERIFIED = 1,
    GCN_AOT_FAILED = 2,
};

typedef struct {
    const GcnAotRange* chunks;
    const u64* hashes;
    u8* states;
    u32 count;
    u32 verified;
    u32 failed;
    u64 verifications;
    u64 native_dispatches;
    u64 invalidations;
} GcnAotModule;

u64 gcn_aot_fnv1a64(const u8* data, u32 size);
bool gcn_aot_module_dispatchable(GcnAotModule* module, CPUState* cpu,
                                 u32 address, u32* chunk_index);
void gcn_aot_module_invalidate(GcnAotModule* module, u32 address, u32 size);

#endif
