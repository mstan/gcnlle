/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Serial Interface (SI) model. See include/si/si.h for scope and the Dolphin
 * source the register/poll/transfer semantics are transcribed from.
 */
#include "si/si.h"
#include "debug/rings.h"

#include <string.h>

void gcn_si_set_irq(GcnSi* si, GcnSiIrqFn fn, void* user) {
    si->irq = fn;
    si->irq_user = user;
}

/* Neutral standard-controller pad report (SI_DeviceGCController.cpp:168-236,
 * reporting mode 3 = the power-on default, SI_DeviceGCController.h). MapPadStatus
 * packs the high word as (button|PAD_USE_ORIGIN)<<16 | stickX<<8 | stickY with
 * all sticks centered at 0x80 (GCPadStatus.h MAIN/C_STICK_CENTER) and no buttons
 * pressed (PAD_USE_ORIGIN=0x0080); mode-3 GetData packs the low word as
 * substickX<<24 | substickY<<16 | triggerLeft<<8 | triggerRight, triggers
 * released. Single source for both the polled channel registers and the
 * CMD_DIRECT buffer so they cannot drift (real input arrives via the
 * Observability input-injection surface). */
static void si_pad_report(u32* hi, u32* lo) {
    *hi = 0x00808080u;   /* (0|PAD_USE_ORIGIN)<<16 | stickX(0x80)<<8 | stickY(0x80) */
    *lo = 0x80800000u;   /* substickX(0x80)<<24 | substickY(0x80)<<16 | trigL(0)<<8 | trigR(0) */
}

/* Replay one VI-driven SI poll's steady-state effect on the CONNECTED channels
 * (SI.cpp UpdateDevices:529-548): each connected controller answers Success, so
 * we latch RDST and deposit its pad report into in_hi/in_lo. Disconnected
 * channels' NOREP is derived live in the SISR read path (see the header). Called
 * wherever a poll would certainly have run: init and every SICOMCSR read. */
static void si_poll(GcnSi* si) {
    for (u32 n = 0; n < GCN_SI_CHANNELS; n++) {
        if (si->ch[n].connected) {
            si->ch[n].rdst = 1;
            si_pad_report(&si->ch[n].in_hi, &si->ch[n].in_lo);
        }
    }
}

void gcn_si_init(GcnSi* si) {
    memset(si, 0, sizeof *si);
    for (u32 n = 0; n < GCN_SI_CHANNELS; n++)
        si->ch[n].mode = (u8)GCN_SI_CTRL_MODE_DEFAULT;
    /* Dolphin's default config connects a standard GC controller on channel 0
     * only (MainSettings.cpp:171-172). Its continuous VI poll latches RDST0 and
     * deposits the pad report long before the IPL first reads SICOMCSR, so ch0
     * powers up read-status-ready with its input registers already filled. */
    si->ch[0].connected = 1;
    si_poll(si);
}

static u32 si_any_rdst(const GcnSi* si) {
    for (u32 n = 0; n < GCN_SI_CHANNELS; n++)
        if (si->ch[n].rdst) return 1;
    return 0;
}

/* ---- interrupt line (SI.cpp UpdateInterrupts:98-116) ----
 * Dolphin derives RDSTINT = OR of the live per-channel RDST bits, then raises
 * the line on (RDSTINT & RDSTINTMSK) || (TCINT & TCINTMSK). We mirror the
 * derivation here from the same live RDST bits gcn_si_read exposes (never a
 * stored RDSTINT), so the line matches what a SICOMCSR read would return. The
 * continuous-VI-poll re-arm of RDST stays a SICOMCSR-read side effect (see the
 * header's timing-free model); this evaluation reads the current bits, so an
 * input-buffer read that clears RDST correctly drops the line until the next
 * SICOMCSR read re-arms it. Pushed to PI on EVERY evaluation (PI INTSR is W1C). */
static void si_update_interrupts(GcnSi* si) {
    int rdstint = si_any_rdst(si);
    int level = (rdstint && (si->comcsr & GCN_SI_CSR_RDSTINTMSK)) ||
                ((si->comcsr & GCN_SI_CSR_TCINT) && (si->comcsr & GCN_SI_CSR_TCINTMSK));
    if (si->irq)
        si->irq(si->irq_user, level);
    if (level != si->irq_level) {
        gcn_ring_event(level ? GCN_EV_IRQ_RAISE : GCN_EV_IRQ_CLEAR,
                       /*source*/ 3u /* PI cause bit index of INT_CAUSE_SI=0x8 */,
                       0u, 0u);
        si->irq_level = level;
    }
}

