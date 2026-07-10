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
#include "mmio/mmio.h"
#include "exi/exi.h"
#include "si/si.h"
#include "pi/pi.h"
#include "dsp/dsp.h"
#include "ai/ai.h"
#include "vi/vi.h"
#include "di/di.h"
#include "debug/rings.h"
#include "debug/debug_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* VI display-interrupt line -> PI interrupt cause (level-triggered). */
static void vi_irq_to_pi(void* user, int level) {
    gcn_pi_set_interrupt((GcnPi*)user, GCN_PI_INT_VI, level);
}

/* DI drive interrupt line -> PI interrupt cause (level-triggered). */
static void di_irq_to_pi(void* user, int level) {
    gcn_pi_set_interrupt((GcnPi*)user, GCN_PI_INT_DI, level);
}

/* DSP mailbox / ARAM / AI-DMA interrupt line -> PI interrupt cause. */
static void dsp_irq_to_pi(void* user, int level) {
    gcn_pi_set_interrupt((GcnPi*)user, GCN_PI_INT_DSP, level);
}

/* EXI (any-channel) interrupt line -> PI interrupt cause. */
static void exi_irq_to_pi(void* user, int level) {
    gcn_pi_set_interrupt((GcnPi*)user, GCN_PI_INT_EXI, level);
}

/* SI (RDST/transfer-complete) interrupt line -> PI interrupt cause. */
static void si_irq_to_pi(void* user, int level) {
    gcn_pi_set_interrupt((GcnPi*)user, GCN_PI_INT_SI, level);
}

