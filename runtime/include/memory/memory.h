/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gcnrecomp runtime — flat MEM1 bus, runtime-facing helpers.
 *
 * The DolRecomp ABI-facing bus primitives (mem_read8..64 / mem_write8..64) and
 * the CPU lifecycle (cpu_init/reset/free/alloc_mem2) are declared in
 * cpu/cpu.h and implemented in memory.c. This header adds the host-side
 * conveniences the loader/seed/tests use: blob load into guest space, address
 * decode, and the cached<->uncached mirror mapping.
 *
 * FIDELITY (PRINCIPLES.md "Runtime Boundaries"): these primitives are boring.
 * MEM1 is 24 MB of flat storage at guest 0x80000000 with the uncached alias at
 * 0xC0000000 pointing at the SAME bytes. Reads/writes byte-swap (guest is
 * big-endian). Nothing here drops, forces, or rewrites an address to paper over
 * a bug — accesses outside RAM go to the device (external_*) layer, never to a
 * silent fake. (Contrast reshine's mem.c, which force-writes 0x80000044=0xFFFF
 * and fakes EXI/SI/VI reads — that is the HLE "fake-the-answer" shortcut our
 * LLE-first baseline forbids.)
 */
#ifndef GCN_MEMORY_MEMORY_H
#define GCN_MEMORY_MEMORY_H

#include "cpu/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MEM1 geometry (aliases of the cpu.h contract macros, named for the runtime). */
#define GCN_MEM1_BASE      GC_RAM_BASE        /* 0x80000000 cached */
#define GCN_MEM1_UNCACHED  GC_RAM_UNCACHED    /* 0xC0000000 uncached mirror */
#define GCN_MEM1_SIZE      GC_MAIN_RAM_SIZE   /* 24 MB */

/* Gekko locked L1 data-cache backing. The hardware tags are programmable, but
 * retail software conventionally addresses allocated lines through
 * 0xE0000000. Dolphin's LLE memory model exposes a 256-KiB backing here. */
#define GCN_L1_CACHE_BASE  0xE0000000u
#define GCN_L1_CACHE_SIZE  0x00040000u

/* Flipper MMIO aperture (for address classification; models land later). */
#define GCN_MMIO_BASE      0xCC000000u
#define GCN_MMIO_SIZE      0x01000000u

/* M1 finding: BS1's TRUE early bring-up (before it turns MSR[DR] on — the
 * ~0x100-0x14C window Dolphin's HLE never executes and M0 never reached
 * either) pokes hardware registers via the PHYSICAL address directly (e.g.
 * DSP reset at 0x0C005012/0x0C00501A, MI at 0x0C004026 — seen live on a real
 * true-reset BS1-at-ROM run, port 4386 rings). GCN_MMIO_PHYS_BASE is that
 * physical aperture — the SAME registers GCN_MMIO_BASE addresses once BATs +
 * DR are on, mirrored exactly like the RAM cached (0x80..)/uncached (0xC0..)
 * collapse above (a real hardware alias, not a device model change). Consumed
 * in mmio.c's dispatch translation. */
#define GCN_MMIO_PHYS_BASE 0x0C000000u

/* Resolve a guest address to a host pointer into MEM1, MEM2 if allocated, or
 * the locked-L1 window, collapsing cached/uncached RAM mirrors to the same
 * bytes. Returns NULL if the address is not memory-backed; *avail (if
 * non-NULL) receives the contiguous byte count. */
u8* gcn_mem_resolve(CPUState* cpu, u32 addr, u32* avail);

/* Canonical locked-L1 backing for debug/co-sim hashing. The runtime owns one
 * guest CPU per process; `cpu` must be that initialized instance. */
u8* gcn_mem_locked_l1(CPUState* cpu, u32* size);

/* True if addr (cached or uncached alias) lands in MEM1. */
bool gcn_mem_in_mem1(u32 addr, u32 need_bytes);

