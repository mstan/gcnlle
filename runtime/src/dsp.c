/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * DSP interface + ARAM DMA model. See include/dsp/dsp.h for scope and the
 * oracle-observed boot sequence this reproduces.
 */
#include "dsp/dsp.h"

#include <stdlib.h>
#include <string.h>

void gcn_dsp_init(GcnDsp* dsp) {
    memset(dsp, 0, sizeof *dsp);
    dsp->csr = GCN_DSP_CSR_RESET_VALUE;   /* power-on: HALT | INIT (0x0804) */
    dsp->aram = (u8*)calloc(1, GCN_ARAM_SIZE);
}

void gcn_dsp_free(GcnDsp* dsp) {
    free(dsp->aram);
    dsp->aram = NULL;
}

static u16 idx16(u32 off) { return (u16)(off >> 1); }

/* Perform the ARAM DMA the guest just kicked. AR_CNT (0x28) holds the length in
 * its low bits; bit 31 selects direction (0: MRAM->ARAM, 1: ARAM->MRAM). MMADDR
 * is a physical MEM1 offset. The transfer is done for real; DSPDMA then reads
 * as in-progress for a nominal number of CSR polls (see header). */
static void aram_dma_kick(GcnDsp* dsp, CPUState* cpu, u32 cnt) {
    u32 mmaddr = ((u32)dsp->reg[idx16(GCN_DSP_AR_MMADDR)]     << 16) |
                  (u32)dsp->reg[idx16(GCN_DSP_AR_MMADDR) + 1];
    u32 araddr = ((u32)dsp->reg[idx16(GCN_DSP_AR_ARADDR)]     << 16) |
                  (u32)dsp->reg[idx16(GCN_DSP_AR_ARADDR) + 1];
    u32 len    = cnt & 0x03FFFFFFu;
    int to_aram = ((cnt & 0x80000000u) == 0);   /* 0 => write into ARAM */

    if (dsp->aram && len &&
        (u64)mmaddr + len <= cpu->ram_size &&
        (u64)araddr + len <= GCN_ARAM_SIZE) {
        if (to_aram)
            memcpy(dsp->aram + araddr, cpu->ram + mmaddr, len);
        else
            memcpy(cpu->ram + mmaddr, dsp->aram + araddr, len);
    }

    dsp->dma_active = true;
    dsp->dma_polls_left = GCN_DSP_ARAM_DMA_NOMINAL_POLLS;
}

static u32 read_csr(GcnDsp* dsp) {
    u32 v = dsp->csr | (dsp->dma_active ? GCN_DSP_CSR_DMA : 0u)
                     | (dsp->initcode_active ? GCN_DSP_CSR_INITCODE : 0u);
    if (dsp->dma_active && --dsp->dma_polls_left == 0) {
        dsp->dma_active = false;
        dsp->csr |= GCN_DSP_CSR_ARINT;   /* DMA complete raises the ARAM int */
    }
    if (dsp->initcode_active && --dsp->initcode_polls_left == 0)
        dsp->initcode_active = false;    /* init microcode boot window ends */
    return v;
}

static void write_csr(GcnDsp* dsp, u16 w) {
    bool init_was_set = (dsp->csr & GCN_DSP_CSR_INIT) != 0;
    /* write-1-to-clear the interrupt status bits */
    dsp->csr &= (u16)~(w & (GCN_DSP_CSR_AIDINT | GCN_DSP_CSR_ARINT | GCN_DSP_CSR_DSPINT));
    /* update the read/write control bits from the write. INITCODE (bit 10) is
     * hardware-driven (not guest-writable) and DMA (bit 9) is read-only — both
     * are computed on read, never stored. */
    const u16 CTRL = GCN_DSP_CSR_PIINT | GCN_DSP_CSR_HALT |
                     GCN_DSP_CSR_AIDMASK | GCN_DSP_CSR_ARMASK | GCN_DSP_CSR_DSPMASK |
                     GCN_DSP_CSR_INIT;
    dsp->csr = (u16)((dsp->csr & ~CTRL) | (w & CTRL));
    /* RES (auto-clears) is never stored. A reset restarts the ROM microcode,
     * discarding any queued DSP->CPU mail (Dolphin ClearPending on SetUCode). */
    if (w & GCN_DSP_CSR_RES)
        dsp->mail_pending = false;
    /* Clearing DSPInit (1->0) boots the init microcode; DSPInitCode reports set
     * for a short window afterward (Dolphin DSPHLE.cpp:214-227). */
    if (init_was_set && !(w & GCN_DSP_CSR_INIT)) {
        dsp->initcode_active = true;
        dsp->initcode_polls_left = GCN_DSP_INITCODE_NOMINAL_POLLS;
        /* The init microcode posts its boot mail to the CPU (INIT.cpp:21). */
        dsp->mail_value = GCN_DSP_INIT_BOOT_MAIL;
        dsp->mail_pending = true;
    }
}

/* DSP->CPU mailbox reads (Dolphin CMailHandler). Mail is visible only when the
 * DSP is not halted; the CPU reads the high word (peek) then the low word (which
 * consumes the mail and clears the ready bit for subsequent high reads). */
static u16 read_mbox_out_hi(GcnDsp* dsp) {
    bool halted = (dsp->csr & GCN_DSP_CSR_HALT) != 0;
    if (!halted && dsp->mail_pending)
        dsp->mail_last = dsp->mail_value;
    return (u16)(dsp->mail_last >> 16);
}

static u16 read_mbox_out_lo(GcnDsp* dsp) {
    bool halted = (dsp->csr & GCN_DSP_CSR_HALT) != 0;
    if (!halted && dsp->mail_pending) {
        dsp->mail_last = dsp->mail_value;
        dsp->mail_pending = false;          /* mail consumed */
    }
    dsp->mail_last &= ~GCN_DSP_MAIL_READY;  /* clear ready bit after low read */
    return (u16)(dsp->mail_last & 0xFFFFu);
}

u32 gcn_dsp_read(void* user, CPUState* cpu, u32 addr, u8 size) {
    (void)cpu; (void)size;
    GcnDsp* dsp = (GcnDsp*)user;
    u32 off = addr - GCN_DSP_BASE;
    switch (off) {
    case GCN_DSP_CSR:        return read_csr(dsp);
    case GCN_DSP_MBOX_OUT_H: return read_mbox_out_hi(dsp);
    case GCN_DSP_MBOX_OUT_L: return read_mbox_out_lo(dsp);
    default: break;
    }
    if (size == 4)
        return ((u32)dsp->reg[idx16(off)] << 16) | (u32)dsp->reg[idx16(off) + 1];
    return dsp->reg[idx16(off)];
}

void gcn_dsp_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size) {
    GcnDsp* dsp = (GcnDsp*)user;
    u32 off = addr - GCN_DSP_BASE;

    if (off == GCN_DSP_CSR) { write_csr(dsp, (u16)value); return; }

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
