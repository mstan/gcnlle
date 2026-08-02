/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Content-validated native chunk selection. This is adapted for gcnrecomp's
 * LLE runtime from ModernGekko/RecompCore's StaticRecomp chunk verification
 * design (ModernGekko dda273bddf486063df0b9c3c8dc2ca479f8d0180,
 * RecompCore e13ab348f13cd67879f6db6e9d7185410f8f62c6).
 */
#include "cpu/aot_module.h"

u64 gcn_aot_fnv1a64(const u8* data, u32 size) {
    u64 hash = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

static bool canonical_mem1(u32 address, u32* canonical, u32* offset) {
    if (address < GC_MAIN_RAM_SIZE) {
        *offset = address;
        *canonical = address | GC_RAM_BASE;
        return true;
    }
    if (address >= GC_RAM_BASE &&
        address < GC_RAM_BASE + GC_MAIN_RAM_SIZE) {
        *offset = address - GC_RAM_BASE;
        *canonical = address;
        return true;
    }
    if (address >= GC_RAM_UNCACHED &&
        address < GC_RAM_UNCACHED + GC_MAIN_RAM_SIZE) {
        *offset = address - GC_RAM_UNCACHED;
        *canonical = *offset | GC_RAM_BASE;
        return true;
    }
    return false;
}

static int find_chunk(const GcnAotModule* module, u32 address) {
    u32 lo = 0;
    u32 hi = module->count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        const GcnAotRange* range = &module->chunks[mid];
        if (address < range->start)
            hi = mid;
        else if (address >= range->end)
            lo = mid + 1u;
        else
            return (int)mid;
    }
    return -1;
}

bool gcn_aot_module_dispatchable(GcnAotModule* module, CPUState* cpu,
                                 u32 address, u32* chunk_index) {
    u32 canonical;
    u32 ignored_offset;
    if (!module || !cpu || !cpu->ram ||
        !canonical_mem1(address, &canonical, &ignored_offset))
        return false;

    int found = find_chunk(module, canonical);
    if (found < 0)
        return false;
    u32 index = (u32)found;
    if (chunk_index)
        *chunk_index = index;

    if (module->states[index] == GCN_AOT_VERIFIED)
        return true;
    if (module->states[index] == GCN_AOT_FAILED)
        return false;

    const GcnAotRange* chunk = &module->chunks[index];
    u32 start_offset = chunk->start - GC_RAM_BASE;
    u32 length = chunk->end - chunk->start;
    module->verifications++;
    if (chunk->start < GC_RAM_BASE || start_offset >= cpu->ram_size ||
        length > cpu->ram_size - start_offset ||
        gcn_aot_fnv1a64(cpu->ram + start_offset, length) !=
            module->hashes[index]) {
        module->states[index] = GCN_AOT_FAILED;
        module->failed++;
        return false;
    }

    module->states[index] = GCN_AOT_VERIFIED;
    module->verified++;
    return true;
}

void gcn_aot_module_invalidate(GcnAotModule* module, u32 address, u32 size) {
    u32 canonical;
    u32 ignored_offset;
    if (!module || !size ||
        !canonical_mem1(address, &canonical, &ignored_offset))
        return;

    const u64 end = (u64)canonical + size;
    for (u32 i = 0; i < module->count; i++) {
        const GcnAotRange* chunk = &module->chunks[i];
        if (chunk->end <= canonical)
            continue;
        if ((u64)chunk->start >= end)
            break;
        if (module->states[i] == GCN_AOT_VERIFIED)
            module->verified--;
        else if (module->states[i] == GCN_AOT_FAILED)
            module->failed--;
        module->states[i] = GCN_AOT_UNVERIFIED;
        module->invalidations++;
    }
}
