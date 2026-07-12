/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gcnrecomp runtime — EXI (External Interface) device model.
 *
 * EXI is the GameCube's serial expansion bus: 3 channels, each with up to 3
 * chip-selectable devices. It carries the mask ROM (font / IPL data), the RTC
 * (date/time), the SRAM (persisted settings), the memory cards, and the AD16
 * dev-hardware probe. BS2 hits it immediately after PI/SI init.
 *
 * PROVENANCE
 *   The per-channel REGISTER model (CSR write-1-to-clear + CLK/CS/mask fields,
 *   CR TSTART/DMA/RW/TLEN decode, DMA MAR/LEN, IMM data) is adapted from
 *   GXRuntime's GPL-3.0 src/exi.c + include/dolruntime/exi.h, whose bit layout
 *   matches YAGCD chapter 5 and — verified here — the Dolphin oracle's CSR/CR
 *   read-backs byte-for-byte (e.g. ch2 CSR 0xBA -> read 0xB0; ch0 CSR 0x150 ->
 *   read 0x1950). Two corrections vs GXRuntime, both forced by the oracle:
 *     (1) channels 0/1 init with EXT set (a device is present), not EXTINT
 *         alone — the oracle reads 0x1808/0x1958 which require EXT (bit 12).
 *     (2) GXRuntime never wires a transfer callback (it HLEs the boot), so the
 *         mask-ROM / RTC / SRAM command decode below is NEW, written from YAGCD
 *         and validated against the oracle's ROM-read DMA stream.
 *
 * The device command decode (mask-ROM read = addr<<6, RTC/SRAM = 0x20000000 /
 * 0x20000100 read, 0xA000.. write) is our own, per YAGCD. AD16 and the memory
 * cards are present-but-empty stubs for now (flagged); the oracle does not
 * exercise memcard traffic in the M0 window, and AD16 correctly reads 0 on a
 * retail console.
 */
#ifndef GCN_EXI_EXI_H
#define GCN_EXI_EXI_H

#include "cpu/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GCN_EXI_BASE            0xCC006800u
#define GCN_EXI_CHANNELS        3u
#define GCN_EXI_CHANNEL_STRIDE  0x14u
#define GCN_EXI_REGISTER_BYTES  (GCN_EXI_CHANNELS * GCN_EXI_CHANNEL_STRIDE)  /* 0x3C */

/* Per-channel register offsets. */
#define GCN_EXI_CSR_OFF   0x00u  /* channel parameter / status register        */
#define GCN_EXI_MAR_OFF   0x04u  /* DMA main-memory address                    */
#define GCN_EXI_LEN_OFF   0x08u  /* DMA length                                 */
#define GCN_EXI_CR_OFF    0x0Cu  /* control register (transfer trigger)        */
#define GCN_EXI_DATA_OFF  0x10u  /* immediate data                             */

/* CSR bit fields (YAGCD ch.5 / GXRuntime). */
#define GCN_EXI_CSR_EXIINTMASK  0x00000001u
#define GCN_EXI_CSR_EXIINT      0x00000002u  /* w1c */
#define GCN_EXI_CSR_TCINTMASK   0x00000004u
#define GCN_EXI_CSR_TCINT       0x00000008u  /* w1c, set on transfer complete */
#define GCN_EXI_CSR_CLK_MASK    0x00000070u
#define GCN_EXI_CSR_CS_MASK     0x00000380u  /* one-hot chip select, bits 7-9 */
#define GCN_EXI_CSR_CS_DEV0     0x00000080u  /* chip-select field = 1 (device 0) */
#define GCN_EXI_CSR_EXTINTMASK  0x00000400u
#define GCN_EXI_CSR_EXTINT      0x00000800u  /* w1c, ch0/1 only */
#define GCN_EXI_CSR_EXT         0x00001000u  /* device present (read-only, on read) */
#define GCN_EXI_CSR_ROMDIS      0x00002000u  /* ch0 only: unmap internal ROM */

/* CR bit fields. */
#define GCN_EXI_CR_TSTART       0x00000001u
#define GCN_EXI_CR_DMA          0x00000002u
#define GCN_EXI_CR_RW_MASK      0x0000000Cu  /* 0=read 1=write 2=read/write */
#define GCN_EXI_CR_TLEN_MASK    0x00000030u  /* immediate length-1 (0..3) */

/* SRAM image size EXI serves for a full SRAM read (0x40 bytes). */
#define GCN_SRAM_SIZE_BYTES 0x40u

typedef struct {
    u32 csr;
    u32 mar;
    u32 len;
    u32 cr;
    u32 data;
} GcnExiChannel;

/* Pending device operation latched from the command word of a transaction. */
enum {
    GCN_EXI_OP_NONE = 0,
    GCN_EXI_OP_ROM_READ,
    GCN_EXI_OP_RTC_READ,
    GCN_EXI_OP_RTC_WRITE,
    GCN_EXI_OP_SRAM_READ,
    GCN_EXI_OP_SRAM_WRITE,
};

