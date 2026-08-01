/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gcnrecomp runtime — faithful flat MEM1 + DolRecomp bus primitives.
 *
 * PROVENANCE: the resolve/read/write/lifecycle logic is adapted from the
 * DolRecomp fork's recompiler/src/cpu/cpu.c (GPL-3.0), which is the reference
 * implementation of the same ABI the generated code links against, and
 * cross-checked against reshine's runtime/src/cpu/recomp_cpu.c + memory/mem.c.
 * It is reimplemented here (not linked from the recompiler) so the runtime owns
 * its bus, exactly as reshine owns its own copy.
 *
 * FIDELITY: big-endian, boring, no fake-the-answer forcing. MEM1 is 24 MB at
 * guest 0x80000000; the uncached alias at 0xC0000000 addresses the SAME bytes.
 * Any access not backed by RAM routes to the device callback layer (installed
 * by future EXI/VI/GX/... models); with no callback we warn loudly and return
 * 0 — the warning is the signal, we never silently synthesize a value.
 */
#include "cpu/cpu.h"
#include "cpu/native_code.h"
#include "debug/rings.h"   /* [gcn-watch] gcn_ring_watch_check */
#include "memory/memory.h"
#include "trace/trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

bool cpu_init(CPUState* cpu) {
    memset(cpu, 0, sizeof(*cpu));
    gcn_native_code_reset();

    cpu->ram_size = GC_MAIN_RAM_SIZE;
    cpu->ram = (u8*)calloc(1, cpu->ram_size);
    if (!cpu->ram) {
        fprintf(stderr, "gcn memory: failed to allocate %u bytes for MEM1\n",
                cpu->ram_size);
        return false;
    }

    /* PVR (SPR 287) reads as the Gekko id. Mirror of recompiler cpu.c. */
    cpu->spr[287] = PPC_GEKKO_PVR;
    return true;
}

bool cpu_alloc_mem2(CPUState* cpu, u32 size) {
    /* MEM2 only exists on Wii; the GameCube IPL never touches it. Kept for ABI
     * parity with recompiler cpu.c so generated Wii code would still link. */
    free(cpu->mem2);
    cpu->mem2 = NULL;
    cpu->mem2_size = 0;

    if (size == 0)
        return true;

    cpu->mem2 = (u8*)calloc(1, size);
    if (!cpu->mem2) {
        fprintf(stderr, "gcn memory: failed to allocate %u bytes for MEM2\n", size);
        return false;
    }
    cpu->mem2_size = size;
    return true;
}

void cpu_free(CPUState* cpu) {
    if (cpu->ram) {
        free(cpu->ram);
        cpu->ram = NULL;
    }
    if (cpu->mem2) {
        free(cpu->mem2);
        cpu->mem2 = NULL;
        cpu->mem2_size = 0;
    }
}

void cpu_reset(CPUState* cpu) {
    /* Preserve the host-owned bindings (RAM backing + device callbacks) across
     * a register-file reset; zero everything else. Mirror of recompiler cpu.c. */
    u8* ram = cpu->ram;
    u32 ram_size = cpu->ram_size;
    u8* mem2 = cpu->mem2;
    u32 mem2_size = cpu->mem2_size;
    PPCExternalRead external_read = cpu->external_read;
    PPCExternalWrite external_write = cpu->external_write;
    PPCExternalRead32 external_read32 = cpu->external_read32;
    PPCExternalWrite32 external_write32 = cpu->external_write32;
    PPCInstructionFallback instruction_fallback = cpu->instruction_fallback;
    PPCHostCall host_call = cpu->host_call;
    void* external_user_data = cpu->external_user_data;

    memset(cpu, 0, sizeof(*cpu));
    gcn_native_code_reset();
    cpu->ram = ram;
    cpu->ram_size = ram_size;
    cpu->mem2 = mem2;
    cpu->mem2_size = mem2_size;
    cpu->external_read = external_read;
    cpu->external_write = external_write;
    cpu->external_read32 = external_read32;
    cpu->external_write32 = external_write32;
    cpu->instruction_fallback = instruction_fallback;
    cpu->host_call = host_call;
    cpu->external_user_data = external_user_data;

    if (cpu->ram)
        memset(cpu->ram, 0, cpu->ram_size);
    if (cpu->mem2)
        memset(cpu->mem2, 0, cpu->mem2_size);

    cpu->spr[287] = PPC_GEKKO_PVR;
}

/* ---------------------------------------------------------------------------
 * Address decode — cached + uncached mirror collapse
 * ------------------------------------------------------------------------- */

