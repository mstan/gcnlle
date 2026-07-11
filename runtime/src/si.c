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

/* Debug-server singleton (mirrors vi.c's s_vi / gcn_vi_xfb_info): the debug
 * server has no other handle to the live SI instance boot.c owns. */
static GcnSi* s_si = NULL;

/* Standard-controller pad report (SI_DeviceGCController.cpp:168-236, reporting
 * mode 3 = the power-on default, SI_DeviceGCController.h). MapPadStatus packs
 * the high word as (button|PAD_USE_ORIGIN)<<16 | stickX<<8 | stickY —
 * PAD_USE_ORIGIN (0x0080) is ORed in unconditionally and coexists with every
 * real button bit, never gated by one; mode-3 GetData packs the low word as
 * substickX<<24 | substickY<<16 | triggerLeft<<8 | triggerRight. Single source
 * for both the polled channel registers and the CMD_DIRECT buffer so they
 * cannot drift. Reads the injected state (si->input) — neutral at init
 * (buttons=0, sticks centered 0x80, triggers 0) until the debug-surface
 * set_input command (or a live client) changes it. */
static void si_pad_report(const GcnSi* si, u32* hi, u32* lo) {
    const GcnSiPadInput* in = &si->input;
    *hi = (u32)in->stick_y | ((u32)in->stick_x << 8) |
          (((u32)in->buttons | GCN_SI_PAD_USE_ORIGIN) << 16);
    *lo = (u32)in->trigger_r | ((u32)in->trigger_l << 8) |
          ((u32)in->substick_y << 16) | ((u32)in->substick_x << 24);
}

/* One VI-scheduled SI poll (SI.cpp UpdateDevices:529-548): each connected
 * controller answers Success, so we latch RDST and deposit its pad report
 * into in_hi/in_lo; each disconnected channel answers ErrorNoResponse, so we
 * latch NOREP (SetNoResponse, SI.cpp:538-539) — both real per-channel latches,
 * not values re-derived on read. Called only from gcn_si_beam_poll, the beam-
 * scheduled hook (see the header doc). */
static void si_poll(GcnSi* si) {
    for (u32 n = 0; n < GCN_SI_CHANNELS; n++) {
        if (si->ch[n].connected) {
            si->ch[n].rdst = 1;
            si_pad_report(si, &si->ch[n].in_hi, &si->ch[n].in_lo);
        } else {
            si->ch[n].norep = 1;
        }
    }
}

void gcn_si_init(GcnSi* si) {
    memset(si, 0, sizeof *si);
    for (u32 n = 0; n < GCN_SI_CHANNELS; n++)
        si->ch[n].mode = (u8)GCN_SI_CTRL_MODE_DEFAULT;
    /* Dolphin's default config connects a standard GC controller on channel 0
     * only (MainSettings.cpp:171-172). */
    si->ch[0].connected = 1;
    si->input = (GcnSiPadInput)GCN_SI_PAD_INPUT_NEUTRAL;
    /* SI.cpp Init:275-276 default poll period (before the IPL ever writes
     * SIPOLL), and VideoInterface.cpp:175 "first sampling starts at vsync" —
     * seed the schedule from beam zero rather than a special-case poll here.
     * The beam (vi.c, ticking from cycle 0 alongside the CPU) reaches
     * next_poll_halfline, and any further default-period polls after it, well
     * before BS2's own init code reaches its first SISR read — so RDST0 ends
     * up genuinely poll-latched by then, matching Dolphin's continuous
     * background poll without faking the result here. */
    si->poll = GCN_SI_POLL_DEFAULT;
    si->next_poll_halfline = GCN_SI_NUM_HALF_LINES_FOR_POLL;
    s_si = si;   /* register for the debug-surface set_input/get_input accessors */
}

void gcn_si_set_input(GcnSi* si, const GcnSiPadInput* input) {
    si->input = *input;
}

void gcn_si_reset_input(GcnSi* si) {
    si->input = (GcnSiPadInput)GCN_SI_PAD_INPUT_NEUTRAL;
}

int gcn_si_debug_set_input(int have_buttons, u32 buttons,
                            int have_stick_x, u32 stick_x,
                            int have_stick_y, u32 stick_y,
                            int have_substick_x, u32 substick_x,
                            int have_substick_y, u32 substick_y,
                            int have_trigger_l, u32 trigger_l,
                            int have_trigger_r, u32 trigger_r,
                            int reset) {
    if (!s_si) return 0;
    if (reset) {
        gcn_si_reset_input(s_si);
        return 1;
    }
    GcnSiPadInput* in = &s_si->input;
    if (have_buttons)    in->buttons    = (u16)buttons;
    if (have_stick_x)    in->stick_x    = (u8)stick_x;
    if (have_stick_y)    in->stick_y    = (u8)stick_y;
    if (have_substick_x) in->substick_x = (u8)substick_x;
    if (have_substick_y) in->substick_y = (u8)substick_y;
    if (have_trigger_l)  in->trigger_l  = (u8)trigger_l;
    if (have_trigger_r)  in->trigger_r  = (u8)trigger_r;
    return 1;
}

