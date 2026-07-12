/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gcnrecomp runtime — M0 declarative seed contract.
 *
 * The M0 seed boots offline-descrambled BS2 (from the real IPL) at the true IPL
 * start address with a PROVENANCE FOR EVERY FIELD and no Dolphin RAM snapshot as
 * input (DESIGN.md §6, ROADMAP.md M0). This header is the contract; seed.c
 * carries the per-field provenance comments. Provenance tags used:
 *
 *   [ROM]      byte-for-byte from the user's real IPL dump (the BS2 payload)
 *   [YAGCD]    GameCube hardware/firmware doc or SDK decomp (OSRtc.c etc.)
 *   [FIXTURE]  a deterministic value we choose (SRAM settings, RTC epoch)
 *   [DEFAULT]  runtime default / reset pattern
 *   [DEBT]     a CPU latch we cannot yet derive from BS1 disasm — FLAGGED, not
 *              silently snapshotted (DESIGN.md §6: Dolphin verifies, never feeds)
 */
#ifndef GCN_SEED_SEED_H
#define GCN_SEED_SEED_H

#include "cpu/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- fixed contract constants (see seed.c for provenance) ---- */

/* [ORACLE] Guest load address and entry PC of the descrambled BS2 payload,
 * corrected against Dolphin (2026-07-09): the descrambled body (from file
 * offset 0x100) loads at 0x81200000 and boot begins at 0x81200150. The earlier
 * 0x81300000 was wrong — it placed the image 0x100000 too high, so every
 * absolute pointer missed and BS2 derailed into an `sc` almost immediately. */
#define GCN_BS2_LOAD_ADDR   0x81200000u
#define GCN_BS2_ENTRY_PC    0x81200150u

/* [ROM] Exact size of the USA BS2 payload = descrambled [0x100, 0x1AFF00) =
 * 0x1AFE00 bytes (oracle-corrected: the code image begins at file offset 0x100,
 * after the copyright header, not 0x820). A payload of any other size is a
 * mismatch the seed rejects. */
#define GCN_BS2_USA_SIZE    0x001AFE00u

/* [DEFAULT] MEM1 reset fill. 0x00000000 matches Dolphin's zero-initialised
 * MEM1, which keeps the oracle low-mem diff clean; flip to a poison pattern
 * (e.g. 0xDEADBEEF) to hunt uninitialised reads. BS2Init clears low memory
 * itself, so almost nothing here is read before being written. */
#define GCN_SEED_MEM1_FILL  0x00000000u

/* [FIXTURE] RTC epoch: GameCube RTC counts seconds since 2000-01-01 00:00:00.
 * We fix boot time to 2004-01-01 00:00:00 UTC = 1461 days * 86400 s. */
#define GCN_RTC_EPOCH_2000_BASE  0u          /* the RTC's own zero point */
#define GCN_SEED_RTC_COUNTER     126230400u  /* 2004-01-01 00:00:00 */

/* [YAGCD] SRAM geometry: EXI reads 0x40 bytes; OSSram occupies the first 0x14,
 * OSSramEx the rest. The checksum (OSRtc.c UnlockSram) covers OSSram bytes
 * 0x0C..0x13 only. */
#define GCN_SRAM_SIZE            0x40u
#define GCN_SRAM_OSSRAM_SIZE     0x14u
#define GCN_SRAM_CKSUM_START     0x0Cu   /* &OSSram.counterBias */

/* SRAM flags byte (offset 0x13) bit meanings, from OSRtc.c getters/setters. */
#define GCN_SRAM_FLAG_VIDEO_MASK   0x03u  /* 0=NTSC 1=PAL 2=MPAL (OSGetVideoMode, clamped <3) */
#define GCN_SRAM_FLAG_SOUND_STEREO 0x04u  /* OSGetSoundMode: (flags&4)?stereo:mono */
#define GCN_SRAM_FLAG_PROGRESSIVE  0x80u  /* OSGetProgressiveMode: (flags&0x80)>>7 */

/* Language byte values (offset 0x12), from OSRtc.h OS_LANGUAGE_*. */
#define GCN_SRAM_LANG_ENGLISH   0u

/* [ROM] M1: the TRUE hardware reset vector, not the Dolphin-HLE landing point
 * GCN_BS2_ENTRY_PC above. gc-forever Bootrom wiki / ogamespec gc-ipl (cited
 * docs/M1_PLAN.md §2) + cpu_glue.c's exception_vector_address (already routes
 * here when MSR[IP]=1): system-reset vector 0x100, prefixed by IP -> 0xFFF00100. */
#define GCN_RESET_ENTRY_PC  0xFFF00100u

/* [ROM] Textbook PowerPC hard-reset MSR: only IP (bit 25, "exception prefix",
 * mask 0x00000040) is set; IR/DR/EE/ME/FP/... all start clear. BS1's own
 * recompiled code (not a seed fake) programs HID0/BATs/SRs and turns on
 * IR|DR itself (docs/M1_PLAN.md §3.1) — that is the entire point of M1: what
 * GCN_BS2's seed marks [DEBT] below, real BS1 execution now supplies. */