/* Synchronous SI transfer (mirror of Dolphin RunSIBuffer, SI.cpp:146-201). The
 * IPL kicks a transfer by writing TSTART; the device answers immediately into
 * the I/O buffer and TCINT latches. Controller command/response bytes are from
 * SI_DeviceGCController.cpp; the pad state is neutral/centered here (real input
 * arrives with the Observability input-injection surface). Exact command
 * responses are validated against the extended (post-sc) oracle. */
static void si_run_buffer(GcnSi* si) {
    u32 chan = (si->comcsr & GCN_SI_CSR_CHANNEL) >> 1u;
    GcnSiChannel* c = &si->ch[chan];

    si->comcsr &= ~GCN_SI_CSR_TSTART;      /* completes synchronously */

    if (!c->connected) {
        /* No device: Dolphin's RunSIBuffer gets actual_response_length < 0, sets
         * COMERR and calls SetNoResponse (SI.cpp:189-193). We latch COMERR here;
         * the channel's NOREP is derived live in the SISR read path (a
         * disconnected channel reports NOREP on every poll, see the header). */
        si->comcsr |= GCN_SI_CSR_COMERR | GCN_SI_CSR_TCINT;
        return;
    }

    u8 cmd = si->iobuf[0];
    switch (cmd) {
    case 0x00:                              /* CMD_STATUS */
    case 0xFF:                              /* CMD_RESET  */
        /* SI_GC_CONTROLLER = 0x09000000, high 3 bytes (SI_Device.cpp:92-99). */
        si->iobuf[0] = 0x09; si->iobuf[1] = 0x00; si->iobuf[2] = 0x00;
        break;
    case 0x40: {                            /* CMD_DIRECT: 8-byte pad report */
        u32 hi, lo;
        si_pad_report(&hi, &lo);            /* same neutral report the VI poll deposits */
        si->iobuf[0] = (u8)(hi >> 24); si->iobuf[1] = (u8)(hi >> 16);
        si->iobuf[2] = (u8)(hi >> 8);  si->iobuf[3] = (u8)hi;
        si->iobuf[4] = (u8)(lo >> 24); si->iobuf[5] = (u8)(lo >> 16);
        si->iobuf[6] = (u8)(lo >> 8);  si->iobuf[7] = (u8)lo;
        break;
    }
    case 0x41:                              /* CMD_ORIGIN */
    case 0x42:                              /* CMD_RECALIBRATE: 10-byte centered origin */
        memset(si->iobuf, 0, 10);
        si->iobuf[2] = 0x80; si->iobuf[3] = 0x80;   /* stick X/Y centered */
        si->iobuf[4] = 0x80; si->iobuf[5] = 0x80;   /* substick X/Y centered */
        break;
    default:
        break;                              /* unknown command: leave buffer as-is */
    }
    si->comcsr &= ~GCN_SI_CSR_COMERR;
    si->comcsr |= GCN_SI_CSR_TCINT;
}

