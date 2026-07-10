/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * DSP interface model. The 0xCC005000 block splits two ways:
 *   - mailboxes (0x00-0x06) and the control register's DSP-owned bits
 *     (DSP_CONTROL_MASK) route to the REAL DSP running its firmware, via the
 *     dsp_lle C API (runtime/dsp_lle/ — Dolphin's DSP-LLE interpreter + our
 *     Host binding). No HLE: the DSP boots its IROM and runs the ucode the IPL
 *     uploads, posting real mail.
 *   - the CPU-side CSR bits (interrupt status/masks, ARAM-DMA in-progress) and
 *     the AR/AI DMA engine (0x12/0x20/0x24/0x28) are Flipper hardware, modeled
 *     here. The AR DMA moves bytes between MEM1 and the 16 MB ARAM for real.
 *
 * CSR read/write mirror Dolphin's DSP.cpp: the DSP-owned bits come from the core
 * (dsp_lle_read_control), the rest from this CPU-side state.
 */
#include "dsp/dsp.h"
#include "dsp_lle_c.h"

#include <stdlib.h>
#include <string.h>

void gcn_dsp_init(GcnDsp* dsp, const u8* irom, const u8* coef, u8* mem1, u32 mem1_size) {
    memset(dsp, 0, sizeof *dsp);
    dsp_lle_init(irom, coef, mem1, mem1_size);   /* brings up the core + ARAM */
}

void gcn_dsp_free(GcnDsp* dsp) {
    (void)dsp;
    dsp_lle_shutdown();   /* frees the shared ARAM */
}

void gcn_dsp_tick(u32 ppc_cycles) {
    if (getenv("GCN_DSP_NOTICK")) return;   /* bisect guard */
    dsp_lle_update((int)ppc_cycles);
}

static u16 idx16(u32 off) { return (u16)(off >> 1); }

/* Perform the ARAM DMA the guest just kicked (Flipper AR engine, CPU-side). See
 * the header for the register layout; direction bit 31 of AR_CNT. */
static void aram_dma_kick(GcnDsp* dsp, CPUState* cpu, u32 cnt) {
    u32 mmaddr = ((u32)dsp->reg[idx16(GCN_DSP_AR_MMADDR)]     << 16) |
                  (u32)dsp->reg[idx16(GCN_DSP_AR_MMADDR) + 1];
    u32 araddr = ((u32)dsp->reg[idx16(GCN_DSP_AR_ARADDR)]     << 16) |
                  (u32)dsp->reg[idx16(GCN_DSP_AR_ARADDR) + 1];
    u32 len    = cnt & 0x03FFFFFFu;
    int to_aram = ((cnt & 0x80000000u) == 0);
    u8* aram = dsp_lle_aram();          /* shared with the DSP accelerator */
    u32 aram_size = dsp_lle_aram_size();

    if (aram && len &&
        (u64)mmaddr + len <= cpu->ram_size &&
        (u64)araddr + len <= aram_size) {
        if (to_aram)
            memcpy(aram + araddr, cpu->ram + mmaddr, len);
        else
            memcpy(cpu->ram + mmaddr, aram + araddr, len);
    }

    dsp->dma_active = true;
    dsp->dma_polls_left = GCN_DSP_ARAM_DMA_NOMINAL_POLLS;
}

/* CPU-side CSR bits (masks, interrupt status, ARAM-DMA bit), combined with the
 * DSP-owned bits read from the real core. */
static u32 read_csr(GcnDsp* dsp) {
    /* A DSP->CPU interrupt request from the core latches the mailbox int bit. */
    if (dsp_lle_take_interrupt())
        dsp->csr |= GCN_DSP_CSR_DSPINT;

    u32 v = (dsp->csr & ~GCN_DSP_CONTROL_MASK)
          | (dsp->dma_active ? GCN_DSP_CSR_DMA : 0u)
          | ((u32)dsp_lle_read_control() & GCN_DSP_CONTROL_MASK);

    if (dsp->dma_active && --dsp->dma_polls_left == 0) {
        dsp->dma_active = false;
        dsp->csr |= GCN_DSP_CSR_ARINT;   /* AR DMA complete raises the ARAM int */
    }
    return v;
}

static void write_csr(GcnDsp* dsp, u16 w) {
    /* DSP-owned bits (reset/halt/init) drive the real core. */
    dsp_lle_write_control((uint16_t)(w & GCN_DSP_CONTROL_MASK));

    /* CPU-side interrupt status is write-1-to-clear. */
    dsp->csr &= (u16)~(w & (GCN_DSP_CSR_AIDINT | GCN_DSP_CSR_ARINT | GCN_DSP_CSR_DSPINT));
    /* CPU-side interrupt masks are stored. */
    const u16 MASKS = GCN_DSP_CSR_AIDMASK | GCN_DSP_CSR_ARMASK | GCN_DSP_CSR_DSPMASK;
    dsp->csr = (u16)((dsp->csr & ~MASKS) | (w & MASKS));
}

u32 gcn_dsp_read(void* user, CPUState* cpu, u32 addr, u8 size) {
    (void)cpu; (void)size;
    GcnDsp* dsp = (GcnDsp*)user;
    u32 off = addr - GCN_DSP_BASE;
    switch (off) {
    case GCN_DSP_MBOX_IN_H:  return dsp_lle_read_mbox_hi(1);  /* CPU->DSP box */
    case GCN_DSP_MBOX_IN_L:  return dsp_lle_read_mbox_lo(1);
    case GCN_DSP_MBOX_OUT_H: return dsp_lle_read_mbox_hi(0);  /* DSP->CPU box */
    case GCN_DSP_MBOX_OUT_L: return dsp_lle_read_mbox_lo(0);
    case GCN_DSP_CSR:        return read_csr(dsp);
    default: break;
    }
    if (size == 4)
        return ((u32)dsp->reg[idx16(off)] << 16) | (u32)dsp->reg[idx16(off) + 1];
    return dsp->reg[idx16(off)];
}

void gcn_dsp_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size) {
    GcnDsp* dsp = (GcnDsp*)user;
    u32 off = addr - GCN_DSP_BASE;

    switch (off) {
    case GCN_DSP_MBOX_IN_H:  dsp_lle_write_mbox_hi((uint16_t)value); return;
    case GCN_DSP_MBOX_IN_L:  dsp_lle_write_mbox_lo((uint16_t)value); return;
    case GCN_DSP_MBOX_OUT_H: /* CPU cannot write the DSP->CPU mailbox */ return;
    case GCN_DSP_MBOX_OUT_L: return;
    case GCN_DSP_CSR:        write_csr(dsp, (u16)value); return;
    default: break;
    }

    /* store into the generic 16-bit backing (32-bit writes fill two halves) */
    if (size == 4) {
        dsp->reg[idx16(off)]     = (u16)(value >> 16);
        dsp->reg[idx16(off) + 1] = (u16)value;
    } else {
        dsp->reg[idx16(off)] = (u16)value;
    }

    /* the AR_CNT write kicks the ARAM DMA (MMADDR/ARADDR were set just before) */
    if (off == GCN_DSP_AR_CNT)
        aram_dma_kick(dsp, cpu, value);
}