u8* gcn_mem_resolve(CPUState* cpu, u32 addr, u32* avail) {
    u32 dummy;
    if (!avail) avail = &dummy;

    if (addr >= GC_RAM_BASE && addr < GC_RAM_BASE + cpu->ram_size) {
        u32 offset = addr - GC_RAM_BASE;
        *avail = cpu->ram_size - offset;
        return cpu->ram + offset;
    }
    if (addr >= GC_RAM_UNCACHED && addr < GC_RAM_UNCACHED + cpu->ram_size) {
        u32 offset = addr - GC_RAM_UNCACHED;   /* uncached alias -> same bytes */
        *avail = cpu->ram_size - offset;
        return cpu->ram + offset;
    }
    /* Physical (real-mode) mirror: with MSR[IR]/[DR] cleared — as on any exception
     * entry — effective addresses ARE physical, and physical 0x00000000 is MEM1.
     * The BS2 exception handlers (installed in low memory, entered via the 0xC00
     * syscall vector) run in this mode and access physical low memory directly.
     * Physical MMIO (0x0C00xxxx) is NOT mapped here — no modeled handler touches
     * it yet; if one does it will diverge loudly (the signal to route it). */
    if (addr < cpu->ram_size) {
        *avail = cpu->ram_size - addr;
        return cpu->ram + addr;
    }
    if (cpu->mem2 && cpu->mem2_size) {
        if (addr >= WII_MEM2_BASE && addr < WII_MEM2_BASE + cpu->mem2_size) {
            u32 offset = addr - WII_MEM2_BASE;
            *avail = cpu->mem2_size - offset;
            return cpu->mem2 + offset;
        }
        if (addr >= WII_MEM2_UNCACHED && addr < WII_MEM2_UNCACHED + cpu->mem2_size) {
            u32 offset = addr - WII_MEM2_UNCACHED;
            *avail = cpu->mem2_size - offset;
            return cpu->mem2 + offset;
        }
    }
    *avail = 0;
    return NULL;
}

bool gcn_mem_in_mem1(u32 addr, u32 need_bytes) {
    if (need_bytes == 0) need_bytes = 1;
    if (addr >= GC_RAM_BASE && (u64)addr + need_bytes <= (u64)GC_RAM_BASE + GC_MAIN_RAM_SIZE)
        return true;
    if (addr >= GC_RAM_UNCACHED && (u64)addr + need_bytes <= (u64)GC_RAM_UNCACHED + GC_MAIN_RAM_SIZE)
        return true;
    return false;
}

bool gcn_mem_is_mmio(u32 addr) {
    return addr >= GCN_MMIO_BASE && addr < GCN_MMIO_BASE + GCN_MMIO_SIZE;
}

bool gcn_mem_load_blob(CPUState* cpu, u32 dst, const void* src, u32 size) {
    u32 avail = 0;
    u8* host = gcn_mem_resolve(cpu, dst, &avail);
    if (!host || avail < size) {
        fprintf(stderr,
                "gcn memory: blob load of %u bytes at 0x%08X does not fit in RAM"
                " (avail=%u)\n", size, dst, avail);
        return false;
    }
    memcpy(host, src, size);   /* guest image is already big-endian; verbatim */
    return true;
}

void gcn_mem_fill_mem1(CPUState* cpu, u32 pattern_be) {
    if (!cpu->ram) return;
    for (u32 off = 0; off + 4 <= cpu->ram_size; off += 4)
        write_be32(cpu->ram + off, pattern_be);
}

/* ---------------------------------------------------------------------------
 * M1: the 0xFFF00000 ROM window — READ-ONLY, consulted only from the read-side
 * bus primitives below (never gcn_mem_resolve / mem_write*), so a guest write
 * there is unmapped exactly like any other unbacked address.
 * ------------------------------------------------------------------------- */

void gcn_mem_set_rom_window(CPUState* cpu, const u8* rom, u32 size) {
    cpu->rom_window = rom;
    cpu->rom_window_size = rom ? size : 0u;
}

