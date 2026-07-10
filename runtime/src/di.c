/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Disc Interface (DI) model (impl). See include/di/di.h for scope and the exact
 * Dolphin DVDInterface functions/lines every register, bit and command response
 * is transcribed from. No disc is inserted this milestone (M5 mounts one), so
 * IsDiscInside() is hard-wired false and every read-like command DEINTs.
 */
#include "di/di.h"
#include "debug/rings.h"

#include <stdio.h>
#include <string.h>

/* The dispatch loop ticks one DI (like the one VI); registered at init. */
static GcnDi* s_di = NULL;

void gcn_di_set_irq(GcnDi* di, GcnDiIrqFn fn, void* user) {
    di->irq = fn;
    di->irq_user = user;
}

/* No disc this milestone. DVDInterface::IsDiscInside() == DVDThread::HasDisc()
 * (DVDInterface.cpp:433-436); with nothing mounted it is false. M5 flips this to
 * the live mounted-disc state — the single switch that turns the whole no-disc
 * command path into a real drive. */
static int di_disc_inside(void) { return 0; }

/* ---- interrupt line (DVDInterface.cpp UpdateInterrupts:632-642) ----
 * Level = OR of the four (status & mask) pairs across DISR and DICVR. Pushed to
 * the PI DI cause line on EVERY evaluation (not just edges): PI INTSR is W1C, so
 * an acked-but-still-asserted level must re-appear — exactly as in Dolphin. */
static void di_update_interrupts(GcnDi* di) {
    int level =
        ((di->disr  & GCN_DI_SR_DEINT)  && (di->disr  & GCN_DI_SR_DEINTMASK))  ||
        ((di->disr  & GCN_DI_SR_TCINT)  && (di->disr  & GCN_DI_SR_TCINTMASK))  ||
        ((di->disr  & GCN_DI_SR_BRKINT) && (di->disr  & GCN_DI_SR_BRKINTMASK)) ||
        ((di->dicvr & GCN_DI_CVR_CVRINT)&& (di->dicvr & GCN_DI_CVR_CVRINTMASK));

    if (di->irq)
        di->irq(di->irq_user, level);
    if (level != di->irq_level) {
        gcn_ring_event(level ? GCN_EV_IRQ_RAISE : GCN_EV_IRQ_CLEAR,
                       /*source*/ 2u /* PI cause bit index of INT_CAUSE_DI=0x4 */,
                       0u, 0u);
        di->irq_level = level;
    }
}

/* DVDInterface.cpp GenerateDIInterrupt:644-663 — latch a status bit + re-eval. */
static void di_generate_interrupt(GcnDi* di, u8 type) {
    switch (type) {
    case GCN_DI_INT_DEINT:  di->disr  |= GCN_DI_SR_DEINT;   break;
    case GCN_DI_INT_TCINT:  di->disr  |= GCN_DI_SR_TCINT;   break;
    case GCN_DI_INT_BRKINT: di->disr  |= GCN_DI_SR_BRKINT;  break;
    case GCN_DI_INT_CVRINT: di->dicvr |= GCN_DI_CVR_CVRINT; break;
    }
    di_update_interrupts(di);
}

/* DVDInterface.cpp CheckReadPreconditions:706-738. With no disc the very first
 * check fires (MediumNotPresent); the rest are transcribed for M5 fidelity. */
static int di_check_read_preconditions(GcnDi* di) {
    if (!di_disc_inside()) {          /* implies CoverOpened or NoMediumPresent */
        di->error_code = GCN_DI_ERR_MEDIUM_NOT_PRESENT;
        return 0;
    }
    if (di->drive_state == GCN_DI_STATE_DISC_CHANGE) {
        di->error_code = 0x62800u;    /* MediumChanged */
        return 0;
    }
    if (di->drive_state == GCN_DI_STATE_MOTOR_STOPPED) {
        di->error_code = GCN_DI_ERR_MOTOR_STOPPED;
        return 0;
    }
    if (di->drive_state == GCN_DI_STATE_DISC_ID_NOT_READ) {
        di->error_code = GCN_DI_ERR_NO_DISC_ID;
        return 0;
    }
    return 1;
}

/* DVDInterface.cpp ExecuteCommand:782-1234, restricted to the GameCube-retail
 * command set the IPL issues. Runs the command's immediate effects and returns
 * the interrupt type to raise on completion (default TCINT). No command is ever
 * "handled by thread" here (that path requires a mounted disc), so completion is
 * always deferred to the next tick by the caller. `cpu` is used for the Inquiry
 * DMA writes into guest RAM. */
