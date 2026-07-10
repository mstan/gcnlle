/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gcnrecomp runtime — LLE host entry point (M0 stage).
 *
 * At M0 this does NOT execute PPC yet (Track A's recompiled BS2 is not linked).
 * It exercises the seed contract end-to-end against a real descrambled BS2 dump
 * when one is supplied, so the runtime side of M0 is verifiable today:
 *
 *   gcn_runtime <descrambled_bs2.bin>
 *
 * loads the payload, applies the declarative seed, and reports the seeded state
 * (entry PC, payload presence, SRAM validity, RTC). With no argument it prints
 * usage. Booting the real firmware lands when the generated C from Track A is
 * linked and the dispatch driver (TODO seam below) runs blocks from ctx->pc.
 */
#include "cpu/cpu.h"
#include "memory/memory.h"
#include "seed/seed.h"

#include <stdio.h>
#include <stdlib.h>

/* TODO(M0 integration): when Track A's generated C is linked, the dispatch
 * driver plugs in here. DolRecomp emits a static-inline dispatch table
 * (dolrecomp_find_original / dolrecomp_run_blocks in the generated header, see
 * recompiler/src/backend/dispatch.c); reshine re-exposes it non-static via a
 * copied dispatch_table.inc. We will do the same: a gcn_dispatch.c that
 * #includes the generated header and calls func_XXXXXXXX(ctx) for ctx->pc,
 * looping until ctx->exception or a host-entry sentinel, and driving the
 * device models (EXI/VI/GX/...) between block budgets. Left as a seam so this
 * translation unit stays free of any generated dependency at M0. */

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
            "gcnrecomp runtime (M0 seed stage)\n"
            "usage: %s <descrambled_bs2.bin>\n"
            "  Loads the offline-descrambled BS2 payload, applies the M0\n"
            "  declarative seed (entry 0x81300000, MEM1 reset fill, EXI SRAM +\n"
            "  RTC fixtures), and reports the seeded state. Does not execute PPC\n"
            "  yet (awaiting Track A's recompiled BS2 + the dispatch seam).\n",
            argv[0]);
        return 2;
    }

    u8* payload = NULL;
    u32 size = 0;
    if (!gcn_seed_read_file(argv[1], &payload, &size))
        return 1;

    CPUState cpu;
    if (!cpu_init(&cpu)) { free(payload); return 1; }

    GcnSeedConfig cfg;
    gcn_seed_default_config(&cfg);
    cfg.payload = payload;
    cfg.payload_size = size;

    GcnSeedDevices devices;
    if (!gcn_seed_apply(&cpu, &devices, &cfg)) {
        cpu_free(&cpu);
        free(payload);
        return 1;
    }

    fprintf(stdout,
        "gcn runtime: seed applied\n"
        "  entry pc        : 0x%08X\n"
        "  payload         : 0x%08X bytes @ 0x%08X\n"
        "  mem1 fill       : 0x%08X\n"
        "  SRAM checksum   : %s\n"
        "  SRAM language   : %u, flags 0x%02X\n"
        "  RTC counter     : %u (seconds since 2000-01-01)\n"
        "  next            : link Track A's BS2 + dispatch seam to execute.\n",
        cpu.pc, size, cfg.load_addr, cfg.mem1_fill,
        gcn_sram_validate(devices.sram) ? "valid" : "INVALID",
        devices.sram[0x12], devices.sram[0x13],
        devices.rtc_counter);

    cpu_free(&cpu);
    free(payload);
    return 0;
}
