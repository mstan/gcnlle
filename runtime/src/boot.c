/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gcn_boot — M0 execution: seed the real descrambled BS2 and actually run the
 * recompiled firmware blocks from the entry PC, reporting where it stops.
 *
 *   gcn_boot <descrambled_bs2.bin> [max_blocks]
 *
 * This is the first point BS2 executes. It will run real PowerPC until it
 * touches hardware we don't model yet — the memory layer prints a loud
 * "unmapped 0x0C..." for the first MMIO register BS2 reaches for, which is the
 * signal for the next device to build. A finite block budget (default 200k)
 * prevents a hang if BS2 busy-waits on a status bit that currently reads 0.
 */
#include "cpu/cpu.h"
#include "memory/memory.h"
#include "seed/seed.h"
#include "dispatch/dispatch.h"
#include "trace/trace.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
            "gcnrecomp boot (M0 execution)\n"
            "usage: %s <descrambled_bs2.bin> [max_blocks]\n"
            "  Seeds the BS2 payload + M0 declarative seed, then runs recompiled\n"
            "  blocks from 0x81300000 until it stops (exception, off-image PC, or\n"
            "  the block budget). Default budget: 200000 blocks.\n",
            argv[0]);
        return 2;
    }
    u32 max_blocks = (argc >= 3) ? (u32)strtoul(argv[2], NULL, 0) : 200000u;

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

    gcn_trace_init();  /* emits a RUNTIME trace to $GCN_TRACE_OUT if set */

    fprintf(stdout,
        "gcn boot: seeded; entering recompiled BS2 at 0x%08X (budget %u blocks)\n"
        "--- execution (unmapped-MMIO warnings below are the M0 signal) ---\n",
        cpu.pc, max_blocks);
    fflush(stdout);

    int still_live = gcn_dispatch_run(&cpu, max_blocks);
    gcn_trace_close();

    const char* reason =
        still_live ? "block budget reached (still live — likely busy-waiting on unmodeled HW)"
        : cpu.exception ? "PPC exception raised"
        : "PC has no recompiled function / host call (fell off the image)";

    fprintf(stdout,
        "\n--- stopped ---\n"
        "  reason      : %s\n"
        "  final pc    : 0x%08X\n"
        "  exception   : 0x%08X\n"
        "  lr          : 0x%08X\n",
        reason, cpu.pc, cpu.exception, cpu.lr);

    cpu_free(&cpu);
    free(payload);
    return 0;
}
