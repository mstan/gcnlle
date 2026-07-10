/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gcnrecomp runtime — general Flipper MMIO device-dispatch layer (impl).
 * See include/mmio/mmio.h for the design. This is the single tracing point for
 * all MMIO and the fallback that keeps unmapped accesses loud (never faked).
 */
#include "mmio/mmio.h"
#include "trace/trace.h"

#include <stdio.h>

void gcn_mmio_bus_init(GcnMmioBus* bus) {
    bus->count = 0;
}

static GcnMmioDevice* find_device(GcnMmioBus* bus, u32 addr) {
    for (u32 i = 0; i < bus->count; i++) {
        GcnMmioDevice* d = &bus->devices[i];
        if (addr >= d->base && addr < d->base + d->size)
            return d;
    }
    return NULL;
}

bool gcn_mmio_register(GcnMmioBus* bus, const char* name, u32 base, u32 size,
                       GcnMmioReadFn read, GcnMmioWriteFn write, void* user) {
    if (bus->count >= GCN_MMIO_MAX_DEVICES) {
        fprintf(stderr, "gcn mmio: device table full, cannot register %s\n", name);
        return false;
    }
    for (u32 i = 0; i < bus->count; i++) {
        GcnMmioDevice* d = &bus->devices[i];
        if (base < d->base + d->size && d->base < base + size) {
            fprintf(stderr, "gcn mmio: %s [0x%08X+0x%X) overlaps %s [0x%08X+0x%X)\n",
                    name, base, size, d->name, d->base, d->size);
            return false;
        }
    }
    GcnMmioDevice* d = &bus->devices[bus->count++];
    d->name = name; d->base = base; d->size = size;
    d->read = read; d->write = write; d->user = user;
    return true;
}

/* Track unmapped addresses so we warn once per address, not once per access
 * (a busy-wait on an unmodeled status bit would otherwise flood stderr). */
static void warn_unmapped(u32 addr, int is_write) {
    static u32 seen[64];
    static u32 seen_n = 0;
    for (u32 i = 0; i < seen_n; i++)
        if (seen[i] == addr) return;
    if (seen_n < 64) seen[seen_n++] = addr;
    fprintf(stderr, "gcn mmio: %s unmapped 0x%08X (no device model; returns 0)\n",
            is_write ? "write" : "read", addr);
}

static u64 mmio_ext_read(CPUState* cpu, u32 ea, u8 size) {
    GcnMmioBus* bus = (GcnMmioBus*)cpu->external_user_data;
    u32 value = 0;
    GcnMmioDevice* d = bus ? find_device(bus, ea) : NULL;
    if (d && d->read)
        value = d->read(d->user, cpu, ea, size);
    else
        warn_unmapped(ea, 0);
    if (gcn_trace_active())
        gcn_trace_mmio(cpu->pc, ea, value, size, 0);
    return value;
}

static void mmio_ext_write(CPUState* cpu, u32 ea, u64 value, u8 size) {
    GcnMmioBus* bus = (GcnMmioBus*)cpu->external_user_data;
    if (gcn_trace_active())
        gcn_trace_mmio(cpu->pc, ea, (u32)value, size, 1);
    GcnMmioDevice* d = bus ? find_device(bus, ea) : NULL;
    if (d && d->write)
        d->write(d->user, cpu, ea, (u32)value, size);
    else
        warn_unmapped(ea, 1);
}

void gcn_mmio_install(GcnMmioBus* bus, CPUState* cpu) {
    cpu->external_user_data = bus;
    cpu->external_read = mmio_ext_read;
    cpu->external_write = mmio_ext_write;
}
