/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gcnrecomp runtime — M0 declarative seed contract (implementation).
 *
 * Every field written here carries a provenance tag ([ROM]/[YAGCD]/[FIXTURE]/
 * [DEFAULT]/[DEBT]) as required by DESIGN.md §6. No input comes from a Dolphin
 * RAM snapshot: the payload is the user's real IPL descrambled OFFLINE, the SRAM
 * and RTC are explicit fixtures, and the CPU latches we cannot yet derive from
 * BS1 disassembly are FLAGGED as [DEBT] rather than lifted from the oracle.
 */
#include "seed/seed.h"
#include "memory/memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * SRAM fixture — layout + checksum
 *
 * OSSram layout (YAGCD / dolphin SDK decomp OSRtc.h):
 *   0x00 u16 checkSum
 *   0x02 u16 checkSumInv
 *   0x04 u32 ead0            (BBA/flash id; 0 = none)
 *   0x08 u32 ead1
 *   0x0C u32 counterBias     (RTC bias)
 *   0x10 s8  displayOffsetH
 *   0x11 u8  ntd
 *   0x12 u8  language
 *   0x13 u8  flags
 *   0x14.. OSSramEx (flash ids, wireless ids, dvd error, gbs) — zeroed here.
 *
 * Checksum (dolphin SDK decomp OSRtc.c, UnlockSram commit path):
 *   checkSum = checkSumInv = 0;
 *   for (p = &counterBias; p < &sram[sizeof(OSSram)]; p++) {  // 0x0C..0x13
 *       checkSum    += *p;      // *p is a BIG-ENDIAN u16 on the guest
 *       checkSumInv += ~*p;
 *   }
 * i.e. four BE16 words at 0x0C, 0x0E, 0x10, 0x12.
 * ------------------------------------------------------------------------- */

u16 gcn_sram_checksum(const u8 sram[GCN_SRAM_SIZE], u16* inv) {
    u16 sum = 0;
    u16 sum_inv = 0;
    for (u32 off = GCN_SRAM_CKSUM_START; off < GCN_SRAM_OSSRAM_SIZE; off += 2) {
        u16 word = read_be16(sram + off);   /* [YAGCD] guest reads BE16 */
        sum = (u16)(sum + word);
        sum_inv = (u16)(sum_inv + (u16)~word);
    }
    if (inv) *inv = sum_inv;
    return sum;
}

bool gcn_sram_validate(const u8 sram[GCN_SRAM_SIZE]) {
    u16 inv = 0;
    u16 sum = gcn_sram_checksum(sram, &inv);
    u16 stored_sum = read_be16(sram + 0x00);
    u16 stored_inv = read_be16(sram + 0x02);
    return sum == stored_sum && inv == stored_inv;
}

void gcn_seed_build_sram(u8 sram[GCN_SRAM_SIZE], const GcnSeedConfig* cfg) {
    memset(sram, 0, GCN_SRAM_SIZE);   /* [DEFAULT] OSSramEx region stays zero */

    /* [FIXTURE] ead0/ead1 = 0: no broadband adapter / flash id programmed. */
    write_be32(sram + 0x04, 0);
    write_be32(sram + 0x08, 0);

    /* [FIXTURE] counterBias: added to the RTC to form wall-clock time. */
    write_be32(sram + 0x0C, cfg->counter_bias);

    /* [FIXTURE] displayOffsetH / ntd. */
    sram[0x10] = (u8)cfg->display_offset_h;
    sram[0x11] = 0;

    /* [FIXTURE] language (OSRtc.h OS_LANGUAGE_*). */
    sram[0x12] = cfg->language;

    /* [FIXTURE] flags byte, assembled from the documented bits (OSRtc.c). */
    u8 flags = 0;
    flags |= (u8)(cfg->video_mode & GCN_SRAM_FLAG_VIDEO_MASK);
    if (cfg->sound_stereo) flags |= GCN_SRAM_FLAG_SOUND_STEREO;
    if (cfg->progressive)  flags |= GCN_SRAM_FLAG_PROGRESSIVE;
    sram[0x13] = flags;

    /* [YAGCD] recompute + store the checksum pair (BE16 at 0x00 / 0x02) so the
     * IPL accepts the SRAM instead of resetting it to defaults. */
    u16 inv = 0;
    u16 sum = gcn_sram_checksum(sram, &inv);
    write_be16(sram + 0x00, sum);
    write_be16(sram + 0x02, inv);
}

/* ---------------------------------------------------------------------------
 * Config defaults
 * ------------------------------------------------------------------------- */

