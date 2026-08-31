/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Content-keyed dispatch for relocated overlay code. See
 * include/cpu/overlay_module.h for why this exists separately from
 * cpu/aot_module.h.
 */
#include "cpu/overlay_module.h"
#include "cpu/aot_module.h"   /* gcn_aot_fnv1a64 -- one hash function, shared */

/* Same three MEM1 windows the reservation logic and the AOT module fold
 * together: cached, uncached, and the bare physical view used in real mode.
 * Overlay pages are recorded in the cached form, so everything canonicalizes
 * to that before lookup. */
static bool canonical_mem1(u32 address, u32* canonical, u32* offset) {
    if (address < GC_MAIN_RAM_SIZE) {
        *offset = address;
        *canonical = address | GC_RAM_BASE;
        return true;
    }
    if (address >= GC_RAM_BASE && address < GC_RAM_BASE + GC_MAIN_RAM_SIZE) {
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

static int find_page(const GcnOverlayModule* module, u32 canonical) {
    u32 lo = 0;
    u32 hi = module->page_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        const GcnOverlayPage* page = &module->pages[mid];
        if (canonical < page->start)
            hi = mid;
        else if (canonical >= page->end)
            lo = mid + 1u;
        else
            return (int)mid;
    }
    return -1;
}

/* Hash the page's live bytes once and record which compiled variant (if any)
 * is currently resident. Only reached after an invalidation, never per call. */
static u32 resolve_resident(GcnOverlayModule* module, CPUState* cpu,
                            u32 page_index, u32 offset) {
    const GcnOverlayPage* page = &module->pages[page_index];
    u32 length = page->end - page->start;
    if (offset > cpu->ram_size || length > cpu->ram_size - offset) {
        module->resident[page_index] = GCN_OVERLAY_NONE;
        return GCN_OVERLAY_NONE;
    }

    u64 live = gcn_aot_fnv1a64(cpu->ram + offset, length);
    module->hashes++;

    u32 chosen = GCN_OVERLAY_NONE;
    for (u32 i = 0; i < page->count; i++) {
        if (module->variants[page->first + i].hash == live) {
            chosen = page->first + i;
            break;
        }
    }
    if (chosen != GCN_OVERLAY_NONE)
        module->pages_resident++;
    module->resident[page_index] = chosen;
    return chosen;
}

bool gcn_overlay_module_call(GcnOverlayModule* module, CPUState* cpu,
                             u32 address) {
    u32 canonical;
    u32 offset;
    if (!module || !module->page_count || !cpu || !cpu->ram ||
        !canonical_mem1(address, &canonical, &offset))
        return false;

    int found = find_page(module, canonical);
    if (found < 0)
        return false;
    u32 page_index = (u32)found;

    u32 resident = module->resident[page_index];
    if (resident == GCN_OVERLAY_UNKNOWN) {
        /* Hash the whole page, not the entry offset. */
        u32 page_offset = module->pages[page_index].start - GC_RAM_BASE;
        resident = resolve_resident(module, cpu, page_index, page_offset);
    }
    if (resident == GCN_OVERLAY_NONE)
        return false;

    /* The generated body switches on ctx->pc, so a mid-page entry is fine. */
    cpu->pc = canonical;
    module->variants[resident].fn(cpu);
    module->dispatches++;
    return true;
}

void gcn_overlay_module_invalidate(GcnOverlayModule* module, u32 address,
                                   u32 size) {
    u32 canonical;
    u32 offset;
    if (!module || !module->page_count || !size ||
        !canonical_mem1(address, &canonical, &offset))
        return;

    const u64 end = (u64)canonical + size;
    for (u32 i = 0; i < module->page_count; i++) {
        const GcnOverlayPage* page = &module->pages[i];
        if (page->end <= canonical)
            continue;
        if ((u64)page->start >= end)
            break;
        if (module->resident[i] != GCN_OVERLAY_UNKNOWN &&
            module->resident[i] != GCN_OVERLAY_NONE)
            module->pages_resident--;
        module->resident[i] = GCN_OVERLAY_UNKNOWN;
        module->invalidations++;
    }
}
