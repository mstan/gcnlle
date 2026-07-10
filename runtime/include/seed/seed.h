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

/* [ROM] Guest load address AND entry PC of the descrambled BS2 payload. The
 * real IPL descrambles/copies BS2 to 0x81300000 and jumps there; we load the
 * offline-descrambled payload at the same address and enter at its start. */
#define GCN_BS2_LOAD_ADDR   0x81300000u
#define GCN_BS2_ENTRY_PC    0x81300000u

/* [ROM] Exact size of the USA BS2 payload (0x1AF6E0 bytes). A loaded payload of
 * any other size is a mismatch the seed rejects. */
#define GCN_BS2_USA_SIZE    0x001AF6E0u

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

/* Convenience: read a raw descrambled BS2 file into a malloc'd buffer.
 * Caller frees. */
bool gcn_seed_read_file(const char* path, u8** out_data, u32* out_size);

#ifdef __cplusplus
}
#endif

#endif /* GCN_SEED_SEED_H */
