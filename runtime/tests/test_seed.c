/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gcnrecomp runtime — M0 seed-contract unit test.
 *
 * Asserts the M0 deliverable without executing any PPC:
 *   1. MEM1 holds the BS2 payload bytes at 0x81300000 (cached + uncached alias).
 *   2. The entry PC is set to 0x81300000.
 *   3. The SRAM fixture's stored checksum validates (OSRtc.c algorithm).
 * Plus faithfulness checks on the bus: big-endian round-trip and the
 * cached/uncached mirror collapsing to the same bytes; and the MEM1 reset fill
 * outside the payload.
 *
 * The test is self-contained: it synthesises a payload of the exact BS2 size so
 * ctest passes with no copyrighted firmware present. If the environment variable
 * GCN_BS2_PATH points at a real descrambled BS2 dump of the right size, it is
 * additionally seeded and checked (never required).
 */
#include "seed/seed.h"
#include "memory/memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); g_failures++; } \
    else { fprintf(stdout, "ok: %s\n", (msg)); } \
} while (0)

/* Deterministic synthetic payload: byte i = low 8 bits of a mixing function of
 * i, so every 4-byte word is distinct and order-sensitive. */
static u8 synth_byte(u32 i) {
    u32 x = i * 2654435761u;      /* Knuth multiplicative hash */
    return (u8)((x >> 24) ^ (x >> 3) ^ i);
}

static void fill_synth(u8* buf, u32 size) {
    for (u32 i = 0; i < size; i++) buf[i] = synth_byte(i);
}

/* Verify the seed landed `payload` at load_addr in MEM1, entry pc, SRAM. */
static void check_seeded(CPUState* cpu, const GcnSeedDevices* dev,
                         const GcnSeedConfig* cfg) {
    /* (2) entry PC */
    CHECK(cpu->pc == GCN_BS2_ENTRY_PC, "entry PC == 0x81300000");

    /* (1) payload bytes present at load addr via the cached view */
    u32 avail = 0;
    u8* host = gcn_mem_resolve(cpu, cfg->load_addr, &avail);
    CHECK(host != NULL, "load address resolves into MEM1");
    CHECK(avail >= cfg->payload_size, "payload fits within MEM1 from load addr");
    if (host) {
        CHECK(memcmp(host, cfg->payload, cfg->payload_size) == 0,
              "MEM1 holds the BS2 payload bytes at 0x81300000");
    }

    /* (1b) same bytes visible through the uncached mirror (0xC0000000 alias) */
    u32 uncached_addr = (cfg->load_addr - GC_RAM_BASE) + GC_RAM_UNCACHED;
    u8* host_uncached = gcn_mem_resolve(cpu, uncached_addr, NULL);
    CHECK(host_uncached == host,
          "uncached mirror aliases the same MEM1 bytes as cached");

    /* (1c) spot-check the last payload word via the big-endian bus primitive */
    u32 last_off = cfg->payload_size - 4;
    u32 expect = read_be32(cfg->payload + last_off);
    u32 got = mem_read32(cpu, cfg->load_addr + last_off);
    CHECK(got == expect, "mem_read32 returns big-endian payload tail word");

    /* (1d) MEM1 immediately before the payload holds the reset fill */
    if (cfg->load_addr > GC_RAM_BASE) {
        u32 before = mem_read32(cpu, cfg->load_addr - 4);
        CHECK(before == cfg->mem1_fill, "MEM1 before payload == reset fill");
    }

    /* (3) SRAM checksum validates */
    CHECK(gcn_sram_validate(dev->sram),
          "SRAM fixture checksum validates (OSRtc.c algorithm)");

    /* device RTC fixture present */
    CHECK(dev->rtc_counter == cfg->rtc_counter, "RTC fixture counter installed");
}

/* Independent recomputation of the SRAM checksum for a hand-built image with
 * known settings, to pin the algorithm itself (not just self-consistency). */