void gcn_seed_default_config(GcnSeedConfig* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->payload = NULL;
    cfg->payload_size = 0;
    cfg->load_addr = GCN_BS2_LOAD_ADDR;              /* [ROM] */
    cfg->entry_pc = GCN_BS2_ENTRY_PC;                /* [ROM] */
    cfg->mem1_fill = GCN_SEED_MEM1_FILL;             /* [DEFAULT] */
    cfg->language = GCN_SRAM_LANG_ENGLISH;           /* [FIXTURE] */
    cfg->sound_stereo = 1;                           /* [FIXTURE] stereo */
    cfg->progressive = 0;                            /* [FIXTURE] off */
    cfg->video_mode = 0;                             /* [FIXTURE] NTSC */
    cfg->display_offset_h = 0;                       /* [FIXTURE] */
    cfg->counter_bias = 0;                           /* [FIXTURE] */
    cfg->rtc_counter = GCN_SEED_RTC_COUNTER;         /* [FIXTURE] 2004-01-01 */
}

/* ---------------------------------------------------------------------------
 * Seed application
 * ------------------------------------------------------------------------- */

bool gcn_seed_apply(CPUState* cpu, GcnSeedDevices* devices, const GcnSeedConfig* cfg) {
    if (!cpu || !cfg || !cfg->payload) {
        fprintf(stderr, "gcn seed: null cpu/config/payload\n");
        return false;
    }
    if (cfg->payload_size != GCN_BS2_USA_SIZE) {
        fprintf(stderr,
                "gcn seed: payload size 0x%X != expected BS2 size 0x%X\n",
                cfg->payload_size, GCN_BS2_USA_SIZE);
        return false;
    }

    /* Reset the register file + zero MEM1 (keeps device callbacks intact). */
    cpu_reset(cpu);

    /* [DEFAULT] Establish the deterministic MEM1 reset image, THEN drop the
     * payload on top. BS2Init will clear low memory + set the physical-memory
     * size / console type itself, so we deliberately seed nothing else in
     * low-mem (DESIGN.md §6). */
    gcn_mem_fill_mem1(cpu, cfg->mem1_fill);

    /* [ROM] Load the descrambled BS2 image at its real target address. */
    if (!gcn_mem_load_blob(cpu, cfg->load_addr, cfg->payload, cfg->payload_size))
        return false;

    /* [ROM] Enter at the true IPL start routine. */
    cpu->pc = cfg->entry_pc;

    /* ---- CPU latches ----
     * These are the only genuinely Dolphin-adjacent unknowns. Dolphin VERIFIES
     * them; it never feeds them (DESIGN.md §6). What we can justify from docs we
     * set with provenance; the rest are [DEBT] to be pinned from BS1 disasm in
     * M1 (real BS1 will set them itself, at which point these seeds go away).
     */

    /* [YAGCD] PVR (SPR 287) reads as the Gekko id. cpu_init already set this;
     * cpu_reset restored it. Asserted here for clarity. */
    cpu->spr[287] = PPC_GEKKO_PVR;

    /* [DEBT] HID2: the IPL menu's rolling-cube scene uses paired singles, which
     * require HID2[PSE] (and HID2[LSQE] for non-indexed psq). The real BS1/BS2
     * sets HID2 via mtspr before any ps op; we pre-enable PSE|LSQE so that if we
     * enter slightly past that mtspr the ps path still works. FLAGGED: confirm
     * the exact HID2 value BS2 writes from BS1/BS2 disasm in M1. */
    cpu->hid2 = PPC_HID2_PSE | PPC_HID2_LSQE;

    /* [DEBT] MSR: at BS2 entry the machine runs supervisor, FP-enabled, with
     * translation configured by BS1's BAT setup. We set FP + ME as a documented
     * best-effort; EE/IR/DR/IP and the BAT/SR contents are NOT yet derived and
     * are the primary M1 debt. FLAGGED. */
    cpu->msr = 0x00002000u;   /* FP (bit 18) on; everything else left at reset 0 */

    /* [DEBT] BATs (SPR 528..543), SRs (sr[0..15]), GQRs (gqr[0..7]) are left at
     * reset 0. BS1 programs the BATs and the SDK programs GQR0 for standard
     * float quantisation; deriving the exact values is M1 work. FLAGGED. */

    cpu->cycles = 0;
    cpu->halted = false;
    cpu->halt_reason = GCN_HALT_NONE;

    /* ---- device fixtures ---- */
    if (devices) {
        memset(devices, 0, sizeof(*devices));
        gcn_seed_build_sram(devices->sram, cfg);       /* [FIXTURE] valid SRAM */
        devices->rtc_counter = cfg->rtc_counter;       /* [FIXTURE] RTC epoch */
    }

    return true;
}

bool gcn_seed_read_file(const char* path, u8** out_data, u32* out_size) {
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0;
    if (!path || !out_data || !out_size) return false;

    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "gcn seed: cannot open '%s'\n", path);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long size = ftell(f);
    if (size <= 0) { fclose(f); return false; }
    rewind(f);

    u8* buf = (u8*)malloc((size_t)size);
    if (!buf) { fclose(f); return false; }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        fprintf(stderr, "gcn seed: short read on '%s' (%zu/%ld)\n", path, got, size);
        return false;
    }
    *out_data = buf;
    *out_size = (u32)size;
    return true;
}