static u8 di_execute_command(GcnDi* di, CPUState* cpu) {
    u8 intr = GCN_DI_INT_TCINT;
    u8 cmd  = (u8)(di->cmdbuf[0] >> 24);

    /* RequestError needs the error code the previous command set, so only
     * non-RequestError commands reset it here (ExecuteCommand:803-805). */
    if (cmd != GCN_DI_CMD_REQUEST_ERROR)
        di->error_code = GCN_DI_ERR_NONE;

    switch (cmd) {
    case GCN_DI_CMD_INQUIRY:                 /* ExecuteCommand:810-820 */
        /* Drive firmware ID (shuffle2's Wii dump). Faithful DMA into DIMAR. */
        mem_write32(cpu, di->dimar + 0, 0x00000002u);   /* revision / device code */
        mem_write32(cpu, di->dimar + 4, 0x20060526u);   /* release date */
        mem_write32(cpu, di->dimar + 8, 0x41000000u);   /* version */
        break;

    case GCN_DI_CMD_READ:                    /* ExecuteCommand:839-882 */
        switch (di->cmdbuf[0] & 0xFFu) {
        case 0x00:                           /* Read Sector */
            if (di->drive_state == GCN_DI_STATE_READY_NO_READS_MADE)
                di->drive_state = GCN_DI_STATE_READY;
            if (!di_check_read_preconditions(di))   /* no disc => DEINT */
                intr = GCN_DI_INT_DEINT;
            break;
        case 0x40:                           /* Read DiscID */
            if (di->drive_state == GCN_DI_STATE_DISC_ID_NOT_READ)
                di->drive_state = GCN_DI_STATE_READY_NO_READS_MADE;
            else if (di->drive_state == GCN_DI_STATE_READY_NO_READS_MADE)
                di->drive_state = GCN_DI_STATE_READY;
            if (!di_check_read_preconditions(di))   /* no disc => DEINT */
                intr = GCN_DI_INT_DEINT;
            break;
        default:                             /* unknown read subcommand: logged, TCINT */
            break;
        }
        break;

    case GCN_DI_CMD_SEEK:                     /* ExecuteCommand:885-889 (ignored, TCINT) */
        break;

    case GCN_DI_CMD_REQUEST_ERROR: {          /* ExecuteCommand:982-994 */
        u32 state = (di->drive_state == GCN_DI_STATE_READY)
                        ? 0u : (u32)di->drive_state - 1u;
        di->diimmbuf = (state << 24) | di->error_code;
        di->error_code = GCN_DI_ERR_NONE;
        break;
    }

    case GCN_DI_CMD_AUDIO_STREAM:             /* ExecuteCommand:1001-1064 */
    case GCN_DI_CMD_AUDIO_STATUS:             /* ExecuteCommand:1067-1119 */
        /* Both begin with CheckReadPreconditions; no disc => DEINT before any
         * DTK work (and even with a disc, DTK-disabled => NoAudioBuf/DEINT). The
         * streaming data path itself is deferred (see di.h). */
        (void)di_check_read_preconditions(di);   /* sets MediumNotPresent */
        intr = GCN_DI_INT_DEINT;
        break;

    case GCN_DI_CMD_STOP_MOTOR:               /* ExecuteCommand:1122-1148 */
        if (di->drive_state == GCN_DI_STATE_READY ||
            di->drive_state == GCN_DI_STATE_READY_NO_READS_MADE ||
            di->drive_state == GCN_DI_STATE_DISC_ID_NOT_READ)
            di->drive_state = GCN_DI_STATE_MOTOR_STOPPED;
        /* Auto-disc-change / software eject need a mounted disc (deferred). */
        break;

    case GCN_DI_CMD_AUDIO_CONFIG:             /* ExecuteCommand:1152-1178 */
        /* Requires a disc + ReadyNoReadsMade; no disc => DEINT via the check. */
        if (!di_check_read_preconditions(di))
            intr = GCN_DI_INT_DEINT;
        break;

    default:                                  /* ExecuteCommand default:1218-1224 */
        di->error_code = GCN_DI_ERR_INVALID_COMMAND;
        intr = GCN_DI_INT_DEINT;
        break;
    }

    return intr;
}

/* DVDInterface.cpp FinishExecutingCommand:1306-1353, ReplyType::Interrupt. On a
 * successful transfer (TCINT) DIMAR advances by DILENGTH and DILENGTH clears;
 * then, iff TSTART is still set, clear it and raise the interrupt. */
void gcn_di_tick(void) {
    GcnDi* di = s_di;
    if (!di || !di->cmd_pending)
        return;
    di->cmd_pending = 0;

    if (di->pending_intr == GCN_DI_INT_TCINT) {
        di->dimar += di->dilength;
        di->dilength = 0;
    }
    if (di->dicr & GCN_DI_CR_TSTART) {
        di->dicr &= ~GCN_DI_CR_TSTART;
        di_generate_interrupt(di, di->pending_intr);
    }
}

