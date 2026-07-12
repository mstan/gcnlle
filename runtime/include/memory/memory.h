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

/* Resolve a guest address to a host pointer into MEM1 (or MEM2 if allocated),
 * collapsing the cached (0x80..) and uncached (0xC0..) mirrors to the same
 * bytes. Returns NULL if the address is not RAM-backed; *avail (if non-NULL)
 * receives the number of contiguous bytes available from that pointer. */
u8* gcn_mem_resolve(CPUState* cpu, u32 addr, u32* avail);

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

#ifdef __cplusplus
}
#endif

#endif /* GCN_MEMORY_MEMORY_H */
