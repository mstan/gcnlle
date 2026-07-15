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
 *
 * Environment variables (all optional; unset = today's behavior, unchanged):
 *   GCN_IPL_ROM       full descrambled IPL dump backing the EXI mask-ROM
 *                      device (else the BS2 payload window is reused).
 *   GCN_DSP_ROM        DSP-LLE IROM dump (default bios/dsp_rom.bin).
 *   GCN_DSP_COEF       DSP-LLE coefficient dump (default bios/dsp_coef.bin).
 *   GCN_AUDIO          host playback for the real AID DMA stream. Defaults
 *                      ON with GCN_WINDOW=1 and OFF for headless runs; "1"
 *                      or "0" explicitly overrides that policy.
 *   GCN_GX_BACKEND     "software", "vulkan", or "vulkan-shadow". Interactive
 *                      GCN_WINDOW=1 runs default to the exact resident Vulkan
 *                      path; headless/oracle runs default to software.
 *   GCN_GL             host presenter policy for GCN_WINDOW=1. Default/unset
 *                      uses double-buffered WGL; "0" selects the GDI fallback.
 *   GCN_GL_VSYNC       WGL swap interval policy. Default/unset enables vsync;
 *                      "0" disables it for diagnostics.
 *   GCN_DEBUG_PORT     enable the TCP debug server on this port (see
 *                      docs/TCP_COMMANDS.md); with it set, gcn_boot runs
 *                      unbounded and parks on stop instead of exiting.
 *   GCN_MEM_DUMP       ';'-separated "<addr>:<len>:<path>" MEM1 dump specs,
 *                      taken after the run stops.
 *   GCN_SRAM_FILE      host-file backing for the EXI SRAM+RTC state (ROADMAP
 *                      M3 persistence). If the file exists and is exactly
 *                      GCN_EXI_PERSIST_FILE_SIZE bytes (0x44 — the real GC
 *                      "combined RTC+SRAM" blob, byte-identical to Dolphin's
 *                      `GC/SRAM.raw`: BE32 rtc counter then the 0x40-byte
 *                      OSSram/OSSramEx image, Core/HW/Sram.h `struct Sram`),
 *                      it OVERRIDES the seed.c fixture at boot. Every guest
 *                      RTC_WRITE/SRAM_WRITE (exi.c ipl_transfer) then flushes
 *                      the live state back to that path, and a final flush
 *                      runs at clean shutdown. Unset (the default): nothing
 *                      is read or written — fixtures only, byte-identical to
 *                      before this option existed.
 *   GCN_RTC_HOST       [ENHANCEMENT] "1" samples host local time ONCE at boot,
 *                      seeds the EXI RTC, then advances it only from emulated
 *                      Gekko cycles. Never set it for oracle-diff runs. It
 *                      wins over a persisted counter; SRAM settings still load.
 *   GCN_GX_PIPELINE    default-on CPU/GX execution overlap. "0" selects the
 *                      fully synchronous fallback for diagnostics. Both paths
 *                      retain the pinned XFB/oracle results. GXSetDrawDone
 *                      materializes completed XFBs before VI can flip to them;
 *                      debug RAM access, FIFO reset, and shutdown explicitly
 *                      join before observing or releasing produced state.
 *   GCN_DSP_THREAD     experimental DSP-core worker. Default is the exact,
 *                      synchronous LLE path; "1" opts into overlap whose DSP
 *                      interrupt publication can depend on host scheduling.
 *
 *   GCN_BOOT_BS1       [M1] "1" switches to the REAL BS1 boot: argv[1] is now
 *                      the RAW SCRAMBLED bios/ipl.bin (not a pre-descrambled
 *                      payload), the CPU is seeded at the TRUE hardware reset
 *                      vector (pc=0xFFF00100, msr=IP-only — seed.c
 *                      gcn_seed_apply_true_reset, no [DEBT] latch faked), and
 *                      the EXI mask-ROM + the CPU's 0xFFF00000 ROM window are
 *                      both backed by the SAME image, descrambled once, in
 *                      CPU-adjacent hardware (exi.c gcn_exi_set_rom_scrambled)
 *                      rather than offline. BS1's own recompiled code (linked
 *                      into the SAME dispatch table as the M0 default path —
 *                      docs/M1_PLAN.md §7 step 4/5) then does its own real
 *                      hardware bring-up and EXI DMA of BS2 into 0x81300000,
 *                      at which point execution falls through into the SAME
 *                      BS2 the M0 default path uses. Unset (default): today's
 *                      M0 behavior, byte-identical.
 *   GCN_BS1_REFERENCE  [M1] path to an independently-produced offline
 *                      descramble to CRC/byte-compare BS1's own real-DMA'd
 *                      MEM1 against (docs/M1_PLAN.md §8) — the M1 integrity
 *                      check, only meaningful with GCN_BOOT_BS1=1. Accepts
 *                      either the FULL descrambled image (tools/ipl_descramble
 *                      -o, BS2 at file offset 0x820) or the trimmed BS2-payload
 *                      slice (--bs2, BS2 at file offset 0x720) — detected by
 *                      size. Unset: the check is skipped with a loud notice
 *                      (never silently assumed to pass).
 */