/* AI (streaming) interrupt line -> PI interrupt cause. */
static void ai_irq_to_pi(void* user, int level) {
    gcn_pi_set_interrupt((GcnPi*)user, GCN_PI_INT_AI, level);
}

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

    /* ---- MMIO device-dispatch layer + EXI model ----
     * Route non-RAM (0xCC00xxxx) accesses through a general device-dispatch bus
     * and register the EXI model on it. Every future device (VI/GX/DSP/DI/SI)
     * registers on this same bus. */
    static GcnMmioBus bus;   /* static: outlives main's scope for the callbacks */
    static GcnExi     exi;
    gcn_mmio_bus_init(&bus);
    gcn_exi_init(&exi);

    /* EXI mask-ROM backing: prefer a full descrambled IPL image via
     * GCN_IPL_ROM (offset 0 = ROM offset 0); otherwise reuse the descrambled
     * BS2 payload already loaded, whose byte k IS ROM offset (load_off + k). The
     * IPL font/ROM DMA reads offsets 0x800..0x170800, all within that slice, so
     * both backings serve identical bytes. */
    u8* rom_file = NULL;
    u32 rom_size = 0;
    const char* rom_path = getenv("GCN_IPL_ROM");
    if (rom_path && *rom_path && gcn_seed_read_file(rom_path, &rom_file, &rom_size)) {
        gcn_exi_set_rom(&exi, rom_file, rom_size, 0u);
        fprintf(stdout, "gcn boot: EXI mask-ROM backed by %s (%u bytes)\n",
                rom_path, rom_size);
    } else {
        /* load_addr is guest 0x8120_0000; ROM offset of payload[0] is the file
         * offset the descramble started at (0x100 for USA). Derive it from the
         * documented load contract: payload[0] == descrambled IPL[0x100]. */
        gcn_exi_set_rom(&exi, payload, size, 0x100u);
        fprintf(stdout, "gcn boot: EXI mask-ROM backed by BS2 payload window "
                "(ROM base 0x100, %u bytes)\n", size);
    }
    gcn_exi_set_sram(&exi, devices.sram);
    gcn_exi_set_rtc(&exi, devices.rtc_counter);

    gcn_mmio_register(&bus, "EXI", GCN_EXI_BASE, GCN_EXI_REGISTER_BYTES,
                      gcn_exi_read, gcn_exi_write, &exi);

    /* SI: minimal read-back register file (early boot touches only SIEXILK). */
    static GcnSi si;
    gcn_si_init(&si);
    gcn_mmio_register(&bus, "SI", GCN_SI_BASE, GCN_SI_SIZE,
                      gcn_si_read, gcn_si_write, &si);

    /* PI: R/W register file + read-only chipset-revision register (0x2C). The
     * menu reads the revision at stage-2 pc 0x813004B0. */
    static GcnPi pi;
    gcn_pi_init(&pi);
    gcn_mmio_register(&bus, "PI", GCN_PI_BASE, GCN_PI_SIZE,
                      gcn_pi_read, gcn_pi_write, &pi);

    /* EXI/SI were constructed above (before PI); wire their interrupt lines now
     * that the PI sink exists. EXI asserts INT_CAUSE_EXI (transfer-complete /
     * device / cover), SI asserts INT_CAUSE_SI (RDST / transfer-complete). */
    gcn_exi_set_irq(&exi, exi_irq_to_pi, &pi);
    gcn_si_set_irq(&si, si_irq_to_pi, &pi);

    /* DSP: the REAL DSP core runs its firmware (IROM + uploaded ucode). Load the
     * DSP ROM dumps (GCN_DSP_ROM / GCN_DSP_COEF, default bios/dsp_{rom,coef}.bin).
     * The Flipper AR/AI DMA engine is modeled inside the DSP device itself. */
    static GcnDsp dsp;
    u8* dsp_rom = NULL; u32 dsp_rom_size = 0;
    u8* dsp_coef = NULL; u32 dsp_coef_size = 0;
    const char* dsp_rom_path  = getenv("GCN_DSP_ROM");
    const char* dsp_coef_path = getenv("GCN_DSP_COEF");
    if (!dsp_rom_path  || !*dsp_rom_path)  dsp_rom_path  = "bios/dsp_rom.bin";
    if (!dsp_coef_path || !*dsp_coef_path) dsp_coef_path = "bios/dsp_coef.bin";
    if (!gcn_seed_read_file(dsp_rom_path, &dsp_rom, &dsp_rom_size) ||
        !gcn_seed_read_file(dsp_coef_path, &dsp_coef, &dsp_coef_size)) {
        fprintf(stderr, "gcn boot: DSP ROM load failed (%s / %s) — set GCN_DSP_ROM/GCN_DSP_COEF\n",
                dsp_rom_path, dsp_coef_path);
        cpu_free(&cpu); free(payload); return 1;
    }
    gcn_dsp_init(&dsp, dsp_rom, dsp_coef, cpu.ram, cpu.ram_size);
    gcn_dsp_set_irq(&dsp, dsp_irq_to_pi, &pi);
    gcn_mmio_register(&bus, "DSP", GCN_DSP_BASE, GCN_DSP_SIZE,
                      gcn_dsp_read, gcn_dsp_write, &dsp);

    /* AI: audio-interface control register (menu reads/writes it during init). */
    static GcnAi ai;
    gcn_ai_init(&ai);
    gcn_ai_set_irq(&ai, ai_irq_to_pi, &pi);
    gcn_mmio_register(&bus, "AI", GCN_AI_BASE, GCN_AI_SIZE,
                      gcn_ai_read, gcn_ai_write, &ai);

    /* VI: register file + cycle-driven beam counter + display interrupts. The
     * IPL busy-waits on the vertical beam position (0xCC00202C); the beam is
     * driven from the guest time base by the dispatch loop (gcn_vi_tick). The
     * DI comparators assert INT_CAUSE_VI in the PI cause word. */
    static GcnVi vi;
    gcn_vi_init(&vi);
    gcn_vi_set_irq(&vi, vi_irq_to_pi, &pi);
    gcn_mmio_register(&bus, "VI", GCN_VI_BASE, GCN_VI_SIZE,
                      gcn_vi_read, gcn_vi_write, &vi);

    /* DI: the GameCube DVD drive with NO DISC INSERTED. The menu kicks a
     * DVDReadDiskID (DICMDBUF0=0xA8000040, DICR=3) and polls DICVR every frame;
     * with no disc the read completes with DEINT and DICVR bit 0 (cover open)
     * reads 1, so the menu concludes "no disc" and proceeds. The drive
     * interrupt line asserts INT_CAUSE_DI in the PI cause word. */
    static GcnDi di;
    gcn_di_init(&di);
    gcn_di_set_irq(&di, di_irq_to_pi, &pi);
    gcn_mmio_register(&bus, "DI", GCN_DI_BASE, GCN_DI_SIZE,
                      gcn_di_read, gcn_di_write, &di);

    gcn_mmio_install(&bus, &cpu);

    gcn_trace_init();  /* emits a RUNTIME trace to $GCN_TRACE_OUT if set */

    /* Observability: bring up the always-on ring buffers and (if GCN_DEBUG_PORT
     * is set) the TCP debug query surface over them + live CPU/RAM state. */
    gcn_rings_init();
    GcnDebugCtx dbg = { &cpu };
    int dbg_port = gcn_debug_server_start(&dbg);
    /* With the debug server on, run unbounded and park on stop, so the process
     * (and its always-on rings) stay inspectable until a client sends "quit"
     * rather than racing to the block budget. */
    u32 run_blocks = (dbg_port > 0) ? 0u : max_blocks;

    fprintf(stdout,
        "gcn boot: seeded; entering recompiled BS2 at 0x%08X (budget %u blocks)\n"
        "--- execution (unmapped-MMIO warnings below are the M0 signal) ---\n",
        cpu.pc, dbg_port > 0 ? 0u : max_blocks);
    fflush(stdout);

    int still_live = gcn_dispatch_run(&cpu, run_blocks);
    if (dbg_port > 0 && !gcn_debug_server_quit_requested())
        gcn_debug_server_park();
    gcn_debug_server_stop();
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
        "  lr          : 0x%08X\n"
        "  srr0(fault) : 0x%08X\n"
        "  dar (addr)  : 0x%08X\n"
        "  dsisr       : 0x%08X\n",
        reason, cpu.pc, cpu.exception, cpu.lr, cpu.srr0, cpu.dar, cpu.dsisr);

    /* Modify-before-recomp: dump MEM1 regions (the post-DMA code image + the
     * BS2 low-memory exception handlers) so DMA-loaded / runtime-written code can
     * be recompiled from what actually lands in RAM. GCN_MEM_DUMP is one or more
     * ';'-separated "<hexaddr>:<hexlen>:<path>" specs (guest addr, e.g.
     * 0x81200000 for stage-2, 0x80000000 for the low-memory handlers). */
    const char* dump_spec = getenv("GCN_MEM_DUMP");
    if (dump_spec) {
        char specs[2048];
        snprintf(specs, sizeof(specs), "%s", dump_spec);
        for (char* s = strtok(specs, ";"); s; s = strtok(NULL, ";")) {
            unsigned daddr = 0, dlen = 0; char dpath[512] = {0};
            if (sscanf(s, "%x:%x:%511[^\n]", &daddr, &dlen, dpath) == 3 &&
                daddr >= GC_RAM_BASE &&
                (unsigned long long)daddr + dlen <= (unsigned long long)GC_RAM_BASE + cpu.ram_size) {
                FILE* df = fopen(dpath, "wb");
                if (df) {
                    fwrite(cpu.ram + (daddr - GC_RAM_BASE), 1, dlen, df);
                    fclose(df);
                    fprintf(stdout, "gcn boot: dumped MEM1[0x%08X..0x%08X] -> %s\n",
                            daddr, daddr + dlen, dpath);
                } else fprintf(stderr, "gcn boot: cannot open dump path %s\n", dpath);
            } else fprintf(stderr, "gcn boot: bad/out-of-range GCN_MEM_DUMP spec '%s'\n", s);
        }
    }

    gcn_dsp_free(&dsp);
    cpu_free(&cpu);
    free(payload);
    return 0;
}
