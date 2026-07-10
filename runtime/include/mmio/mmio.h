/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gcnrecomp runtime — general Flipper MMIO device-dispatch layer.
 *
 * memory.c routes every non-RAM access to cpu->external_read / external_write.
 * This layer IS that callback: it owns a small table of registered device
 * models keyed by [base, base+size) address range and forwards each access to
 * the matching model, falling back to a loud "unmapped" trace+zero for any
 * address no model claims (the M0 divergence signal — never a silent fake).
 *
 * It is DELIBERATELY GENERAL. EXI is the first tenant; VI / GX / DSP / DI / SI
 * all register through the same table in later milestones, so the routing +
 * tracing lives here once, not per-device.
 *
 * TRACING: this layer is the single point that emits GCN_TR_MMIO records. When
 * a device answers a read, we trace the value the DEVICE returned (so the
 * runtime stream carries real hardware values, e.g. an EXI CSR read-back, and
 * lines up value+order against the Dolphin oracle). memory.c does NOT trace
 * once a device layer is installed — it delegates here.
 */
#ifndef GCN_MMIO_MMIO_H
#define GCN_MMIO_MMIO_H

#include "cpu/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GCN_MMIO_MAX_DEVICES 24

/* A device answers accesses in [base, base+size). `addr` passed to the callback
 * is the absolute guest address (not an offset) so a model spanning several
 * sub-blocks (like EXI's 3 channels) can decode it itself. Reads/writes are
 * 1/2/4 bytes; the GameCube firmware uses 4-byte MMIO almost exclusively. */
typedef u32  (*GcnMmioReadFn)(void* user, CPUState* cpu, u32 addr, u8 size);
typedef void (*GcnMmioWriteFn)(void* user, CPUState* cpu, u32 addr, u32 value, u8 size);

typedef struct {
    const char*    name;
    u32            base;
    u32            size;
    GcnMmioReadFn  read;
    GcnMmioWriteFn write;
    void*          user;
} GcnMmioDevice;

typedef struct {
    GcnMmioDevice devices[GCN_MMIO_MAX_DEVICES];
    u32           count;
} GcnMmioBus;

void gcn_mmio_bus_init(GcnMmioBus* bus);

/* Register a device model over [base, base+size). Returns false if the table is
 * full or the range overlaps an existing device (overlaps are a wiring bug). */
bool gcn_mmio_register(GcnMmioBus* bus, const char* name, u32 base, u32 size,
                       GcnMmioReadFn read, GcnMmioWriteFn write, void* user);

/* Install this bus as the CPU's external device callback layer (external_read /
 * external_write + external_user_data). After this, every non-RAM access from
 * the generated code lands here. */
void gcn_mmio_install(GcnMmioBus* bus, CPUState* cpu);

#ifdef __cplusplus
}
#endif

#endif /* GCN_MMIO_MMIO_H */