u32 gcn_si_read(void* user, CPUState* cpu, u32 addr, u8 size) {
    (void)cpu; (void)size;
    GcnSi* si = (GcnSi*)user;
    u32 off = addr - GCN_SI_BASE;

    if (off < 0x30u) {                      /* channel out / in_hi / in_lo */
        u32 n = off / GCN_SI_CH_STRIDE;
        u32 sub = off % GCN_SI_CH_STRIDE;
        GcnSiChannel* c = &si->ch[n];
        if (sub == 0x4u || sub == 0x8u) {
            c->rdst = 0;                    /* reading input clears read-status */
            u32 v = (sub == 0x4u) ? c->in_hi : c->in_lo;
            si_update_interrupts(si);       /* cleared RDST may drop RDSTINT */
            return v;
        }
        return c->out;
    }

    switch (off) {
    case GCN_SI_POLL:  return si->poll;
    case GCN_SI_COMCSR: {
        /* Timing-free poll: a VI poll would have re-latched RDST and re-deposited
         * the pad report for every connected channel since the last input read
         * (see header). RDSTINT is then derived from the live RDST bits
         * (SI.cpp:98-108). */
        si_poll(si);
        u32 v = si->comcsr & ~GCN_SI_CSR_RDSTINT;
        if (si_any_rdst(si)) v |= GCN_SI_CSR_RDSTINT;
        si_update_interrupts(si);   /* the re-armed RDST just moved the line */
        return v;
    }
    case GCN_SI_SISR: {
        /* Per-channel status, derived live from the continuous poll (SI.cpp
         * UpdateDevices:529-548): a connected channel reports RDST while its
         * read-status latch is set; a disconnected channel's GetData returns
         * ErrorNoResponse every poll, latching NOREP (re-set after any W1C clear
         * by the next poll — the header's timing-free invariant). The stored
         * si->sisr keeps only the non-derived error bits (COLL/OVRUN/UNRUN, W1C). */
        u32 derived = 0;
        for (u32 n = 0; n < GCN_SI_CHANNELS; n++)
            derived |= GCN_SI_RDST_BIT(n) | GCN_SI_NOREP_BIT(n);
        u32 v = si->sisr & ~derived;
        for (u32 n = 0; n < GCN_SI_CHANNELS; n++) {
            if (si->ch[n].connected) {
                if (si->ch[n].rdst) v |= GCN_SI_RDST_BIT(n);
            } else {
                v |= GCN_SI_NOREP_BIT(n);
            }
        }
        return v;
    }
    case GCN_SI_EXILK: return si->exilk;
    default: break;
    }

    if (off >= GCN_SI_IOBUF && off < GCN_SI_SIZE) {
        u32 v = 0; u32 p = off - GCN_SI_IOBUF;
        for (u32 i = 0; i < (u32)size && p + i < sizeof si->iobuf; i++)
            v |= (u32)si->iobuf[p + i] << (24u - 8u * i);
        return v;
    }
    return 0;
}

void gcn_si_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size) {
    (void)cpu;
    GcnSi* si = (GcnSi*)user;
    u32 off = addr - GCN_SI_BASE;

    if (off < 0x30u) {                      /* channel out / in_hi / in_lo */
        u32 n = off / GCN_SI_CH_STRIDE;
        u32 sub = off % GCN_SI_CH_STRIDE;
        GcnSiChannel* c = &si->ch[n];
        if (sub == 0x0u)      c->out = value;
        else if (sub == 0x4u) c->in_hi = value;
        else                  c->in_lo = value;
        return;
    }

    switch (off) {
    case GCN_SI_POLL:  si->poll = value; return;
    case GCN_SI_COMCSR:
        si->comcsr = (si->comcsr & ~GCN_SI_CSR_PERSIST) | (value & GCN_SI_CSR_PERSIST);
        if (value & GCN_SI_CSR_TCINT) si->comcsr &= ~GCN_SI_CSR_TCINT;  /* W1C ack */
        /* RDSTINT is derived from RDST, not stored — its write-1 ack is absorbed
         * by recomputation on the next read (SI.cpp:375-378). */
        if (value & GCN_SI_CSR_TSTART) {
            si->comcsr |= GCN_SI_CSR_TSTART;
            si_run_buffer(si);          /* completes synchronously, latches TCINT */
        }
        /* Mask/ack changes and a completed transfer's TCINT all move the line
         * (SI.cpp CSR write handler ends in UpdateInterrupts:390). */
        si_update_interrupts(si);
        return;
    case GCN_SI_SISR:
        /* Error bits are write-1-to-clear; WR triggers per-channel SendCommand
         * (mode/rumble) and self-clears. The IPL boot path does not use WR yet,
         * so we clear it and defer SendCommand until the oracle shows it. */
        si->sisr &= ~(value & 0x0FFFFFFFu);
        si_update_interrupts(si);       /* SI.cpp:355 SISR write -> UpdateInterrupts */
        return;
    case GCN_SI_EXILK: si->exilk = value; return;
    default: break;
    }

    if (off >= GCN_SI_IOBUF && off < GCN_SI_SIZE) {
        u32 p = off - GCN_SI_IOBUF;
        for (u32 i = 0; i < (u32)size && p + i < sizeof si->iobuf; i++)
            si->iobuf[p + i] = (u8)(value >> (24u - 8u * i));
        return;
    }
}