/* Level change on the EXI->PI interrupt line (level: 1 assert, 0 deassert). */
typedef void (*GcnExiIrqFn)(void* user, int level);

/* Fired synchronously whenever a guest RTC_WRITE or SRAM_WRITE transaction
 * mutates exi->rtc_counter / exi->sram (ROADMAP M3 persistence). The runtime
 * host (boot.c) hooks this to flush the combined RTC+SRAM blob to the
 * GCN_SRAM_FILE path, mirroring the "write straight through" semantics real
 * SRAM/RTC hardware has (no host-side batching invented here). NULL by
 * default: zero overhead / byte-identical behavior when no file is configured. */
typedef void (*GcnExiPersistFn)(void* user);

typedef struct GcnExi {
    GcnExiChannel channels[GCN_EXI_CHANNELS];

    /* IPL device backing (channel 0, chip-select bit 1 = device index 1). */
    const u8* rom;        /* descrambled mask-ROM image (borrowed pointer) */
    u32       rom_size;
    u32       rom_base;   /* ROM offset that maps to rom[0] (0, or 0x100 when
                             backed by the BS2 payload slice) */
    u8*       owned_rom;  /* M1: non-NULL iff gcn_exi_set_rom_scrambled malloc'd
                             `rom` itself (the descrambled-in-place copy); freed
                             by gcn_exi_free. NULL for the borrowed-pointer
                             paths (gcn_exi_set_rom), which are unowned as
                             before this field existed. */
    u8        sram[GCN_SRAM_SIZE_BYTES];
    u32       rtc_counter;

    /* [ENHANCEMENT, opt-in] Host-clock RTC (GCN_RTC_HOST=1): RTC reads return
     * the live host wall clock converted to the GC epoch — seconds since
     * 2000-01-01 00:00:00 UTC, i.e. unix_time - 0x386D4380 (Dolphin
     * EXI_DeviceIPL.h:27 GC_EPOCH; its GetEmulatedTime does this same
     * conversion) — plus rtc_host_offset, which an RTC_WRITE adjusts so the
     * guest can still "set the clock" relative to host time (we never touch
     * the host clock). DEFAULT OFF: reads serve the deterministic fixture
     * counter, byte-identical to before — host mode is a per-run convenience
     * on top of the validated baseline (PRINCIPLES: enhancements never
     * replace it) and NOT usable for oracle-diff runs (host time is
     * inherently nondeterministic). In host mode the persisted blob's rtc
     * field is written on flush but IGNORED on load (host time wins). */
    int       rtc_host_mode;
    s32       rtc_host_offset;

    /* Per-channel transaction state (reset when a device is (re)selected). */
    int       op[GCN_EXI_CHANNELS];
    u32       rom_offset[GCN_EXI_CHANNELS];  /* current byte offset for ROM read */
    u32       dev_pos[GCN_EXI_CHANNELS];     /* byte position within RTC/SRAM */
    u8        prev_cs[GCN_EXI_CHANNELS];     /* last chip-select, for select edge */
    int       have_cmd[GCN_EXI_CHANNELS];    /* command word latched this xact */

    /* EXT (device-present) is computed on each CSR read from the presence of the
     * device at chip-select 1 (Dolphin: GetDevice(1)->IsPresent()), not latched.
     * This flag is that presence per channel: ch0 = a device occupies slot A (as
     * in the Dolphin oracle's config, EXT reads 1), ch1 = slot B empty (EXT 0),
     * ch2 = forced 0. When memory-card modelling lands (M4), this becomes the
     * live card-inserted state; until then a menu card probe diverges loudly. */
    u8        dev_present[GCN_EXI_CHANNELS];

    int         irq_level;   /* last EXI->PI line level (edge detect for the ring) */
    GcnExiIrqFn irq;         /* sink for the EXI interrupt line (boot.c -> PI) */
    void*       irq_user;

    GcnExiPersistFn persist;      /* NULL unless GCN_SRAM_FILE is configured */
    void*           persist_user;
} GcnExi;

/* Initialise all channels (reset CSR: ch0 EXTINT; ch1 EXTINT|CS=1; ch2 idle;
 * EXT is computed on read, never latched). */
void gcn_exi_init(GcnExi* exi);

/* Register the EXI interrupt-line sink (boot.c trampoline into PI INT_CAUSE_EXI). */
void gcn_exi_set_irq(GcnExi* exi, GcnExiIrqFn fn, void* user);

/* Point the IPL/mask-ROM device at its backing image. `base` is the ROM byte
 * offset that `rom[0]` corresponds to (0 for a full 2 MB descrambled image;
 * 0x100 when reusing the descrambled BS2 payload as the backing window). */