static const u8* rom_window_resolve(CPUState* cpu, u32 addr, u32* avail) {
    if (cpu->rom_window && addr >= GCN_ROM_WINDOW_BASE &&
        addr < GCN_ROM_WINDOW_BASE + cpu->rom_window_size) {
        u32 offset = addr - GCN_ROM_WINDOW_BASE;
        *avail = cpu->rom_window_size - offset;
        return cpu->rom_window + offset;
    }
    *avail = 0;
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Reservation bookkeeping (lwarx/stwcx) — mirror of recompiler cpu.c
 * ------------------------------------------------------------------------- */

static void clear_matching_reservation(CPUState* cpu, u32 addr) {
    if (cpu->reserve_valid && ((cpu->reserve_addr ^ addr) & ~31u) == 0)
        cpu->reserve_valid = false;
}

/* ---------------------------------------------------------------------------
 * Bus primitives (the DolRecomp ABI the generated C calls)
 *
 * RAM-backed access -> big-endian read/write of the flat buffer.
 * Non-RAM access    -> device callback layer (external_read/external_write).
 *                      With no device installed we warn once-per-size and
 *                      return 0. We NEVER silently synthesize a "convenient"
 *                      value (PRINCIPLES: Runtime Boundaries).
 * ------------------------------------------------------------------------- */

/* Device-layer access with no model installed: record it for the oracle diff
 * and warn loudly. We NEVER synthesize a value (reads return 0); the diff vs
 * Dolphin is what tells us which register actually matters. */
static void mmio_note(CPUState* cpu, u32 addr, u32 value, u8 size, int is_write) {
    if (gcn_trace_active())
        gcn_trace_mmio(cpu->pc, addr, value, size, is_write);
    fprintf(stderr, "gcn memory: %s%u unmapped 0x%08X (no device model installed)\n",
            is_write ? "write" : "read", (unsigned)(size * 8u), addr);
}

/* Phase C (codegen speed campaign): the emitter now inlines the RAM-hit fast
 * path directly into generated code (dolrecomp_mem_read*_fast/dolrecomp_mem_
 * write*_fast, emitted per-TU by recompiler/src/backend/emitter.c's
 * emit_header_for_cpu) for every plain load/store, so these functions are
 * now the SLOW path for those opcodes -- reached only for MMIO/ROM-window-
 * miss/MEM2/unmapped addresses -- plus the full path (fast-window-check +
 * slow) for every opcode the emitter still keeps on an unconditional call
 * (lwarx/stwcx/lswi/lswx/stswi/stswx/lmw/stmw/dcbz/lwbrx/lhbrx/stwbrx/sthbrx,
 * and cpu_glue.c's psq_* / dcbz_l/eciwx/ecowx helpers, which call the
 * *_legacy 2/3-arg form since they don't carry a per-call cia of their own --
 * their own caller stays pc-stamped by ppc_op_is_pc_pure's conservative
 * "calls another CPUState helper" class, so cpu->pc IS the correct cia at
 * that point, exactly like every other *_legacy caller).
 *
 * ABI: every mem_read* / mem_write* now takes the calling instruction's own
 * address as `cia` (see cpu.h's macro-dispatch comment for the *_cia/
 * *_legacy mechanism). The RAM fast path below (gcn_mem_resolve + rom
 * window) never touches cpu->pc; only the MMIO/external/unmapped slow
 * branch does, via `cpu->pc = cia;` BEFORE dispatching to external_read/
 * external_write/mmio_note, so every consumer of cpu->pc downstream of
 * those calls (gcn_trace_mmio, the always-on rings, mmio.c's device
 * dispatch, the card-traffic ring, the oracle trace) observes exactly the
 * value the emitter's old unconditional pre-instruction pc-stamp used to
 * guarantee. See runtime/ABI.md sec. 3. */

u64 mem_read64_cia(CPUState* cpu, u32 addr, u32 cia) {
    u32 avail;
    u8* host = gcn_mem_resolve(cpu, addr, &avail);
    if (host && avail >= 8) return read_be64(host);
    {
        const u8* rom = rom_window_resolve(cpu, addr, &avail);
        if (rom && avail >= 8) return read_be64(rom);
    }
    cpu->pc = cia;
    if (cpu->external_read)
        return cpu->external_read(cpu, addr, 8);
    mmio_note(cpu, addr, 0u, 8, 0);
    return 0;
}
u64 mem_read64_legacy(CPUState* cpu, u32 addr) { return mem_read64_cia(cpu, addr, cpu->pc); }

void mem_write64_cia(CPUState* cpu, u32 addr, u64 value, u32 cia) {
    u32 avail;
    u8* host = gcn_mem_resolve(cpu, addr, &avail);
    if (!host || avail < 8) {
        cpu->pc = cia;
        if (cpu->external_write) { cpu->external_write(cpu, addr, value, 8); return; }
        mmio_note(cpu, addr, (u32)value, 8, 1);
        return;
    }
    clear_matching_reservation(cpu, addr);
    gcn_native_code_invalidate(addr, 8);
    write_be64(host, value);
}
void mem_write64_legacy(CPUState* cpu, u32 addr, u64 value) { mem_write64_cia(cpu, addr, value, cpu->pc); }

u32 mem_read32_cia(CPUState* cpu, u32 addr, u32 cia) {
    u32 avail;
    u8* host = gcn_mem_resolve(cpu, addr, &avail);
    if (host && avail >= 4) return read_be32(host);
    {
        const u8* rom = rom_window_resolve(cpu, addr, &avail);
        if (rom && avail >= 4) return read_be32(rom);
    }
    cpu->pc = cia;
    if (cpu->external_read)
        return (u32)cpu->external_read(cpu, addr, 4);
    mmio_note(cpu, addr, 0u, 4, 0);
    return 0;
}
u32 mem_read32_legacy(CPUState* cpu, u32 addr) { return mem_read32_cia(cpu, addr, cpu->pc); }

void mem_write32_cia(CPUState* cpu, u32 addr, u32 value, u32 cia) {
    u32 avail;
    gcn_ring_watch_check(addr, value, 4u, cia);   /* [gcn-watch] psq/slow path */
    u8* host = gcn_mem_resolve(cpu, addr, &avail);
    if (!host || avail < 4) {
        cpu->pc = cia;
        if (cpu->external_write) { cpu->external_write(cpu, addr, value, 4); return; }
        mmio_note(cpu, addr, value, 4, 1);
        return;
    }
    clear_matching_reservation(cpu, addr);
    gcn_native_code_invalidate(addr, 4);
    write_be32(host, value);
}
void mem_write32_legacy(CPUState* cpu, u32 addr, u32 value) { mem_write32_cia(cpu, addr, value, cpu->pc); }

u16 mem_read16_cia(CPUState* cpu, u32 addr, u32 cia) {
    u32 avail;
    u8* host = gcn_mem_resolve(cpu, addr, &avail);
    if (host && avail >= 2) return read_be16(host);
    {
        const u8* rom = rom_window_resolve(cpu, addr, &avail);
        if (rom && avail >= 2) return read_be16(rom);
    }
    cpu->pc = cia;
    if (cpu->external_read)
        return (u16)cpu->external_read(cpu, addr, 2);
    mmio_note(cpu, addr, 0u, 2, 0);
    return 0;
}
u16 mem_read16_legacy(CPUState* cpu, u32 addr) { return mem_read16_cia(cpu, addr, cpu->pc); }

void mem_write16_cia(CPUState* cpu, u32 addr, u16 value, u32 cia) {
    u32 avail;
    gcn_ring_watch_check(addr, value, 2u, cia);   /* [gcn-watch] */
    u8* host = gcn_mem_resolve(cpu, addr, &avail);
    if (!host || avail < 2) {
        cpu->pc = cia;
        if (cpu->external_write) { cpu->external_write(cpu, addr, value, 2); return; }
        mmio_note(cpu, addr, value, 2, 1);
        return;
    }
    clear_matching_reservation(cpu, addr);
    gcn_native_code_invalidate(addr, 2);
    write_be16(host, value);
}
void mem_write16_legacy(CPUState* cpu, u32 addr, u16 value) { mem_write16_cia(cpu, addr, value, cpu->pc); }

u8 mem_read8_cia(CPUState* cpu, u32 addr, u32 cia) {
    u32 avail;
    u8* host = gcn_mem_resolve(cpu, addr, &avail);
    if (host) return *host;
    {
        const u8* rom = rom_window_resolve(cpu, addr, &avail);
        if (rom) return *rom;
    }
    cpu->pc = cia;
    if (cpu->external_read)
        return (u8)cpu->external_read(cpu, addr, 1);
    mmio_note(cpu, addr, 0u, 1, 0);
    return 0;
}
u8 mem_read8_legacy(CPUState* cpu, u32 addr) { return mem_read8_cia(cpu, addr, cpu->pc); }

void mem_write8_cia(CPUState* cpu, u32 addr, u8 value, u32 cia) {
    u32 avail;
    gcn_ring_watch_check(addr, value, 1u, cia);   /* [gcn-watch] */
    u8* host = gcn_mem_resolve(cpu, addr, &avail);
    if (!host) {
        cpu->pc = cia;
        if (cpu->external_write) { cpu->external_write(cpu, addr, value, 1); return; }
        mmio_note(cpu, addr, value, 1, 1);
        return;
    }
    clear_matching_reservation(cpu, addr);
    gcn_native_code_invalidate(addr, 1);
    *host = value;
}
void mem_write8_legacy(CPUState* cpu, u32 addr, u8 value) { mem_write8_cia(cpu, addr, value, cpu->pc); }
