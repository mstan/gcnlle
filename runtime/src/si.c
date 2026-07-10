/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Serial Interface (SI) model. See include/si/si.h for scope and the Dolphin
 * source the register/poll/transfer semantics are transcribed from.
 */
#include "si/si.h"

#include <string.h>

void gcn_si_init(GcnSi* si) {
    memset(si, 0, sizeof *si);
    for (u32 n = 0; n < GCN_SI_CHANNELS; n++)
        si->ch[n].mode = (u8)GCN_SI_CTRL_MODE_DEFAULT;
    /* Dolphin's default config connects a standard GC controller on channel 0
     * only (MainSettings.cpp:171-172). Its continuous VI poll latches RDST0 long
     * before the IPL first reads SICOMCSR, so ch0 powers up read-status-ready. */
    si->ch[0].connected = 1;
    si->ch[0].rdst = 1;
}

static u32 si_any_rdst(const GcnSi* si) {
    for (u32 n = 0; n < GCN_SI_CHANNELS; n++)
        if (si->ch[n].rdst) return 1;
    return 0;
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
        /* No device: Dolphin returns -1 -> COMERR (+ NOREP in SISR, whose exact
         * bit we leave unmodeled until the oracle exercises it). */
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
        u32 hi = 0x00808080u;               /* button|PAD_USE_ORIGIN, stickX, stickY (neutral) */
        u32 lo = 0x80800000u;               /* substickX, substickY, triggerL, triggerR (neutral) */
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
            return (sub == 0x4u) ? c->in_hi : c->in_lo;
        }
        return c->out;
    }

    switch (off) {
    case GCN_SI_POLL:  return si->poll;
    case GCN_SI_COMCSR: {
        /* Timing-free RDST: a VI poll would have re-latched RDST for every
         * connected channel since the last input read (see header). RDSTINT is
         * then derived from the live RDST bits (SI.cpp:98-108). */
        for (u32 n = 0; n < GCN_SI_CHANNELS; n++)
            if (si->ch[n].connected) si->ch[n].rdst = 1;
        u32 v = si->comcsr & ~GCN_SI_CSR_RDSTINT;
        if (si_any_rdst(si)) v |= GCN_SI_CSR_RDSTINT;
        return v;
    }
    case GCN_SI_SISR: {
        u32 v = si->sisr & ~(GCN_SI_RDST_BIT(0) | GCN_SI_RDST_BIT(1) |
                             GCN_SI_RDST_BIT(2) | GCN_SI_RDST_BIT(3));
        for (u32 n = 0; n < GCN_SI_CHANNELS; n++)
            if (si->ch[n].rdst) v |= GCN_SI_RDST_BIT(n);
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
            si_run_buffer(si);
        }
        return;
    case GCN_SI_SISR:
        /* Error bits are write-1-to-clear; WR triggers per-channel SendCommand
         * (mode/rumble) and self-clears. The IPL boot path does not use WR yet,
         * so we clear it and defer SendCommand until the oracle shows it. */
        si->sisr &= ~(value & 0x0FFFFFFFu);
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