void gcn_exi_set_rom(GcnExi* exi, const u8* rom, u32 size, u32 base);

/* M1: back the mask-ROM device with a RAW SCRAMBLED image (bios/ipl.bin as
 * dumped, no offline pre-descramble) — the real reset-vector boot path
 * (docs/M1_PLAN.md §4/§6.2). Mallocs an OWNED `raw_size`-byte copy, applies
 * segher's descrambler ONCE, in place, over the documented body range
 * [IPL_SCRAMBLE_START, IPL_SCRAMBLE_END) (tools/ipl_descramble/
 * descramble_core.c — the vendored transcription this calls verbatim, not
 * reimplemented here per PRINCIPLES "one algorithm, every call site"), then
 * wires it up exactly like gcn_exi_set_rom(exi, owned, raw_size, 0).
 *
 * Precomputing the whole buffer once is mathematically identical to a
 * faithful per-read hardware descramble (the keystream is a pure stateless
 * function of byte offset — docs/M1_PLAN.md §6.2) and is NOT a fake-the-
 * answer shortcut: the real EXI CSR/MAR/LEN transaction protocol still drives
 * every byte served (ipl_rom_read below is untouched).
 *
 * Returns the descrambled buffer (borrowed; owned by `exi`, valid until
 * gcn_exi_free) so the SAME image can also back the CPU's 0xFFF00000 ROM
 * window (memory.c gcn_mem_set_rom_window) — one descrambled image, two
 * consumers. Returns NULL (no state changed) if `raw` is NULL, `raw_size` is
 * smaller than the documented scrambled range, or the allocation fails. */
const u8* gcn_exi_set_rom_scrambled(GcnExi* exi, const u8* raw, u32 raw_size);

/* Frees the buffer owned by a prior gcn_exi_set_rom_scrambled call (no-op if
 * none, or if the device is currently backed by a borrowed gcn_exi_set_rom
 * pointer instead). Safe to call unconditionally at shutdown. */
void gcn_exi_free(GcnExi* exi);

/* Install the SRAM fixture (0x40 bytes) and RTC seconds-since-2000 counter. */
void gcn_exi_set_sram(GcnExi* exi, const u8 sram[GCN_SRAM_SIZE_BYTES]);
void gcn_exi_set_rtc(GcnExi* exi, u32 rtc_counter);

/* [ENHANCEMENT] Enable host-clock RTC mode (see the struct field doc). */
void gcn_exi_set_rtc_host_mode(GcnExi* exi, int on);

/* Register the write-back hook (ROADMAP M3): called synchronously right after
 * a guest RTC_WRITE or SRAM_WRITE lands in exi->rtc_counter / exi->sram. NULL
 * (the gcn_exi_init default) means no persistence — every write path stays
 * exactly as it is today. */
void gcn_exi_set_persist(GcnExi* exi, GcnExiPersistFn fn, void* user);

/* ---- host-file persistence (ROADMAP M3: GCN_SRAM_FILE) --------------------
 * On-disk layout is the real GC "combined RTC+SRAM" blob, byte-identical to
 * Dolphin's raw `GC/SRAM.raw` dump (Core/HW/Sram.h `struct Sram`: a
 * big-endian u32 rtc counter immediately followed by the 0x40-byte
 * OSSram/OSSramEx settings image) — 0x44 bytes total. Using the same layout
 * means a file written by this runtime is byte-for-byte what Dolphin itself
 * would write/read, and vice versa. */
#define GCN_EXI_PERSIST_FILE_SIZE (4u + GCN_SRAM_SIZE_BYTES)

/* Load `path` (exactly GCN_EXI_PERSIST_FILE_SIZE bytes: BE32 rtc, then the
 * 0x40-byte SRAM image) into *out_rtc / out_sram. Returns false (out params
 * left untouched) if the file does not exist, cannot be opened, or is not
 * exactly that size — the caller's fixture stays authoritative, matching the
 * documented "no file yet -> fixture, byte-identical to today" contract. */
bool gcn_exi_persist_load(const char* path, u32* out_rtc,
                           u8 out_sram[GCN_SRAM_SIZE_BYTES]);

/* Write `path` atomically-enough for a single-process debug tool (truncate +
 * full write) as BE32 rtc followed by the 0x40-byte SRAM image. Returns false
 * on any I/O failure (caller logs; never fatal — persistence is optional). */
bool gcn_exi_persist_save(const char* path, u32 rtc,
                           const u8 sram[GCN_SRAM_SIZE_BYTES]);

/* MMIO device-dispatch entry points (registered on the MMIO bus for
 * [GCN_EXI_BASE, GCN_EXI_BASE + GCN_EXI_REGISTER_BYTES)). */
u32  gcn_exi_read(void* user, CPUState* cpu, u32 addr, u8 size);
void gcn_exi_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size);

#ifdef __cplusplus
}
#endif

#endif /* GCN_EXI_EXI_H */
