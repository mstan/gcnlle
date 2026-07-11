/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pixel Engine (PE) model (impl). See include/pe/pe.h for scope and the exact
 * Dolphin PixelEngine functions/lines every register and bit is transcribed
 * from. 16-bit registers; 32-bit access is split into halves like vi.c.
 */
#include "pe/pe.h"
#include "debug/rings.h"

#include <string.h>

void gcn_pe_set_irq(GcnPe* pe, GcnPeIrqFn fn, void* user) {
    pe->irq = fn;
    pe->irq_user = user;
}

void gcn_pe_init(GcnPe* pe) {
    /* PixelEngineManager::Init:56-72 — all config + control + token cleared,
     * both signal latches false. */
    memset(pe, 0, sizeof *pe);
}

/* ---- interrupt line (PixelEngine.cpp UpdateInterrupts:161-172) ----
 * Drive the two PE->PI causes independently: token = signal && enable, finish =
 * signal && enable. Called on every CTRL write and every SetToken/SetFinish.
 * Level-pushed each time (idempotent at the PI), transition-logged to the ring. */
static void pe_update_interrupts(GcnPe* pe) {
    int tok = pe->signal_token_interrupt && pe->token_enable;
    int fin = pe->signal_finish_interrupt && pe->finish_enable;
    if (pe->irq) {
        pe->irq(pe->irq_user, GCN_PE_INT_TOKEN, tok);
        pe->irq(pe->irq_user, GCN_PE_INT_FINISH, fin);
    }
    if (tok != pe->token_level) {
        gcn_ring_event(tok ? GCN_EV_IRQ_RAISE : GCN_EV_IRQ_CLEAR,
                       /*source*/ 9u /* PI cause bit of INT_CAUSE_PE_TOKEN=0x200 */, 0u, 0u);
        pe->token_level = tok;
    }
    if (fin != pe->finish_level) {
        gcn_ring_event(fin ? GCN_EV_IRQ_RAISE : GCN_EV_IRQ_CLEAR,
                       /*source*/ 10u /* PI cause bit of INT_CAUSE_PE_FINISH=0x400 */, 0u, 0u);
        pe->finish_level = fin;
    }
}

/* ---- 16-bit register access ---- */

static u16 pe_read16(GcnPe* pe, u32 off) {
    switch (off) {
    case GCN_PE_CTRL:
        /* Reads return the control word with the write-only token/finish bits
         * (2-3) forced 0 (PixelEngine.cpp:139-140 keep them false; DirectRead
         * of m_control.hex therefore never shows them). */
        return pe->reg[GCN_PE_CTRL >> 1] & GCN_PE_CTRL_STORE_MASK;
    case GCN_PE_TOKEN:
        return pe->token;               /* RO (PixelEngine.cpp:147) */
    default:
        return pe->reg[off >> 1];
    }
}

static void pe_write16(GcnPe* pe, u32 off, u16 value) {
    switch (off) {
    case GCN_PE_ZCONF:
    case GCN_PE_ALPHACONF:
    case GCN_PE_DSTALPHACONF:
    case GCN_PE_ALPHAMODE:
    case GCN_PE_ALPHAREAD:
        pe->reg[off >> 1] = value;      /* DirectWrite (PixelEngine.cpp:80-96) */
        return;

    case GCN_PE_CTRL:
        /* PixelEngine.cpp:124-144. Writing token/finish (bits 2-3) acks the
         * corresponding signal latch; store the two enable bits; the write-only
         * token/finish bits are forced 0 in the stored control word. */
        if (value & GCN_PE_CTRL_TOKEN)
            pe->signal_token_interrupt = 0;
        if (value & GCN_PE_CTRL_FINISH)
            pe->signal_finish_interrupt = 0;
        pe->token_enable  = (value & GCN_PE_CTRL_TOKEN_ENABLE)  ? 1 : 0;
        pe->finish_enable = (value & GCN_PE_CTRL_FINISH_ENABLE) ? 1 : 0;
        pe->reg[GCN_PE_CTRL >> 1] = value & GCN_PE_CTRL_STORE_MASK;
        pe_update_interrupts(pe);
        return;

    default:
        /* TOKEN, BBOX and perf-query registers are InvalidWrite on hardware
         * (PixelEngine.cpp:117-121,147,152-157) — ignore. */
        return;
    }
}

u32 gcn_pe_read(void* user, CPUState* cpu, u32 addr, u8 size) {
    GcnPe* pe = (GcnPe*)user;
    u32 off = addr - GCN_PE_BASE;
    (void)cpu;
    switch (size) {
    case 1: {
        u16 v = pe_read16(pe, off & ~1u);
        return (addr & 1u) ? (v & 0xFFu) : (v >> 8);
    }
    case 2:
        return pe_read16(pe, off);
    default: /* 4: HI halfword at off, LO at off+2 */
        return (pe_read16(pe, off) << 16) | pe_read16(pe, off + 2u);
    }
}

void gcn_pe_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size) {
    GcnPe* pe = (GcnPe*)user;
    u32 off = addr - GCN_PE_BASE;
    (void)cpu;
    switch (size) {
    case 1: {
        u16 v = pe_read16(pe, off & ~1u);
        if (addr & 1u) v = (u16)((v & 0xFF00u) | (value & 0xFFu));
        else           v = (u16)((v & 0x00FFu) | ((value & 0xFFu) << 8));
        pe_write16(pe, off & ~1u, v);
        return;
    }
    case 2:
        pe_write16(pe, off, (u16)value);
        return;
    default: /* 4 */
        pe_write16(pe, off, (u16)(value >> 16));
        pe_write16(pe, off + 2u, (u16)value);
        return;
    }
}

/* ---- video-backend entry points (PixelEngine.cpp SetToken/SetFinish + the
 *      SetTokenFinish_OnMainThread effect). WIRED BUT UNCALLED this increment.
 *      Dolphin defers the latch/interrupt via a CoreTiming event; with no
 *      scheduler we apply the same end state synchronously. ---- */

void gcn_pe_set_token(GcnPe* pe, u16 token, int interrupt) {
    pe->token = token;                          /* m_token = m_token_pending */
    if (interrupt)
        pe->signal_token_interrupt = 1;
    pe_update_interrupts(pe);
}

void gcn_pe_set_finish(GcnPe* pe) {
    pe->signal_finish_interrupt = 1;
    pe_update_interrupts(pe);
}