#include "cpu/cpu.h"
#include "memory/memory.h"
#include "seed/seed.h"
#include "dispatch/dispatch.h"
#include "trace/trace.h"
#include "mmio/mmio.h"
#include "exi/exi.h"
#include "memcard/memcard_image.h"   /* M4: format/load/validate a card .raw */
#include "si/si.h"
#include "pi/pi.h"
#include "dsp/dsp.h"
#include "ai/ai.h"
#include "vi/vi.h"
#include "di/di.h"
#include "mi/mi.h"
#include "pe/pe.h"
#include "cp/cp.h"
#include "gp/gp.h"
#include "gx/gx.h"
#include "debug/rings.h"
#include "debug/debug_server.h"
#include "host/host_audio.h"   /* real AID DMA -> interactive WASAPI output */
#include "host/host_window.h"  /* GCN_WINDOW=1: opt-in native display window */
#include "util/crc32.h"
#include "descramble_core.h"   /* IPL_SCRAMBLE_END etc. — the M1 integrity check */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* M1: BS1's real EXI-DMA bulk-copy size, measured empirically (see boot.c's
 * GCN_BOOT_BS1 integrity-check comment below) — NOT docs/M1_PLAN.md §0's
 * IPL_SCRAMBLE_END-0x820=0x1AF6E0 estimate, which the real hardware disproves:
 * everything past this offset in the "descrambled" file is other data (later,
 * on-demand resources) BS1's own initial bulk copy never touches; both a
 * fresh true-reset BS1-at-ROM run and the pre-existing oracle-validated M0
 * pipeline's own post-DMA dump independently confirm this exact boundary. */
#define GCN_BS1_BS2_DMA_LEN 0x0016FFE0u

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

/* PE token/finish lines -> PI interrupt cause. The PE passes the PI cause bit
 * (GCN_PE_INT_TOKEN=0x200 / GCN_PE_INT_FINISH=0x400) as cause_mask, mirroring
 * Dolphin UpdateInterrupts' two independent SetInterrupt calls. */
static void pe_irq_to_pi(void* user, u32 cause_mask, int level) {
    gcn_pi_set_interrupt((GcnPi*)user, cause_mask, level);
}

/* CP watermark/breakpoint interrupt line -> PI interrupt cause. */
static void cp_irq_to_pi(void* user, int level) {
    gcn_pi_set_interrupt((GcnPi*)user, GCN_PI_INT_CP, level);
}

/* GCN_SRAM_FILE write-back (ROADMAP M3): exi.c calls this synchronously right
 * after a guest RTC_WRITE/SRAM_WRITE lands, so the host file always reflects
 * the live device state a moment after the guest commits it — no batching or
 * shutdown-only flush invented here (that would silently lose state on a
 * crash/kill). The context is a static (outlives main's stack frame the same
 * way the device models below do) so the callback needs no heap alloc. */
typedef struct { GcnExi* exi; const char* path; } GcnSramPersistCtx;
static GcnSramPersistCtx s_sram_persist_ctx;

static void sram_persist_to_file(void* user) {
    GcnSramPersistCtx* ctx = (GcnSramPersistCtx*)user;
    if (!gcn_exi_persist_save(ctx->path, ctx->exi->rtc.counter, ctx->exi->sram))
        fprintf(stderr, "gcn boot: WARNING — failed to write GCN_SRAM_FILE '%s'\n",
                ctx->path);
}

/* ROADMAP M4: memory cards. Slot A (channel 0 dev0) and slot B (channel 1 dev0)
 * are backed by a Dolphin-compatible flat .raw image. GCN_MEMCARD_A/B name a
 * host file:
 *   - exists + validates            -> loaded as-is.
 *   - exists but invalid/corrupt    -> reformatted blank (Dolphin-style
 *                                      header/directory/BAT) and overwritten.
 *   - does not exist yet [auto-create] -> a FACTORY-BLANK image (all 0xFF,
 *     no header — real erased-flash state) is created at that path and
 *     mounted; the IPL's own card manager is left to notice it's unformatted
 *     and offer to format it, same as a real blank card in a real console
 *     (see setup_memcard's "!existed" branch). Sized from
 *     GCN_MEMCARD_A_MBITS / GCN_MEMCARD_B_MBITS (4/8/16/32/64/128 real Mbit;
 *     default 128 Mbit / 16 MiB, matching the pre-existing formatted-blank
 *     default below).
 * With no env set, slot A still gets an in-memory formatted card so EXT reads
 * 1 (matching the Dolphin oracle's "card in slot A" config — this preserves
 * the validated boot), and slot B stays empty. Every guest write is flushed
 * back to the file on chip deselect (card_persist_to_file). */
typedef struct { const char* path[2]; } GcnCardPersistCtx;
static GcnCardPersistCtx s_card_persist_ctx;

static void card_persist_to_file(void* user, u32 ch, GcnMemcard* card) {
    GcnCardPersistCtx* ctx = (GcnCardPersistCtx*)user;
    if (ch < 2u && ctx->path[ch]) {
        if (gcn_mc_image_save_file(ctx->path[ch], card->data, card->size_bytes))
            card->dirty = 0;
        else
            fprintf(stderr, "gcn boot: WARNING — failed to persist memory card "
                    "%c to '%s'\n", (char)('A' + ch), ctx->path[ch]);
    } else {
        card->dirty = 0;   /* in-memory card: writes live only in RAM */
    }
}