static void test_sram_algorithm(void) {
    GcnSeedConfig cfg;
    gcn_seed_default_config(&cfg);
    cfg.language = 0;         /* English */
    cfg.sound_stereo = 1;     /* flags bit2 */
    cfg.progressive = 0;
    cfg.video_mode = 0;       /* NTSC */
    cfg.counter_bias = 0;

    u8 sram[GCN_SRAM_SIZE];
    gcn_seed_build_sram(sram, &cfg);

    /* flags byte must be exactly 0x04 (stereo, NTSC, non-progressive). */
    CHECK(sram[0x13] == 0x04, "SRAM flags == 0x04 (stereo/NTSC/non-progressive)");
    CHECK(sram[0x12] == 0x00, "SRAM language == English (0)");

    /* Manually recompute the checksum over the four BE16 words 0x0C..0x12. */
    u16 sum = 0, inv = 0;
    for (u32 off = 0x0C; off < 0x14; off += 2) {
        u16 w = read_be16(sram + off);
        sum = (u16)(sum + w);
        inv = (u16)(inv + (u16)~w);
    }
    CHECK(read_be16(sram + 0x00) == sum, "stored checkSum matches manual recompute");
    CHECK(read_be16(sram + 0x02) == inv, "stored checkSumInv matches manual recompute");

    /* Corrupting a covered byte must invalidate the SRAM. */
    u8 bad[GCN_SRAM_SIZE];
    memcpy(bad, sram, sizeof(bad));
    bad[0x13] ^= 0x04;   /* flip the sound bit */
    CHECK(!gcn_sram_validate(bad), "checksum rejects a corrupted flags byte");
}

int main(void) {
    fprintf(stdout, "== gcnrecomp M0 seed-contract test ==\n");

    /* --- SRAM algorithm pinning --- */
    test_sram_algorithm();

    /* --- synthetic payload path (always runs) --- */
    CPUState cpu;
    CHECK(cpu_init(&cpu), "cpu_init allocates MEM1");

    u8* payload = (u8*)malloc(GCN_BS2_USA_SIZE);
    CHECK(payload != NULL, "allocate synthetic payload");
    if (payload) {
        fill_synth(payload, GCN_BS2_USA_SIZE);

        GcnSeedConfig cfg;
        gcn_seed_default_config(&cfg);
        cfg.payload = payload;
        cfg.payload_size = GCN_BS2_USA_SIZE;

        GcnSeedDevices dev;
        CHECK(gcn_seed_apply(&cpu, &dev, &cfg), "gcn_seed_apply (synthetic)");
        check_seeded(&cpu, &dev, &cfg);

        /* A wrong-sized payload must be rejected (no silent truncation). */
        GcnSeedConfig bad = cfg;
        bad.payload_size = GCN_BS2_USA_SIZE - 4;
        CHECK(!gcn_seed_apply(&cpu, &dev, &bad),
              "gcn_seed_apply rejects a wrong-sized payload");

        free(payload);
    }

    /* --- optional real BS2 path (only if a dump is provided) --- */
    const char* real = getenv("GCN_BS2_PATH");
    if (real && *real) {
        u8* data = NULL; u32 size = 0;
        if (gcn_seed_read_file(real, &data, &size)) {
            fprintf(stdout, "-- real BS2 provided: %s (0x%X bytes)\n", real, size);
            GcnSeedConfig cfg;
            gcn_seed_default_config(&cfg);
            cfg.payload = data;
            cfg.payload_size = size;
            GcnSeedDevices dev;
            CHECK(gcn_seed_apply(&cpu, &dev, &cfg), "gcn_seed_apply (real BS2)");
            if (size == GCN_BS2_USA_SIZE) check_seeded(&cpu, &dev, &cfg);
            free(data);
        }
    }

    cpu_free(&cpu);

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    fprintf(stdout, "\nall checks passed\n");
    return 0;
}
