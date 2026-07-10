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

u64 mem_read64(CPUState* cpu, u32 addr) {
    u32 avail;
    u8* host = gcn_mem_resolve(cpu, addr, &avail);
    if (!host || avail < 8) {
        if (cpu->external_read)
            return cpu->external_read(cpu, addr, 8);
        mmio_note(cpu, addr, 0u, 8, 0);
        return 0;
    }
    return read_be64(host);
}

void mem_write64(CPUState* cpu, u32 addr, u64 value) {
    u32 avail;
    u8* host = gcn_mem_resolve(cpu, addr, &avail);
    if (!host || avail < 8) {
        if (cpu->external_write) { cpu->external_write(cpu, addr, value, 8); return; }
        mmio_note(cpu, addr, (u32)value, 8, 1);
        return;
    }
    clear_matching_reservation(cpu, addr);
    write_be64(host, value);
}

u32 mem_read32(CPUState* cpu, u32 addr) {
    u32 avail;
    u8* host = gcn_mem_resolve(cpu, addr, &avail);
    if (!host || avail < 4) {
        if (cpu->external_read)
            return (u32)cpu->external_read(cpu, addr, 4);
        mmio_note(cpu, addr, 0u, 4, 0);
        return 0;
    }
    return read_be32(host);
}

void mem_write32(CPUState* cpu, u32 addr, u32 value) {
    u32 avail;
    u8* host = gcn_mem_resolve(cpu, addr, &avail);
    if (!host || avail < 4) {
        if (cpu->external_write) { cpu->external_write(cpu, addr, value, 4); return; }
        mmio_note(cpu, addr, value, 4, 1);
        return;
    }
    clear_matching_reservation(cpu, addr);
    write_be32(host, value);
}

u16 mem_read16(CPUState* cpu, u32 addr) {
    u32 avail;
    u8* host = gcn_mem_resolve(cpu, addr, &avail);
    if (!host || avail < 2) {
        if (cpu->external_read)
            return (u16)cpu->external_read(cpu, addr, 2);
        mmio_note(cpu, addr, 0u, 2, 0);
        return 0;
    }
    return read_be16(host);
}

void mem_write16(CPUState* cpu, u32 addr, u16 value) {
    u32 avail;
    u8* host = gcn_mem_resolve(cpu, addr, &avail);
    if (!host || avail < 2) {
        if (cpu->external_write) { cpu->external_write(cpu, addr, value, 2); return; }
        mmio_note(cpu, addr, value, 2, 1);
        return;
    }
    clear_matching_reservation(cpu, addr);
    write_be16(host, value);
}

u8 mem_read8(CPUState* cpu, u32 addr) {
    u32 avail;
    u8* host = gcn_mem_resolve(cpu, addr, &avail);
    if (!host) {
        if (cpu->external_read)
            return (u8)cpu->external_read(cpu, addr, 1);
        mmio_note(cpu, addr, 0u, 1, 0);
        return 0;
    }
    return *host;
}

void mem_write8(CPUState* cpu, u32 addr, u8 value) {
    u32 avail;
    u8* host = gcn_mem_resolve(cpu, addr, &avail);
    if (!host) {
        if (cpu->external_write) { cpu->external_write(cpu, addr, value, 1); return; }
        mmio_note(cpu, addr, value, 1, 1);
        return;
    }
    clear_matching_reservation(cpu, addr);
    *host = value;
}