/* Replicate Dolphin's SetCardFlashID (oracle: Core/HW/Sram.cpp:50-76). The
 * console records each inserted card's "unlock identity" — a 12-byte flash id
 * derived from the card's own header serial (the LCG inverse of the format-time
 * keystream that produced the serial) — plus a one-byte checksum, in OSSramEx.
 *
 * This is load-bearing for the mount: the IPL's __CARDMountAsync, seeing a card
 * whose ReadStatus reports already-unlocked (status bit 0x40, our 0xC1), does NOT
 * re-run the unlock handshake — it TRUSTS the console-side SRAM record. It reads
 * OSSramEx.flashID[slot], sums the 12 bytes, and requires
 * flashIDCheckSum[slot] == (u8)~sum, else returns CARD_RESULT_IOERROR ("the
 * object inserted cannot be used") BEFORE it ever issues SetInterrupt/ReadArray.
 * A zero-filled SRAM (flashID all 0 -> ~sum == 0xFF) with a zero checksum fails
 * that gate — which is exactly why our card never mounted while Dolphin's did
 * (Dolphin runs SetCardFlashID from the memcard ctor; we didn't). Diagnosed with
 * an independent SDK trace (session ChatGPT collab) and confirmed against
 * Sram.cpp + EXI_DeviceMemoryCard.cpp:143-146.
 *
 * OSSramEx layout in our 0x40-byte SRAM image (== Dolphin settings+settings_ex,
 * Sram.h): flash_id[2][12] at 0x14 / 0x20, flash_id_checksum[2] at 0x3A / 0x3B.
 * These live in settings_ex, OUTSIDE the settings checksum's range
 * (FixSRAMChecksums sums settings.rtc_bias..settings_ex), so writing them here
 * does not perturb the SRAM the IPL already validated. `header20` is the card's
 * first 20 bytes (serial[0..11] + format_time[12..19]). */
static void set_card_flash_id(GcnExi* exi, u32 ch, const u8* header20) {
    if (ch >= 2u) return;
    u8* flash_id = exi->sram + 0x14u + ch * 12u;
    u64 rand = 0;                                  /* swap64(&header20[12]) BE */
    for (int i = 0; i < 8; i++) rand = (rand << 8) | header20[12 + i];
    u8 csum = 0;
    for (int i = 0; i < 12; i++) {
        rand = ((rand * 0x41c64e6dULL) + 0x3039ULL) >> 16;
        flash_id[i] = (u8)(header20[i] - ((u8)rand & 0xFFu));
        csum = (u8)(csum + flash_id[i]);
        rand = ((rand * 0x41c64e6dULL) + 0x3039ULL) >> 16;
        rand &= 0x7FFFULL;
    }
    exi->sram[0x3Au + ch] = (u8)(csum ^ 0xFFu);
}

/* ROADMAP M4 [auto-create]: GCN_MEMCARD_A_MBITS / GCN_MEMCARD_B_MBITS pick the
 * size of a brand-new factory-blank card image (see setup_memcard's
 * "!existed" branch). Values are real GameCube Mbit (4/8/16/32/64/128 — the
 * six sizes GCMemcard::Open() accepts, memcard_image.h GCN_MC_MBIT_*).
 * Unset or unrecognized falls back to 128 Mbit, the same size the
 * pre-existing "path given, file missing/invalid" fallback below already
 * defaults to — this knob only widens what's selectable, it never changes
 * the default. Grepped first (PRINCIPLES "do not invent a second knob"): no
 * earlier GCN_MEMCARD_*_MBITS-shaped env var existed in this tree. */
static u32 memcard_mbits_bytes_from_env(const char* env_name) {
    unsigned code = GCN_MC_MBIT_2043;   /* default: 128 Mbit, 16 MiB */
    const char* e = getenv(env_name);
    if (e && *e) {
        unsigned long want = strtoul(e, NULL, 0);
        switch (want) {
        case 4:   code = GCN_MC_MBIT_59;   break;
        case 8:   code = GCN_MC_MBIT_123;  break;
        case 16:  code = GCN_MC_MBIT_251;  break;
        case 32:  code = GCN_MC_MBIT_507;  break;
        case 64:  code = GCN_MC_MBIT_1019; break;
        case 128: code = GCN_MC_MBIT_2043; break;
        default:
            fprintf(stderr, "gcn boot: %s=%s is not one of the six real "
                    "GameCube card sizes (4/8/16/32/64/128 Mbit) — using the "
                    "default 128 Mbit\n", env_name, e);
            break;
        }
    }
    return gcn_mc_image_bytes_for_mbits(code);
}

/* Install a formatted/loaded memory card on `ch`. `path` (may be NULL) is the
 * .raw backing. Returns the malloc'd card image (owned by `card` after
 * gcn_memcard_init) or NULL on hard failure. */
/* [auto-create/reformat] Mint a FACTORY-BLANK card image — all 0xFF, the real
 * erased-flash state, NO header/directory/BAT. Deliberately does NOT call
 * gcn_mc_image_format(): writing a valid header is the IPL's OWN card
 * manager's job the first time it sees an unformatted card (LLE-first —
 * PRINCIPLES.md "the firmware does it", not us). A real GameCube shows "this
 * card is unformatted, format it?" for a brand-new card AND for one whose
 * header no longer validates; both cases route here so the observable flow
 * is the same single one. Returns the malloc'd image (persisted to `path`)
 * or NULL on alloc/write failure — the caller's last-resort in-memory
 * pre-formatted fallback covers that. */