int gcn_si_debug_get_input(GcnSiPadInput* out) {
    if (!s_si) {
        *out = (GcnSiPadInput)GCN_SI_PAD_INPUT_NEUTRAL;
        return 0;
    }
    *out = s_si->input;
    return 1;
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
 * stored RDSTINT), so the line matches what a SICOMCSR read would return. RDST
 * is re-armed only by a scheduled beam poll (gcn_si_beam_poll) now, never by a
 * register read; this evaluation reads whatever the current bits are, so an
 * input-buffer read that clears RDST correctly drops the line until the next
 * scheduled poll re-arms it. Pushed to PI on EVERY evaluation (PI INTSR is W1C). */
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

/* VI-beam per-halfline hook (vi.h GcnViSiPollFn), fired unconditionally by
 * vi_advance_halfline for EVERY halfline (not just when a poll is due — the
 * due-check happens here). Transcribes VideoInterface.cpp:950-968 exactly,
 * including the statement order (poll-and-reschedule, THEN the field-boundary
 * override) — see the si.h header doc for the full citation. Needs
 * si_update_interrupts, hence sits after it. */
void gcn_si_beam_poll(void* user, u32 half_line_count, int is_at_field_boundary) {
    GcnSi* si = (GcnSi*)user;

    if (half_line_count == si->next_poll_halfline) {
        si_poll(si);
        si_update_interrupts(si);   /* UpdateDevices ends in UpdateInterrupts, SI.cpp:550 */
        u32 x = (si->poll >> GCN_SI_POLL_X_SHIFT) & GCN_SI_POLL_X_MASK;
        si->next_poll_halfline += 2u * x;
    }

    if (is_at_field_boundary)
        si->next_poll_halfline = half_line_count + GCN_SI_NUM_HALF_LINES_FOR_POLL;
}

/* Synchronous SI transfer (mirror of Dolphin RunSIBuffer, SI.cpp:146-201). The
 * IPL kicks a transfer by writing TSTART; the device answers immediately into
 * the I/O buffer and TCINT latches. Controller command/response bytes are from
 * SI_DeviceGCController.cpp; the pad state reflects the Observability
 * input-injection surface (si->input, neutral/centered until set_input
 * changes it). Exact command responses are validated against the extended
 * (post-sc) oracle. */
static void si_run_buffer(GcnSi* si) {
    u32 chan = (si->comcsr & GCN_SI_CSR_CHANNEL) >> 1u;
    GcnSiChannel* c = &si->ch[chan];

    si->comcsr &= ~GCN_SI_CSR_TSTART;      /* completes synchronously */

    if (!c->connected) {
        /* No device: Dolphin's RunSIBuffer gets actual_response_length < 0, sets
         * COMERR and calls SetNoResponse (SI.cpp:189-193) — latch NOREP here too,
         * the same per-channel latch a beam poll sets (si_poll). */
        si->comcsr |= GCN_SI_CSR_COMERR | GCN_SI_CSR_TCINT;
        c->norep = 1;
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
        si_pad_report(si, &hi, &lo);         /* same report the VI poll deposits */
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
        /* Pure DirectRead (SI.cpp:364): RDST/RDSTINT are re-armed ONLY by a
         * scheduled beam poll (gcn_si_beam_poll) now, never by a register read.
         * RDSTINT is derived from the live RDST bits on every read (SI.cpp:
         * 98-108) but that derivation has no side effect — it just reflects
         * whatever the last poll (or input-buffer read) left behind. */
        u32 v = si->comcsr & ~GCN_SI_CSR_RDSTINT;
        if (si_any_rdst(si)) v |= GCN_SI_CSR_RDSTINT;
        return v;
    }
    case GCN_SI_SISR: {
        /* Pure DirectRead of m_status_reg (SI.cpp:393): no side effect. RDST and
         * NOREP are both real per-channel latches (GcnSiChannel.rdst/.norep):
         * RDST is latched by gcn_si_beam_poll (scheduled off the VI beam,
         * VideoInterface.cpp:950-968 / SI.cpp:529-548) and cleared by reading
         * that channel's input buffer; NOREP is latched the same way (or by a
         * disconnected-channel TSTART, si_run_buffer) and cleared by a SISR W1C
         * write (gcn_si_write). A read here only OBSERVES the current latches,
         * it never re-polls. The IPL's PAD path tests RDST0 through SISR only
         * (SIGetResponseRaw: read SISR -> RDST set? -> read in_hi/in_lo); it now
         * genuinely sees RDST0 only once the beam's next scheduled poll has
         * actually run, not on every read. */
        u32 derived = 0;
        for (u32 n = 0; n < GCN_SI_CHANNELS; n++)
            derived |= GCN_SI_RDST_BIT(n) | GCN_SI_NOREP_BIT(n);
        u32 v = si->sisr & ~derived;
        for (u32 n = 0; n < GCN_SI_CHANNELS; n++) {
            if (si->ch[n].rdst)  v |= GCN_SI_RDST_BIT(n);
            if (si->ch[n].norep) v |= GCN_SI_NOREP_BIT(n);
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
        /* Error bits are write-1-to-clear; NOREP is the same W1C shape but
         * lives per-channel (SI.cpp:398-433) rather than in si->sisr. WR
         * triggers per-channel SendCommand (mode/rumble) and self-clears. The
         * IPL boot path does not use WR yet, so we clear it and defer
         * SendCommand until the oracle shows it. */
        si->sisr &= ~(value & 0x0FFFFFFFu);
        for (u32 n = 0; n < GCN_SI_CHANNELS; n++)
            if (value & GCN_SI_NOREP_BIT(n)) si->ch[n].norep = 0;
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
