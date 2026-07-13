/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Disc Interface (DI) model — the GameCube DVD drive, no-disc AND disc-present.
 *
 * The Flipper DI block (0xCC006000) is the CPU-side face of the MN102 drive
 * chip: a status/interrupt register, a cover register, a 3-word command buffer,
 * a DMA address/length pair, a DMA control register (the transfer trigger), an
 * immediate-data buffer and a read-only config register. Every register/bit and
 * every command response below is transcribed from Dolphin's DVDInterface — our
 * independent oracle — never invented:
 *   register map + bit layout : DVDInterface.cpp:73-82 (offsets),
 *                               DVDInterface.h:204-257 (UDISR/UDICVR/UDICR/UDICFG)
 *   power-on state             : DVDInterface.cpp Init:261-302 (DICVR.Hex=1,
 *                               DICFG.CONFIG=1) + ResetDrive:306-346
 *   MMIO read/write semantics : DVDInterface.cpp RegisterMMIO:547-630
 *   interrupt line            : UpdateInterrupts:632-642 + GenerateDIInterrupt:644-663
 *   command execution         : ExecuteCommand:782-1234, CheckReadPreconditions:706-738,
 *                               ExecuteReadCommand:741-777, FinishExecutingCommand:1306-1353
 *   drive state/error codes   : DVDInterface.h:70-101 (DriveState / DriveError)
 *   disc mount/eject          : DVDInterface.cpp SetDisc:385-431, IsDiscInside:433-436,
 *                               SetLidOpen:529-535, ResetDrive:306-346
 *   drive re-spin trigger     : ProcessorInterface.cpp:145-160 (PI_RESET_CODE
 *                               ComplexWrite; GC path: `~value & 0x4` calls
 *                               ResetDrive(true)) — wired from pi.c's
 *                               reset_drive_hook (see pi.h), NOT special-cased
 *                               here. This is the REAL mechanism (not a
 *                               mount-time hack) that advances a freshly-mounted
 *                               disc from DiscChangeDetected to DiscIdNotRead:
 *                               the IPL itself writes DICMDBUF-adjacent
 *                               PI_RESET_CODE=1 then =3 very early in stage-1
 *                               boot (pi.h's trace note), and 3 has bit 2 clear.
 * YAGCD chapter 5 (DI) names the registers DISR/DICVR/DICMDBUF/DIMAR/DILENGTH/
 * DICR/DIIMMBUF/DICFG, matching Dolphin.
 *
 * WHY THIS EXISTS NOW: the recompiled IPL menu writes DICMDBUF0=0xA8000040
 * (DVDReadDiskID) + DICR=3 (DMA|TSTART) and then polls DICVR every frame. With
 * no DI model those hit the unmapped fallback (read 0) and the completion
 * interrupt never arrives, so the menu's DVD state machine never concludes "no
 * disc" and never proceeds to render. This model gives the faithful no-disc
 * drive responses, AND (ROADMAP M5) a faithful disc-present drive once one is
 * mounted via GCN_DISC (boot.c) or the "insert_disc" debug command.
 *
 * MOUNT LIFECYCLE. gcn_di_init() ALWAYS starts from the no-disc state — mirrors
 * Dolphin's DVDInterface::Init(), which literally ASSERTs !IsDiscInside()
 * (DVDInterface.cpp:263): the console's own Init never has a disc yet. A disc
 * becomes present only via a LATER, separate gcn_di_set_disc() call (mirroring
 * Boot.cpp's SetDisc(), called after Load_BS2 for "--boot-gc-ipl --disc", or
 * DVDInterface::ChangeDisc for a runtime swap):
 *   - NO DISC (gcn_di_init default). DICVR.CVR powers up 1 == "cover open"
 *     (DVDInterface.cpp:268, "Disc Channel relies on cover being open when no
 *     disc is inserted" — this is the bit the menu's per-frame poll branches
 *     on). ResetDrive(false) with no disc => DriveState::CoverOpened
 *     (DVDInterface.cpp:326). Any read-like command fails CheckReadPreconditions
 *     (SetDriveError(MediumNotPresent)) and completes with DEINT, not TCINT
 *     (ExecuteReadCommand:745-750). RequestError then reports drive state
 *     (CoverOpened-1 = 1) and error MediumNotPresent (0x23a00) in DIIMMBUF, i.e.
 *     0x01023A00 (ExecuteCommand RequestError:982-994).
 *   - DISC MOUNTED (gcn_di_set_disc). SetLidOpen flips DICVR.CVR 1->0 (closed)
 *     and — since that's an edge — raises CVRINT (DVDInterface.cpp:529-535).
 *     ResetDrive(false) (SetDisc always passes spinup=false, DVDInterface.cpp:430)
 *     => DriveState::DiscChangeDetected (DVDInterface.cpp:332), NOT
 *     DiscIdNotRead yet. The state only advances to DiscIdNotRead when the
 *     drive re-spins (ResetDrive(true)) — see "drive re-spin trigger" above.
 *     Real disc reads (Read Sector / Read DiscID) now pread the mounted image
 *     at DVD offset (cmdbuf[1]<<2, DVDInterface.cpp:844) straight into guest
 *     RAM at DIMAR, bounds-clamped per Error #001 / BlockOOB semantics
 *     (ExecuteReadCommand:757-773).
 *   - EJECTED (gcn_di_eject_disc). Mirrors SetDisc(nullptr): SetLidOpen flips
 *     CVR back to 1 (open, CVRINT if it was closed) and ResetDrive(false) with
 *     no disc => CoverOpened again.
 *
 * COMPLETION MODEL: deferred to the next gcn_di_tick, NOT synchronous inside the
 * DICR write. Dolphin's ExecuteCommand runs the command effects immediately
 * (DIIMMBUF / memory writes / drive state+error / interrupt-type selection) but
 * defers the *completion* — clear TSTART, raise the interrupt — to a scheduled
 * FinishExecutingCommand event MINIMUM_COMMAND_LATENCY_US=300us later
 * (DVDInterface.cpp:1227-1233, FinishExecutingCommand:1306-1353). We mirror that
 * split: di_execute_command runs at the DICR write (it has CPUState* for the
 * Inquiry DMA and, now, the real disc-read DMA), latches the pending interrupt
 * type and leaves TSTART set; the next gcn_di_tick clears TSTART and fires the
 * interrupt. Deferring past the issuing block (rather than firing inside the
 * write) matches Dolphin's ordering — the guest sees DICR.TSTART=1 immediately
 * after its own write, then a later completion — and avoids delivering the DI
 * interrupt before the command-issue function has returned. Exact latency is
 * irrelevant to the value+order oracle diff (poll-collapsed); the load-bearing
 * property is "deferred, not in-write". Real disc reads are performed
 * SYNCHRONOUSLY inside di_execute_command too (a plain seek+fread of the
 * mounted image, not a background DVDThread) — command_handled_by_thread is
 * always false in this model, so completion still runs through the same
 * gcn_di_tick path as every other command; only the exact latency Dolphin's
 * DVDThread would add is not reproduced (irrelevant to the value+order diff,
 * same rationale as MINIMUM_COMMAND_LATENCY_US above).
 *
 * DELIBERATELY DEFERRED (diverges loudly if exercised, never silently faked):
 *   - DTK audio streaming DATA PATH (the ADPCM decoder / position tracking /
 *     the free-running CoreTiming DTK pump Init:289-290 schedules). The
 *     AudioStream/RequestAudioStatus/AudioBufferConfig COMMAND HANDSHAKE is
 *     modeled (enable_dtk tracked, correct NoAudioBuf/precondition errors),
 *     but no bytes ever actually stream — out of M5 scope (disc-load SCREEN
 *     only; DTK content belongs to a loaded game/apploader, which M5
 *     deliberately does not reach — see boot.c's null-apploader note).
 *   - The BREAK/abort path (DISR bit 0): Dolphin DEBUG_ASSERTs on it; the IPL
 *     boot path never sets it. We store the mask bits and note it.
 *   - StopMotor's software-eject / auto-disc-change side effect
 *     (ExecuteCommand:1122-1148 force_eject/auto-change branches) — multi-disc
 *     UX, irrelevant to the single dummy disc M5 mounts.
 *   - Triforce/AMMediaboard and all Wii-exclusive commands: not GameCube-retail.
 */
#ifndef GCN_DI_DI_H
#define GCN_DI_DI_H

#include "cpu/cpu.h"
#include <stdio.h>   /* FILE* — the mounted disc image handle */

#define GCN_DI_BASE  0xCC006000u
#define GCN_DI_SIZE  0x40u          /* DI register block; SI starts at 0xCC006400 */

/* Register offsets (DVDInterface.cpp:73-82, YAGCD names). */
#define GCN_DI_SR       0x00u       /* DISR:    status / interrupt              */
#define GCN_DI_CVR      0x04u       /* DICVR:   cover                           */
#define GCN_DI_CMDBUF0  0x08u       /* DICMDBUF0                                */
#define GCN_DI_CMDBUF1  0x0Cu       /* DICMDBUF1                                */
#define GCN_DI_CMDBUF2  0x10u       /* DICMDBUF2                                */
#define GCN_DI_MAR      0x14u       /* DIMAR:   DMA main-memory address         */
#define GCN_DI_LENGTH   0x18u       /* DILENGTH:DMA length                      */
#define GCN_DI_CR       0x1Cu       /* DICR:    DMA control (transfer trigger)  */
#define GCN_DI_IMMBUF   0x20u       /* DIIMMBUF:immediate data buffer           */
#define GCN_DI_CFG      0x24u       /* DICFG:   config (read-only)              */

/* DISR bits (DVDInterface.h:204-219). Status bits (DEINT/TCINT/BRKINT) are
 * write-1-to-clear; the *MASK bits are plain R/W enables. */
#define GCN_DI_SR_BREAK       0x00000001u  /* stop device + interrupt          */
#define GCN_DI_SR_DEINTMASK   0x00000002u
#define GCN_DI_SR_DEINT       0x00000004u  /* device-error interrupt   (W1C)   */
#define GCN_DI_SR_TCINTMASK   0x00000008u
#define GCN_DI_SR_TCINT       0x00000010u  /* transfer-complete int    (W1C)   */
#define GCN_DI_SR_BRKINTMASK  0x00000020u
#define GCN_DI_SR_BRKINT      0x00000040u  /* break-complete int       (W1C)   */

/* DICVR bits (DVDInterface.h:222-233). */
#define GCN_DI_CVR_CVR        0x00000001u  /* 0: cover closed  1: cover open   */
#define GCN_DI_CVR_CVRINTMASK 0x00000002u
#define GCN_DI_CVR_CVRINT     0x00000004u  /* cover interrupt          (W1C)   */

/* DICR bits (DVDInterface.h:236-245). Only the low 3 bits are writable. */
#define GCN_DI_CR_TSTART      0x00000001u  /* w1: start   r0: ready            */
#define GCN_DI_CR_DMA         0x00000002u  /* 1: DMA mode  0: immediate        */
#define GCN_DI_CR_RW          0x00000004u  /* 0: read (disc->mem) 1: write     */
#define GCN_DI_CR_MASK        0x00000007u

/* DICFG: read-only; Dolphin sets CONFIG=1 ("Disable bootrom descrambler",
 * DVDInterface.cpp:277). Reads return this constant; writes are invalid. */
#define GCN_DI_CFG_VALUE      0x00000001u

/* DMA register write masks (DVDInterface.cpp:610-613, GameCube variant). */
#define GCN_DI_MAR_MASK       (~0xFC00001Fu)
#define GCN_DI_LENGTH_MASK    (~0x0000001Fu)

/* Drive state (DVDInterface.h:70-79). RequestError reports Ready as 0, every
 * other state as (value - 1). */
enum {
    GCN_DI_STATE_READY               = 0,
    GCN_DI_STATE_READY_NO_READS_MADE = 1,
    GCN_DI_STATE_COVER_OPENED        = 2,
    GCN_DI_STATE_DISC_CHANGE         = 3,
    GCN_DI_STATE_NO_MEDIUM           = 4,
    GCN_DI_STATE_MOTOR_STOPPED       = 5,
    GCN_DI_STATE_DISC_ID_NOT_READ    = 6,
};

/* Drive error codes (DVDInterface.h:83-101). */
#define GCN_DI_ERR_NONE            0x00000u
#define GCN_DI_ERR_MOTOR_STOPPED   0x20400u
#define GCN_DI_ERR_NO_DISC_ID      0x20401u
#define GCN_DI_ERR_MEDIUM_NOT_PRESENT 0x23A00u  /* medium not present / cover opened */
#define GCN_DI_ERR_INVALID_COMMAND 0x52000u
#define GCN_DI_ERR_NO_AUDIO_BUF    0x52001u
#define GCN_DI_ERR_BLOCK_OOB       0x52100u  /* logical block addr OOB ("Error #001") */
#define GCN_DI_ERR_INVALID_AUDIO   0x52401u
#define GCN_DI_ERR_INVALID_PERIOD  0x52402u
#define GCN_DI_ERR_MEDIUM_CHANGED  0x62800u  /* disc changed / motor stopped by ResetDrive */

/* DI interrupt types (DVDInterface.h:103-109). */
enum {
    GCN_DI_INT_DEINT = 0,
    GCN_DI_INT_TCINT = 1,
    GCN_DI_INT_BRKINT = 2,
    GCN_DI_INT_CVRINT = 3,
};

/* GC disc format: the apploader ALWAYS begins at this FIXED disc offset,
 * regardless of title (Dolphin DiscIO/DiscUtils.h:37 APPLOADER_ADDRESS;
 * Core/Boot/Boot_BS2Emu.cpp:147-149 "The apploader is hardcoded to begin at
 * 0x2440 on the disc"). di.c fires a loud one-time note when a real disc read
 * lands this region in RAM — M5's SCOPE BOUNDARY (docs/ROADMAP.md M5): DI is
 * modeled faithfully up to and including that read landing in guest RAM; what
 * the IPL does with the apploader's CONTENTS (jump to its entry point, run
 * apploader code, load the game) is the next phase and is deliberately NOT
 * modeled here. The dummy disc's apploader entry point is 0 by construction
 * (tools/make_dummy_disc.py) — a real disc's apploader would take over from
 * here; ours can only diverge (loudly, via this note), never fake success. */
#define GCN_DI_APPLOADER_OFFSET 0x2440u

/* DICMDBUF0[31:24] command opcodes the GameCube IPL issues (DVDInterface.h:40-65). */
#define GCN_DI_CMD_INQUIRY       0x12u
#define GCN_DI_CMD_READ          0xA8u
#define GCN_DI_CMD_SEEK          0xABu
#define GCN_DI_CMD_REQUEST_ERROR 0xE0u
#define GCN_DI_CMD_AUDIO_STREAM  0xE1u
#define GCN_DI_CMD_AUDIO_STATUS  0xE2u
#define GCN_DI_CMD_STOP_MOTOR    0xE3u
#define GCN_DI_CMD_AUDIO_CONFIG  0xE4u

/* Level change on the DI->PI interrupt line (level: 1 assert, 0 deassert). */
typedef void (*GcnDiIrqFn)(void* user, int level);

typedef struct {
    u32 disr;            /* DISR   */
    u32 dicvr;           /* DICVR  */
    u32 cmdbuf[3];       /* DICMDBUF0..2 */
    u32 dimar;           /* DIMAR  */
    u32 dilength;        /* DILENGTH */
    u32 dicr;            /* DICR   */
    u32 diimmbuf;        /* DIIMMBUF */

    u8  drive_state;     /* GCN_DI_STATE_* */
    u32 error_code;      /* GCN_DI_ERR_*   */
    u8  enable_dtk;       /* AudioBufferConfig-set DTK enable (handshake only —
                            * see di.h's DTK note; ResetDrive:316 clears it */

    /* Deferred completion: a command runs its effects at the DICR write and
     * latches the interrupt type here; the next gcn_di_tick completes it. */
    u8  cmd_pending;
    u8  pending_intr;    /* GCN_DI_INT_* */

    int irq_level;       /* last DI->PI line level (edge detect for the ring) */
    GcnDiIrqFn irq;
    void*      irq_user;

    /* ROADMAP M5: the mounted disc image, if any. Opened read-only by
     * gcn_di_set_disc; borrowed by nobody else (di.c owns the handle end to
     * end — mount replaces it, eject/shutdown closes it). Never read into RAM:
     * every DI read command seeks+freads exactly the bytes it needs (pread-
     * style), matching a real drive that streams off the disc rather than
     * caching it. disc_present is the di_disc_inside() flip point. */
    FILE* disc_file;
    u64   disc_size;
    int   disc_present;
} GcnDi;

void gcn_di_init(GcnDi* di);
void gcn_di_set_irq(GcnDi* di, GcnDiIrqFn fn, void* user);
/* Complete any command latched by the previous DICR TSTART write (clears TSTART
 * and raises the deferred interrupt). Called once per block by the dispatch
 * loop, exactly like gcn_vi_tick. */
void gcn_di_tick(void);
u32  gcn_di_read(void* user, CPUState* cpu, u32 addr, u8 size);
void gcn_di_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size);

/* ROADMAP M5 — mount lifecycle (see the file header's MOUNT LIFECYCLE note).
 *
 * gcn_di_set_disc: open `path` read-only and mount it (DVDInterface.cpp
 * SetDisc:385-431 restricted to the real-disc branch — no auto-disc-change
 * list, no Wii partition table). Replaces any previously-mounted image.
 * Returns 1 on success, 0 on failure (bad/missing path; loud stderr, no state
 * change — the drive stays exactly as it was). Safe to call at boot (before
 * the CPU runs, GCN_DISC in boot.c) or at runtime (debug_server.c
 * "insert_disc" — the hot-swap driver for the M5 menu-reaction scenario). */
int  gcn_di_set_disc(GcnDi* di, const char* path);
/* Unmount (DVDInterface.cpp SetDisc(nullptr) via EjectDiscCallback:443-446).
 * A no-op (but still re-runs SetLidOpen/ResetDrive, matching Dolphin) if no
 * disc is mounted. */
void gcn_di_eject_disc(GcnDi* di);
/* Release the disc file handle without touching drive registers/state — the
 * shutdown-time counterpart to gcn_exi_free/gcn_dsp_free. Safe to call
 * unconditionally (no-op if nothing is mounted). */
void gcn_di_free(GcnDi* di);

/* DVDInterface.cpp ResetDrive:306-346, transcribed (the DTK stream/read-buffer
 * fields it also clears are not modeled — see the DELIBERATELY DEFERRED note).
 * Exposed (not static) so pi.c's PI_RESET_CODE hook and gcn_di_set_disc/
 * gcn_di_eject_disc can all share one transcription. */
void gcn_di_reset_drive(GcnDi* di, int spinup);
/* No-arg trampoline around gcn_di_reset_drive(s_di, /spinup=/1), matching the
 * gcn_di_tick(void)/gcn_gp_reset(void) idiom other single-instance devices use
 * for cross-module hooks (pi.c stores a GcnPiResetDriveFn taking no args and
 * no user pointer, exactly like GcnPiFifoResetFn). Wired from boot.c via
 * gcn_pi_set_reset_drive_hook — see pi.h and the file header's "drive re-spin
 * trigger" note for why this is the real hardware path to DiscIdNotRead, not
 * a mount-time special case. */
void gcn_di_reset_drive_spinup(void);

#endif /* GCN_DI_DI_H */