void gcn_di_init(GcnDi* di) {
    memset(di, 0, sizeof *di);
    /* DVDInterface::Init:261-302 — DICVR.Hex=1 (cover open: no disc), then
     * ResetDrive(false):306-346 with no disc => DriveState::CoverOpened. */
    di->dicvr = GCN_DI_CVR_CVR;
    di->drive_state = GCN_DI_STATE_COVER_OPENED;
    di->error_code = GCN_DI_ERR_NONE;
    di->irq_level = 0;
    s_di = di;
}

/* ---- MMIO (DVDInterface.cpp RegisterMMIO:547-630) ---- */

static void di_warn_size_once(u32 addr, u8 size, int write) {
    static int warned = 0;
    if (!warned) {
        fprintf(stderr, "gcn di: %d-bit %s at 0x%08X — DI registers are 32-bit; "
                        "handling as 32-bit\n",
                (int)size * 8, write ? "write" : "read", addr);
        warned = 1;
    }
}

u32 gcn_di_read(void* user, CPUState* cpu, u32 addr, u8 size) {
    (void)cpu;
    GcnDi* di = (GcnDi*)user;
    u32 off = addr - GCN_DI_BASE;
    if (size != 4)
        di_warn_size_once(addr, size, 0);

    switch (off) {
    case GCN_DI_SR:      return di->disr;
    case GCN_DI_CVR:     return di->dicvr;
    case GCN_DI_CMDBUF0: return di->cmdbuf[0];
    case GCN_DI_CMDBUF1: return di->cmdbuf[1];
    case GCN_DI_CMDBUF2: return di->cmdbuf[2];
    case GCN_DI_MAR:     return di->dimar;
    case GCN_DI_LENGTH:  return di->dilength;
    case GCN_DI_CR:      return di->dicr;
    case GCN_DI_IMMBUF:  return di->diimmbuf;
    case GCN_DI_CFG:     return GCN_DI_CFG_VALUE;   /* read-only */
    default:             return 0;
    }
}

void gcn_di_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size) {
    GcnDi* di = (GcnDi*)user;
    u32 off = addr - GCN_DI_BASE;
    if (size != 4)
        di_warn_size_once(addr, size, 1);

    switch (off) {
    case GCN_DI_SR:
        /* Copy the mask/break enables; W1C the three status bits; re-eval. */
        di->disr = (di->disr & ~(GCN_DI_SR_DEINTMASK | GCN_DI_SR_TCINTMASK |
                                 GCN_DI_SR_BRKINTMASK | GCN_DI_SR_BREAK)) |
                   (value & (GCN_DI_SR_DEINTMASK | GCN_DI_SR_TCINTMASK |
                             GCN_DI_SR_BRKINTMASK | GCN_DI_SR_BREAK));
        if (value & GCN_DI_SR_DEINT)  di->disr &= ~GCN_DI_SR_DEINT;
        if (value & GCN_DI_SR_TCINT)  di->disr &= ~GCN_DI_SR_TCINT;
        if (value & GCN_DI_SR_BRKINT) di->disr &= ~GCN_DI_SR_BRKINT;
        /* BREAK/abort is not modeled (IPL boot never sets it); Dolphin
         * DEBUG_ASSERTs. Any future BREAK write diverges loudly here. */
        di_update_interrupts(di);
        return;

    case GCN_DI_CVR:
        di->dicvr = (di->dicvr & ~GCN_DI_CVR_CVRINTMASK) |
                    (value & GCN_DI_CVR_CVRINTMASK);
        if (value & GCN_DI_CVR_CVRINT) di->dicvr &= ~GCN_DI_CVR_CVRINT;   /* W1C */
        di_update_interrupts(di);
        return;

    case GCN_DI_CMDBUF0: di->cmdbuf[0] = value; return;
    case GCN_DI_CMDBUF1: di->cmdbuf[1] = value; return;
    case GCN_DI_CMDBUF2: di->cmdbuf[2] = value; return;
    case GCN_DI_MAR:     di->dimar = value & GCN_DI_MAR_MASK; return;
    case GCN_DI_LENGTH:  di->dilength = value & GCN_DI_LENGTH_MASK; return;

    case GCN_DI_CR:
        di->dicr = value & GCN_DI_CR_MASK;
        if (di->dicr & GCN_DI_CR_TSTART) {
            /* ExecuteCommand runs immediately; completion is deferred to the
             * next gcn_di_tick (see di.h COMPLETION MODEL). */
            di->pending_intr = di_execute_command(di, cpu);
            di->cmd_pending = 1;
        }
        return;

    case GCN_DI_IMMBUF: di->diimmbuf = value; return;

    case GCN_DI_CFG:
        /* Read-only (DVDInterface.cpp:628-629 InvalidWrite). Ignore. */
        return;

    default:
        return;
    }
}