#define GCN_RESET_MSR       0x00000040u

/* ---- device fixtures the seed installs (consumed by the future EXI model) ---- */
typedef struct {
    u8  sram[GCN_SRAM_SIZE];   /* [FIXTURE] valid-checksum SRAM image */
    u32 rtc_counter;           /* [FIXTURE] EXI RTC seconds-since-2000 */
} GcnSeedDevices;

/* ---- knobs (all defaulted; gcn_seed_default_config fills them) ---- */
typedef struct {
    const u8* payload;      /* [ROM] descrambled BS2 bytes (borrowed pointer) */
    u32       payload_size; /* [ROM] must equal GCN_BS2_USA_SIZE */
    u32       load_addr;    /* [ROM] default GCN_BS2_LOAD_ADDR */
    u32       entry_pc;     /* [ROM] default GCN_BS2_ENTRY_PC */
    u32       mem1_fill;    /* [DEFAULT] default GCN_SEED_MEM1_FILL */

    /* SRAM settings (baked into the fixture with a recomputed checksum). */
    u8  language;           /* [FIXTURE] default English */
    u8  sound_stereo;       /* [FIXTURE] 1=stereo (default), 0=mono */
    u8  progressive;        /* [FIXTURE] 0=off (default) */
    u8  video_mode;         /* [FIXTURE] 0=NTSC (default) */
    s8  display_offset_h;   /* [FIXTURE] default 0 */
    u32 counter_bias;       /* [FIXTURE] RTC bias; default 0 */
    u32 rtc_counter;        /* [FIXTURE] default GCN_SEED_RTC_COUNTER */
} GcnSeedConfig;

/* Fill cfg with all documented defaults. Caller still sets payload/payload_size. */
void gcn_seed_default_config(GcnSeedConfig* cfg);

/* Build a 0x40-byte SRAM image from settings, computing and storing a valid
 * checksum/checksumInv (OSRtc.c algorithm). */
void gcn_seed_build_sram(u8 sram[GCN_SRAM_SIZE], const GcnSeedConfig* cfg);

/* Compute the OSRtc.c SRAM checksum pair over OSSram[0x0C..0x13].
 * Returns the checksum; *inv (if non-NULL) receives checksumInv. */
u16  gcn_sram_checksum(const u8 sram[GCN_SRAM_SIZE], u16* inv);

/* True iff the stored checksum/checksumInv (BE16 at 0x00/0x02) match a fresh
 * recomputation — i.e. the SRAM the IPL would accept without resetting it. */
bool gcn_sram_validate(const u8 sram[GCN_SRAM_SIZE]);

/* Apply the full seed: reset the CPU, fill MEM1, load the payload, set entry PC
 * and the (flagged) CPU latches, and populate the device fixtures. Returns false
 * on any inconsistency (bad size, load overflow). Does NOT execute PPC. */
bool gcn_seed_apply(CPUState* cpu, GcnSeedDevices* devices, const GcnSeedConfig* cfg);

/* Populate the device fixtures only (SRAM + RTC) — the same [FIXTURE] logic
 * gcn_seed_apply uses internally, factored out so gcn_seed_apply_true_reset
 * (M1) can share it: SRAM/RTC are independent EXI peripherals whose content
 * is genuinely arbitrary regardless of which CPU boot path is seeded, so both
 * paths install the identical documented fixture. */
void gcn_seed_build_devices(GcnSeedDevices* devices, const GcnSeedConfig* cfg);

/* [M1] Apply ONLY the true-reset CPU seed: reset the CPU, fill MEM1 (same
 * [DEFAULT] pattern as gcn_seed_apply, for the same reason — nothing reads it
 * before BS1/BS2Init writes it), set pc=GCN_RESET_ENTRY_PC and
 * msr=GCN_RESET_MSR, and leave EVERY OTHER register at cpu_reset()'s zeroed
 * state. Unlike gcn_seed_apply this loads NO payload into RAM and sets NO
 * [DEBT] latch (HID2/BATs/SRs/GQRs) — real BS1 execution programs those
 * itself (docs/M1_PLAN.md §3.1), which is the entire M1 deliverable. Caller
 * still wires the EXI ROM device (gcn_exi_set_rom_scrambled) and the CPU ROM
 * window (gcn_mem_set_rom_window) separately; this function only touches CPU
 * register state. */
void gcn_seed_apply_true_reset(CPUState* cpu);

/* Convenience: read a raw descrambled BS2 file into a malloc'd buffer.
 * Caller frees. */
bool gcn_seed_read_file(const char* path, u8** out_data, u32* out_size);

#ifdef __cplusplus
}
#endif

#endif /* GCN_SEED_SEED_H */
