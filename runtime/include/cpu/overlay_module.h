/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Content-keyed native dispatch for RELOCATED overlay code (Wind Waker's REL
 * modules, and anything else the guest loads into RAM at runtime).
 *
 * WHY THIS IS NOT cpu/aot_module.h. That module maps one address range to one
 * body of code and one hash: the title DOL is loaded once, at a fixed address,
 * so address identifies code. Overlays break that assumption. Wind Waker loads
 * REL modules into the top of MEM1 and relocates them in place, so a single
 * page hosts many different modules over a run -- 0x80F00000 was observed with
 * 204 distinct content hashes in one session. Address no longer identifies
 * code; only content does.
 *
 * So a page here carries N variants, each keyed by the FNV-1a64 of its 4 KiB
 * of guest bytes, and dispatch picks the variant whose hash matches live RAM.
 * A page with no matching variant falls through to the interpreter exactly as
 * before -- an unknown overlay is a miss, never a guess.
 *
 * COST. Hashing 4 KiB on every dispatch would be far worse than the
 * interpreter it replaces, so the resident variant is cached per page and only
 * recomputed after an invalidation (icbi, or a write into the page). Steady
 * state is a binary search plus an indexed call.
 */
#ifndef GCN_CPU_OVERLAY_MODULE_H
#define GCN_CPU_OVERLAY_MODULE_H

#include "cpu/cpu.h"

typedef void (*GcnOverlayFn)(CPUState* ctx);

/* One 4 KiB guest page that has at least one recompiled variant. `first` and
 * `count` slice the shared variant array. Pages are sorted by `start` and do
 * not overlap, so lookup is a binary search. */
typedef struct {
    u32 start;
    u32 end;
    u32 first;
    u32 count;
} GcnOverlayPage;

/* One recompiled body: the hash of the exact bytes it was compiled from, and
 * its entry point. The generated function switches on ctx->pc internally, so
 * entering mid-page is fine. */
typedef struct {
    u64 hash;
    GcnOverlayFn fn;
} GcnOverlayVariant;

enum {
    GCN_OVERLAY_UNKNOWN = 0xFFFFFFFFu, /* not yet hashed since last invalidate */
    GCN_OVERLAY_NONE    = 0xFFFFFFFEu, /* hashed; no variant matches this page */
};

typedef struct {
    const GcnOverlayPage* pages;
    const GcnOverlayVariant* variants;
    u32* resident;      /* per-page: variant index, or UNKNOWN / NONE */
    u32 page_count;
    u32 variant_count;
    /* counters, for the TCP surface and the exit summary */
    u64 dispatches;
    u64 hashes;
    u64 invalidations;
    u32 pages_resident;
} GcnOverlayModule;

/* Try to run `address` natively from a content-matched overlay variant.
 * Returns false when the page is unknown, has no variant, or the resident
 * bytes match nothing we compiled -- caller falls back as usual. */
bool gcn_overlay_module_call(GcnOverlayModule* module, CPUState* cpu,
                             u32 address);

/* Drop cached residency for every page overlapping [address, address+size).
 * Called from the same icbi/write paths that invalidate the title module. */
void gcn_overlay_module_invalidate(GcnOverlayModule* module, u32 address,
                                   u32 size);

/* Installed by the generated overlay table (or the no-op build). */
bool gcn_overlay_call(CPUState* cpu, u32 address);
void gcn_overlay_icbi(u32 address, u32 size);

#endif /* GCN_CPU_OVERLAY_MODULE_H */