/* True if addr is in the Flipper MMIO aperture (0xCC000000..0xCCFFFFFF). */
bool gcn_mem_is_mmio(u32 addr);

/* Copy a host blob into guest memory at guest address `dst`. The bytes are
 * copied verbatim (the payload is already big-endian guest image data — no
 * swap). Returns false if the range does not fit in RAM. */
bool gcn_mem_load_blob(CPUState* cpu, u32 dst, const void* src, u32 size);

/* Fill the whole of MEM1 with a repeating 32-bit big-endian pattern. Used by
 * the seed to establish a deterministic reset image before the payload lands. */
void gcn_mem_fill_mem1(CPUState* cpu, u32 pattern_be);

/* M1: back the 0xFFF00000 ROM window (cpu.h GCN_ROM_WINDOW_BASE) with a
 * borrowed, already-descrambled image (see exi.c gcn_exi_set_rom_scrambled,
 * whose returned buffer is the intended source — one descrambled image feeds
 * both the EXI mask-ROM device and this CPU-side instruction/data window).
 * READ-ONLY by construction: only the mem_read* bus primitives consult it
 * (never gcn_mem_resolve, which mem_write* also uses), so a guest write into
 * this range stays unmapped/loud exactly like before this existed. `rom`/
 * `size` are borrowed (caller owns the lifetime); NULL/0 unmaps it again. */
void gcn_mem_set_rom_window(CPUState* cpu, const u8* rom, u32 size);

/* Canonicalize an effective address for lwarx/stwcx. reservation comparison.
 *
 * A reservation is held on a PHYSICAL address, so a store must clear it
 * whenever it touches the same physical granule through ANY alias. MEM1 is
 * reachable through three windows -- cached GC_RAM_BASE (0x80000000), uncached
 * GC_RAM_UNCACHED (0xC0000000), and the bare physical window below
 * GC_MAIN_RAM_SIZE (real mode, MSR[IR]/[DR] clear; see gcn_mem_resolve).
 * Comparing raw effective addresses lets a reservation taken through one alias
 * survive a store through another, and the following stwcx. then succeeds where
 * hardware fails it -- a silent failure of an atomic primitive in the lock and
 * queue code the IPL runs.
 *
 * Upstream DolRecomp (93b881c) folds only the cached/uncached pair by masking
 * bit 0x40000000; that is insufficient here because of the physical window.
 * reserve_addr stays the raw EA the guest presented (snapshots and the debug
 * server expose it), so canonicalization happens only at compare time.
 *
 * Lives in this header so every runtime call site shares ONE implementation --
 * memory.c's bus primitives and cpu_glue.c's dcbz path. The recompiler's
 * reference host mirrors it as reservation_key() in recompiler/src/cpu/cpu.c,
 * and the emitter emits it as dolrecomp_reservation_key(); all three must stay
 * semantically identical. */
static inline u32 gcn_reservation_key(u32 addr) {
    if (addr >= GC_RAM_BASE && addr < GC_RAM_BASE + GC_MAIN_RAM_SIZE)
        return addr - GC_RAM_BASE;
    if (addr >= GC_RAM_UNCACHED && addr < GC_RAM_UNCACHED + GC_MAIN_RAM_SIZE)
        return addr - GC_RAM_UNCACHED;
    if (addr < GC_MAIN_RAM_SIZE)
        return addr;
    return addr; /* not RAM: no alias to fold, compare raw */
}

/* True when a store to `addr` must break a reservation held at `reserve_addr`
 * (32-byte granule, alias-folded). */
static inline bool gcn_reservation_hit(u32 reserve_addr, u32 addr) {
    return ((gcn_reservation_key(reserve_addr) ^ gcn_reservation_key(addr)) &
            ~31u) == 0;
}

#ifdef __cplusplus
}
#endif

#endif /* GCN_MEMORY_MEMORY_H */
