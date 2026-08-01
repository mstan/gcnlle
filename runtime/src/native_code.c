/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu/native_code.h"

#include <string.h>

#define GCN_CODE_PAGE_SHIFT 12u
#define GCN_CODE_PAGE_SIZE (1u << GCN_CODE_PAGE_SHIFT)
#define GCN_MEM1_PAGE_COUNT (GC_MAIN_RAM_SIZE / GCN_CODE_PAGE_SIZE)

static u8 s_invalid[(GCN_MEM1_PAGE_COUNT + 7u) / 8u];
static u32 s_invalid_count;

static bool mem1_offset(u32 address, u32* offset) {
    if (address < GC_MAIN_RAM_SIZE) {
        *offset = address;
        return true;
    }
    if (address >= GC_RAM_BASE &&
        address < GC_RAM_BASE + GC_MAIN_RAM_SIZE) {
        *offset = address - GC_RAM_BASE;
        return true;
    }
    if (address >= GC_RAM_UNCACHED &&
        address < GC_RAM_UNCACHED + GC_MAIN_RAM_SIZE) {
        *offset = address - GC_RAM_UNCACHED;
        return true;
    }
    return false;
}

void gcn_native_code_reset(void) {
    memset(s_invalid, 0, sizeof s_invalid);
    s_invalid_count = 0;
}

void gcn_native_code_invalidate(u32 address, u32 size) {
    u32 offset;
    if (!size || !mem1_offset(address, &offset))
        return;

    u64 end = (u64)offset + size;
    if (end > GC_MAIN_RAM_SIZE)
        end = GC_MAIN_RAM_SIZE;
    u32 first = offset >> GCN_CODE_PAGE_SHIFT;
    u32 last = (u32)(end - 1u) >> GCN_CODE_PAGE_SHIFT;
    for (u32 page = first; page <= last; page++) {
        u8 mask = (u8)(1u << (page & 7u));
        u8* byte = &s_invalid[page >> 3];
        if (!(*byte & mask)) {
            *byte |= mask;
            s_invalid_count++;
        }
    }
}

bool gcn_native_code_is_invalid(u32 pc) {
    u32 offset;
    if (!mem1_offset(pc, &offset))
        return false;
    u32 page = offset >> GCN_CODE_PAGE_SHIFT;
    return (s_invalid[page >> 3] & (u8)(1u << (page & 7u))) != 0;
}

u32 gcn_native_code_invalid_page_count(void) {
    return s_invalid_count;
}