static u8* mint_factory_blank(u32 ch, const char* path, u32* out_sz) {
    char env_name[24];
    u32 blank_sz;
    u8* blank;
    snprintf(env_name, sizeof env_name, "GCN_MEMCARD_%c_MBITS", (char)('A' + ch));
    blank_sz = memcard_mbits_bytes_from_env(env_name);
    blank = (u8*)malloc(blank_sz);
    if (!blank) return NULL;
    memset(blank, 0xFF, blank_sz);
    if (!gcn_mc_image_save_file(path, blank, blank_sz)) {
        fprintf(stderr, "gcn boot: memory card %c: failed to write "
                "factory-blank image to '%s' — falling back to an "
                "in-memory pre-formatted card\n", (char)('A' + ch), path);
        free(blank);
        return NULL;
    }
    fprintf(stdout, "gcn boot: created factory-blank memory card at '%s' (%u Mbit)\n",
            path, blank_sz / 131072u);   /* 131072 B/Mbit (GCN_MC_BLOCK_SIZE * GCN_MC_BLOCKS_PER_MBIT) */
    *out_sz = blank_sz;
    return blank;
}

static void setup_memcard(GcnExi* exi, u32 ch, GcnMemcard* card, const char* path) {
    /* Default to 128 Mbit / 2043-block (GetCardId 0x80) — the size Dolphin's
     * oracle boots with, so the card's NintendoID response matches the oracle
     * trace byte-for-byte (verified: oracle DATA read = 0x00000080). A .raw
     * backing keeps its own on-file size. */
    u32 sz = gcn_mc_image_bytes_for_mbits(GCN_MC_MBIT_2043);  /* 128 Mbit, 16 MiB */
    u8* img = NULL;

    if (path && *path) {
        /* Existence probe BEFORE gcn_seed_read_file: a path that has never
         * existed (auto-create a factory-blank card) and a path that exists
         * but is short/corrupt/unreadable (reformat-in-place, pre-existing
         * behavior below) are different real-hardware scenarios, and
         * gcn_seed_read_file's plain bool return doesn't distinguish them. */
        FILE* probe = fopen(path, "rb");
        int existed = probe != NULL;
        if (probe) fclose(probe);

        if (existed) {
            u8* file = NULL; u32 file_sz = 0;
            if (gcn_seed_read_file(path, &file, &file_sz)) {
                char err[128];
                if (gcn_mc_image_valid_size(file_sz) &&
                    gcn_mc_image_check(file, file_sz, err, sizeof err)) {
                    img = file; sz = file_sz;
                    fprintf(stdout, "gcn boot: memory card %c loaded from '%s' (%u bytes)\n",
                            (char)('A' + ch), path, file_sz);
                } else {
                    /* Corrupt/invalid existing file: same real-hardware
                     * scenario as a brand-new card (the console can't read a
                     * valid header either way), so it takes the SAME path —
                     * mint factory-blank, let the IPL offer to format.
                     * Previously this silently pre-formatted in the runtime
                     * (the bottom fallback), which a real console never does. */
                    fprintf(stdout, "gcn boot: memory card %c file '%s' invalid (%s) — "
                            "minting factory-blank; the IPL will offer to format it\n",
                            (char)('A' + ch), path,
                            gcn_mc_image_valid_size(file_sz) ? err : "bad size");
                    free(file);
                    img = mint_factory_blank(ch, path, &sz);
                }
            } else {
                /* gcn_seed_read_file itself failed (zero-length/unreadable):
                 * also "no valid card here" — same unified mint path. */
                img = mint_factory_blank(ch, path, &sz);
            }
        } else {
            /* Path names a card that has never existed. */
            img = mint_factory_blank(ch, path, &sz);
        }
    }

    if (!img) {
        /* Reached only when no path was given at all (the documented fixture
         * convenience keeping slot A's oracle-matched default boot working)
         * or when mint_factory_blank couldn't allocate/persist. Every
         * "a path was given but no valid card is there" case goes through
         * the unified mint-and-let-the-IPL-format path above. */
        img = (u8*)malloc(sz);
        if (!img) { fprintf(stderr, "gcn boot: memory card %c alloc failed\n",
                            (char)('A' + ch)); return; }
        /* flash_id/bias/language/time feed only the (never-validated) header
         * serial; zeros produce a card the IPL accepts. */
        u8 flash_id[12] = {0};
        gcn_mc_image_format(img, sz, flash_id, 0u, 0u, 0ull);
        if (path && *path) {
            gcn_mc_image_save_file(path, img, sz);
            fprintf(stdout, "gcn boot: memory card %c formatted blank -> '%s' (%u bytes)\n",
                    (char)('A' + ch), path, sz);
        } else {
            fprintf(stdout, "gcn boot: memory card %c formatted blank (in-memory, "
                    "%u bytes)\n", (char)('A' + ch), sz);
        }
    }

    gcn_memcard_init(card, img, sz);   /* takes ownership of img */
    s_card_persist_ctx.path[ch] = (path && *path) ? path : NULL;
    gcn_exi_set_memcard(exi, ch, card);

    /* Record this card's flash id + checksum in OSSramEx so the IPL's mount gate
     * (which trusts the console-side unlock identity for an already-unlocked card)
     * passes instead of returning IOERROR. See set_card_flash_id above. Uses the
     * card's first 20 header bytes (serial + format_time). */
    set_card_flash_id(exi, ch, img);
}

int main(int argc, char** argv) {
    const char* bs1_env = getenv("GCN_BOOT_BS1");
    int bs1_mode = bs1_env && *bs1_env && *bs1_env != '0';

    if (argc < 2) {
        fprintf(stderr,
            "gcnrecomp boot (M0/M1 execution)\n"
            "usage: %s <descrambled_bs2.bin | raw_ipl.bin> [max_blocks]\n"
            "  Default (GCN_BOOT_BS1 unset): seeds the BS2 payload + M0 declarative\n"
            "  seed, then runs recompiled blocks from 0x81200150 until it stops.\n"
            "  GCN_BOOT_BS1=1: <arg1> is the RAW SCRAMBLED bios/ipl.bin; seeds the\n"
            "  TRUE hardware reset vector (0xFFF00100) and runs real BS1 -> its own\n"
            "  EXI DMA of BS2 -> the same BS2 the default path uses. See the file\n"
            "  header comment for every GCN_* env var. Default budget: 200000 blocks.\n",
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

    /* GcnSeedConfig still supplies the device-fixture (SRAM/RTC) defaults in
     * BOTH modes — those are independent EXI peripherals, not part of the CPU
     * true-reset debt (seed.h / gcn_seed_apply_true_reset doc comment). */
    GcnSeedConfig cfg;
    gcn_seed_default_config(&cfg);
    cfg.payload = payload;
    cfg.payload_size = size;

    GcnSeedDevices devices;
    if (bs1_mode) {
        if (size < IPL_SCRAMBLE_END) {
            fprintf(stderr,
                "gcn boot: GCN_BOOT_BS1=1 requires the RAW SCRAMBLED bios/ipl.bin "
                "(>= 0x%X bytes), got '%s' (0x%X bytes)\n",
                IPL_SCRAMBLE_END, argv[1], size);
            cpu_free(&cpu);
            free(payload);
            return 1;
        }
        gcn_seed_apply_true_reset(&cpu);
        gcn_seed_build_devices(&devices, &cfg);
    } else if (!gcn_seed_apply(&cpu, &devices, &cfg)) {
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

    /* EXI mask-ROM backing.
     *
     * M1 (GCN_BOOT_BS1=1): argv[1] IS the raw scrambled bios/ipl.bin (already
     * loaded into `payload`/`size` above). Descramble it ONCE here (owned by
     * `exi`) and back BOTH the EXI device AND the CPU's 0xFFF00000 ROM window
     * (memory.c) from the SAME returned buffer — one descrambled image, two
     * consumers, exactly the split docs/M1_PLAN.md §6.2 calls for.
     *
     * M0 default (unchanged): prefer a full descrambled IPL image via
     * GCN_IPL_ROM (offset 0 = ROM offset 0); otherwise reuse the descrambled
     * BS2 payload already loaded, whose byte k IS ROM offset (load_off + k).
     * The IPL font/ROM DMA reads offsets 0x800..0x170800, all within that
     * slice, so both backings serve identical bytes. */
    if (bs1_mode) {
        const u8* descrambled = gcn_exi_set_rom_scrambled(&exi, payload, size);
        if (!descrambled) {
            fprintf(stderr, "gcn boot: failed to descramble '%s' for EXI\n", argv[1]);
            cpu_free(&cpu); free(payload); return 1;
        }
        gcn_mem_set_rom_window(&cpu, descrambled, size);
        fprintf(stdout,
            "gcn boot: GCN_BOOT_BS1 — EXI mask-ROM + 0xFFF00000 CPU ROM window "
            "backed by transparently-descrambled %s (%u bytes)\n", argv[1], size);
    } else {
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
    }
    gcn_exi_set_sram(&exi, devices.sram);
    gcn_exi_set_rtc(&exi, devices.rtc_counter);

    /* ROADMAP M3: optional host-file backing for the SRAM+RTC state, so a
     * date/language/sound change made through the Calendar/Options UI
     * survives a process restart. Unset (default): none of this runs, and
     * behavior is byte-identical to before this feature existed — fixtures
     * only, nothing touches disk. */
    const char* sram_path = getenv("GCN_SRAM_FILE");
    if (sram_path && *sram_path) {
        u32 loaded_rtc = 0;
        u8 loaded_sram[GCN_SRAM_SIZE_BYTES];
        if (gcn_exi_persist_load(sram_path, &loaded_rtc, loaded_sram)) {
            gcn_exi_set_sram(&exi, loaded_sram);
            gcn_exi_set_rtc(&exi, loaded_rtc);
            fprintf(stdout,
                    "gcn boot: GCN_SRAM_FILE '%s' loaded — overriding the seed "
                    "fixture (persisted RTC=%u)\n", sram_path, loaded_rtc);
        } else {
            fprintf(stdout,
                    "gcn boot: GCN_SRAM_FILE '%s' not found/invalid — using the "
                    "seed fixture (will be created on the first guest write or "
                    "at clean shutdown)\n", sram_path);
        }
        s_sram_persist_ctx.exi = &exi;
        s_sram_persist_ctx.path = sram_path;
        gcn_exi_set_persist(&exi, sram_persist_to_file, &s_sram_persist_ctx);
    }

    /* [ENHANCEMENT, opt-in] GCN_RTC_HOST=1: sample host local time exactly
     * once. The EXI RTC then advances from emulated cycles, never by reading
     * the host clock again. Applied after persistence load so the boot sync
     * wins while SRAM settings remain intact. */
    const char* rtc_host = getenv("GCN_RTC_HOST");
    if (rtc_host && *rtc_host && *rtc_host != '0') {
        u32 sampled_rtc = gcn_exi_sync_rtc_from_host(&exi, cpu.cycles);
        fprintf(stdout, "gcn boot: GCN_RTC_HOST — sampled host local time once "
                "at boot (RTC=%u); subsequent time is emulated\n", sampled_rtc);
    }

    /* ROADMAP M4: memory cards. Slot A defaults to a formatted card (EXT=1,
     * matching the oracle); GCN_MEMCARD_A/B back either slot with a host .raw. */
    static GcnMemcard card_a, card_b;
    gcn_exi_set_card_persist(&exi, card_persist_to_file, &s_card_persist_ctx);
    setup_memcard(&exi, 0u, &card_a, getenv("GCN_MEMCARD_A"));   /* slot A (default on) */
    const char* mcb_path = getenv("GCN_MEMCARD_B");
    if (mcb_path && *mcb_path)
        setup_memcard(&exi, 1u, &card_b, mcb_path);              /* slot B (opt-in) */

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
    if (gcn_host_audio_start(32000u))
        gcn_dsp_set_audio_sink(&dsp, gcn_host_audio_submit_be_s16, NULL);
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
    /* GCN_WINDOW=1 (opt-in): lets vi.c hand the host window (host_window.c)
     * the live XFB bytes at each field boundary via the guest RAM pointer.
     * No effect unless GCN_WINDOW is set — see gcn_vi_set_cpu's doc comment. */
    gcn_vi_set_cpu(&vi, &cpu);
    /* The SI poll is scheduled off the VI beam (VideoInterface.cpp:950-968);
     * wire vi's per-halfline hook straight to si.c's beam-poll entry point so
     * vi.c never takes a dependency on si.h (same pattern as PI_FIFO_RESET's
     * hook into gp.c). */
    gcn_vi_set_si_poll_hook(&vi, gcn_si_beam_poll, &si);
    /* GCN_THROTTLE=1 (opt-in): pace emulated time to wall-clock time at each
     * VI field boundary. Matches GcnViFieldFn's signature exactly (see
     * dispatch.h), so no trampoline is needed here — same pattern as the
     * irq_to_pi trampolines above, minus the trampoline. No effect unless
     * GCN_THROTTLE is set (gcn_dispatch_throttle_on_field's own cached-getenv
     * gate; see dispatch.c). */
    gcn_vi_set_field_hook(&vi, gcn_dispatch_throttle_on_field, NULL);
    gcn_mmio_register(&bus, "VI", GCN_VI_BASE, GCN_VI_SIZE,
                      gcn_vi_read, gcn_vi_write, &vi);

    /* DI: the GameCube DVD drive. gcn_di_init always starts from the no-disc
     * state (mirrors DVDInterface::Init's own ASSERT(!IsDiscInside()) — see
     * di.h). With no disc the menu kicks a DVDReadDiskID (DICMDBUF0=
     * 0xA8000040, DICR=3) and polls DICVR every frame; the read completes with
     * DEINT and DICVR bit 0 (cover open) reads 1, so the menu concludes "no
     * disc" and proceeds. The drive interrupt line asserts INT_CAUSE_DI in the
     * PI cause word. The PI_RESET_CODE hook (below) is wired unconditionally —
     * with no disc it's a byte-identical no-op (see di.h/pi.h) but it's the
     * real hardware path a mounted disc uses to reach DiscIdNotRead. */
    static GcnDi di;
    gcn_di_init(&di);
    gcn_di_set_irq(&di, di_irq_to_pi, &pi);
    gcn_mmio_register(&bus, "DI", GCN_DI_BASE, GCN_DI_SIZE,
                      gcn_di_read, gcn_di_write, &di);
    gcn_pi_set_reset_drive_hook(&pi, gcn_di_reset_drive_spinup);

    /* ROADMAP M5: GCN_DISC=path mounts a disc image at boot, read-only,
     * pread-style (the image is NOT read into RAM — di.c seeks+freads exactly
     * the bytes each DI read command asks for; see di.h). Mirrors Dolphin's
     * Boot.cpp SetDisc() call for "--boot-gc-ipl --disc", which runs AFTER
     * DVDInterface::Init() — hence gcn_di_init() above always runs first, from
     * the no-disc state, exactly like the oracle. Unset (default): no mount,
     * byte-identical to before GCN_DISC existed — every existing golden
     * depends on this (cover open, no disc). */
    const char* disc_path = getenv("GCN_DISC");
    if (disc_path && *disc_path) {
        if (gcn_di_set_disc(&di, disc_path))
            fprintf(stdout, "gcn boot: GCN_DISC — mounted '%s'\n", disc_path);
        else
            fprintf(stderr,
                "gcn boot: GCN_DISC='%s' failed to mount — continuing with no disc\n",
                disc_path);
    }

    /* MI: pure u16 register file (memory-controller face). No interrupts this
     * increment; the IPL's observed traffic is a single MI_IRQMASK write. */
    static GcnMi mi;
    gcn_mi_init(&mi);
    gcn_mmio_register(&bus, "MI", GCN_MI_BASE, GCN_MI_SIZE,
                      gcn_mi_read, gcn_mi_write, &mi);

    /* PE: pixel-engine config + token/finish interrupt latches. The GX-init
     * path reads CTRL back at 0xCC00100A. The two PE->PI lines stay dormant
     * (no FIFO parser raises token/finish; the menu masks them). */
    static GcnPe pe;
    gcn_pe_init(&pe);
    gcn_pe_set_irq(&pe, pe_irq_to_pi, &pi);
    gcn_mmio_register(&bus, "PE", GCN_PE_BASE, GCN_PE_SIZE,
                      gcn_pe_read, gcn_pe_write, &pe);

    /* CP: GX command-FIFO front-end. STATUS is computed from live pointer
     * state; the CPU-side watermark evaluation drives INT_CAUSE_CP (the line
     * the menu unmasks at 0x9FC once GX init completes). */
    static GcnCp cp;
    gcn_cp_init(&cp);
    gcn_cp_set_irq(&cp, cp_irq_to_pi, &pi);
    gcn_mmio_register(&bus, "CP", GCN_CP_BASE, GCN_CP_SIZE,
                      gcn_cp_read, gcn_cp_write, &cp);

    /* GP: the 32-byte gather pipe. Each full burst DMAs to guest RAM at the PI
     * FIFO write pointer, advances it (wrap END->BASE), runs the CP burst
     * handler, and is recorded into the always-on FIFO ring. Needs PI + CP. */
    static GcnGp gp;
    gcn_gp_init(&gp, &pi, &cp);
    gcn_mmio_register(&bus, "GP", GCN_GP_BASE, GCN_GP_SIZE,
                      gcn_gp_read, gcn_gp_write, &gp);
    /* PI_FIFO_RESET (GXAbortFrame) resets the gather-pipe staging via this hook
     * (keeps pi.c free of a gp.h dependency). */
    gcn_pi_set_fifo_reset_hook(&pi, gcn_gp_reset);

    /* GX FIFO consumer: device-less, ticked from the dispatch loop. Drains the
     * guest-RAM FIFO ring gp.c fills, decodes + executes commands (CP/XF/BP
     * state, display lists, EFB copy-clear), advancing the CP read pointer. It
     * needs the guest RAM (FIFO/DL/XFB access), the CP register file, and the PE
     * (SETDRAWDONE/PE_TOKEN commands raise its finish/token latches). */
    gcn_gx_init(&cpu, &cp, &pe);

    gcn_mmio_install(&bus, &cpu);

    gcn_trace_init();  /* emits a RUNTIME trace to $GCN_TRACE_OUT if set */

    /* Observability: bring up the always-on ring buffers and (if GCN_DEBUG_PORT
     * is set) the TCP debug query surface over them + live CPU/RAM state. */
    gcn_rings_init();
    GcnDebugCtx dbg = { .cpu = &cpu, .di = &di, .exi = &exi, .dsp = &dsp };
    int dbg_port = gcn_debug_server_start(&dbg);

    /* GCN_WINDOW=1 (opt-in): native Win32 display + keyboard-pad window,
     * replacing tools/gcn_viewer.py's TCP/PPM/Tk loop for interactive use
     * (host/host_window.h). Started here (mirrors the debug-server start
     * just above) so it is up before the dispatch loop runs; a no-op when
     * GCN_WINDOW is unset. */
    gcn_host_window_start(&cpu);

    /* With the debug server or the host window on, run unbounded and stop
     * only on an explicit quit signal (TCP "quit" / window closed), so the
     * process (and its always-on rings) stay inspectable/interactive rather
     * than racing to the block budget. */
    u32 run_blocks = (dbg_port > 0 || gcn_host_window_enabled()) ? 0u : max_blocks;

    /* M1: arm the handoff-instant snapshot the integrity check compares (see
     * dispatch.h — end-of-run memory is useless once BS2 has executed). */
    if (bs1_mode) {
        const char* ref_env = getenv("GCN_BS1_REFERENCE");
        if (ref_env && *ref_env)
            gcn_dispatch_arm_bs1_snapshot(GCN_BS1_BS2_DMA_LEN);
    }

    fprintf(stdout,
        "gcn boot: seeded; entering recompiled %s at 0x%08X (budget %u blocks)\n"
        "--- execution (unmapped-MMIO warnings below are the M0 signal) ---\n",
        bs1_mode ? "BS1 (true reset)" : "BS2",
        cpu.pc, dbg_port > 0 ? 0u : max_blocks);
    fflush(stdout);

    int still_live = gcn_dispatch_run(&cpu, run_blocks);
    /* G3: retire and join the GX worker before anything below (debug park,
     * trace close, GCN_MEM_DUMP, XFB hashes, RAM teardown) can observe or
     * release its state. No-op when the synchronous path is selected. */
    gcn_gx_pipeline_shutdown();
    /* Don't park waiting on a TCP "quit" that may never come if what actually
     * ended the run was the host window closing (GCN_WINDOW=1, no debug
     * client attached). */
    if (dbg_port > 0 && !gcn_debug_server_quit_requested() && !gcn_host_window_quit_requested())
        gcn_debug_server_park();
    gcn_debug_server_stop();
    gcn_trace_close();

    const char* reason =
        gcn_host_window_quit_requested() ? "host window closed"
        : still_live ? "block budget reached (still live — likely busy-waiting on unmodeled HW)"
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

    /* M1 integrity check (docs/M1_PLAN.md §8): did BS1's own real EXI DMA,
     * through the transparent in-CPU/EXI descrambler, reproduce the SAME
     * bytes an independent offline descramble produces? Loud pass/fail,
     * never silent — and skipped (loudly) rather than assumed if no
     * reference was configured. This is the M1 headline; the M0 default path
     * never runs this block. */
    if (bs1_mode) {
        const char* ref_path = getenv("GCN_BS1_REFERENCE");
        if (!ref_path || !*ref_path) {
            fprintf(stdout,
                "gcn boot: GCN_BS1_REFERENCE not set — INTEGRITY CHECK SKIPPED "
                "(not a pass; set it to an offline `ipl_descramble` output to "
                "actually validate the real EXI DMA)\n");
        } else {
            u8* ref = NULL; u32 ref_size = 0;
            if (!gcn_seed_read_file(ref_path, &ref, &ref_size)) {
                fprintf(stderr,
                    "gcn boot: GCN_BS1_REFERENCE '%s' unreadable — INTEGRITY CHECK "
                    "SKIPPED (not a pass)\n", ref_path);
            } else {
                /* Accept either the FULL descrambled image (`-o`, BS2 at file
                 * offset 0x820) or the trimmed BS2-payload slice (`--bs2`,
                 * BS2 at file offset 0x720 — the same bytes, minus the
                 * [0,0x100) copyright header) — distinguished by size. */
                u32 ref_bs2_off = (ref_size >= IPL_SCRAMBLE_END) ? 0x820u : 0x720u;
                /* GCN_BS1_BS2_DMA_LEN (not IPL_SCRAMBLE_END-0x820=0x1AF6E0, the
                 * M1_PLAN.md §0 estimate — that span also covers later on-demand
                 * resources BS1's own bulk copy never touches). Empirically
                 * measured this session: BOTH a fresh true-reset BS1-at-ROM run
                 * and the pre-existing oracle-validated M0 pipeline's own
                 * post-DMA dump agree byte-for-byte with the offline reference
                 * up to exactly this length and NEVER beyond it (all-zero
                 * tail — BS1 simply never writes there). Two independent real
                 * EXI-DMA runs landing on the identical boundary is strong
                 * confirmation this is BS1's true, fixed DMA size, not an
                 * artifact of either run. */
                /* Compare the HANDOFF-INSTANT snapshot (dispatch.h), never the
                 * end-of-run memory: BS2's own .data/.bss live inside the
                 * DMA'd span, so any post-handoff execution mutates it and a
                 * longer block budget would turn a true PASS into a false
                 * FAIL (caught in independent re-verification of the first
                 * implementation, which compared at process exit and only
                 * passed with a budget that stopped at the jump). */
                u32 have_ref = (ref_size > ref_bs2_off) ? (ref_size - ref_bs2_off) : 0u;
                u32 snap_len = 0;
                const u8* snap = gcn_dispatch_bs1_snapshot(&snap_len);
                u32 len = snap_len;
                if (len > have_ref) len = have_ref;

                if (!snap || len == 0) {
                    fprintf(stderr,
                        "gcn boot: INTEGRITY CHECK FAIL — execution never reached "
                        "the BS1->BS2 handoff (pc 0x81300000), or the reference is "
                        "too short (snapshot=%u ref_avail=%u)\n", snap_len, have_ref);
                } else {
                    u32 mem_crc = gcn_crc32(snap, len);
                    u32 ref_crc = gcn_crc32(ref + ref_bs2_off, len);
                    int match = (mem_crc == ref_crc) &&
                                (memcmp(snap, ref + ref_bs2_off, len) == 0);
                    fprintf(stdout,
                        "\n--- M1 INTEGRITY CHECK: handoff snapshot MEM1[0x81300000,+0x%X) "
                        "vs %s[0x%X,+0x%X) ---\n"
                        "  runtime CRC32 : 0x%08X\n"
                        "  reference CRC32: 0x%08X\n"
                        "  RESULT         : %s\n",
                        len, ref_path, ref_bs2_off, len, mem_crc, ref_crc,
                        match ? "PASS -- byte-identical (real BS1 EXI DMA reproduces "
                                "the offline descramble exactly)"
                              : "FAIL -- MISMATCH (see docs/M1_PLAN.md section 8)");
                }
            }
            free(ref);
        }
    }

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

    /* ROADMAP M3: final flush at clean shutdown. Every guest write already
     * flushes immediately (exi.c's persist hook), so this is a safety-net
     * no-op in the common case — it only matters if gcn_exi_set_persist was
     * never wired to a per-write flush for some future caller, or as cheap
     * insurance against this function's own flush having failed earlier. */
    gcn_exi_rtc_latch(&exi, cpu.cycles);
    if (sram_path && *sram_path)
        sram_persist_to_file(&s_sram_persist_ctx);

    /* M4: safety-net flush (every write already flushed on chip-deselect), then
     * release the card images. gcn_memcard_free on a never-installed slot B is a
     * safe free(NULL). */
    if (card_a.dirty) card_persist_to_file(&s_card_persist_ctx, 0u, &card_a);
    if (card_b.dirty) card_persist_to_file(&s_card_persist_ctx, 1u, &card_b);
    gcn_memcard_free(&card_a);
    gcn_memcard_free(&card_b);

    gcn_host_audio_stop();
    gcn_dsp_free(&dsp);
    gcn_exi_free(&exi);   /* no-op unless GCN_BOOT_BS1 owned a descrambled buffer */
    gcn_di_free(&di);     /* no-op unless GCN_DISC mounted an image */
    cpu_free(&cpu);
    free(payload);
    return 0;
}
