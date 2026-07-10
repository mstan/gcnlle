/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * DSP interface + ARAM DMA model (Flipper DSP block, 0xCC005000).
 *
 * The block fronts three things the IPL touches during boot:
 *   - the 16-bit DSP mailboxes (CPU<->DSP microcode handshake),
 *   - the DSP control/status register (CSR, 0x0A), and
 *   - the ARAM (auxiliary 16 MB audio RAM) DMA engine (0x20/0x24/0x28).
 *
 * The IPL's DSP bring-up, exactly as the Dolphin oracle shows it:
 *   R  CSR      -> 0x0804   (power-on: HALT | INIT)
 *   W  CSR       = 0x08AC   (write-1-to-clear the AID/ARAM/DSP interrupt bits)
 *   W  CSR       = 0x0805   (RES=1: reset the DSP; the bit auto-clears)
 *   W  MBOX_IN, R MBOX_OUT  (mailbox handshake; no microcode -> reads 0)
 *   W  AR_MMADDR = 0x01000000, AR_ARADDR = 0, AR_CNT = 0x20   (kick a DMA)
 *   R  CSR      -> 0x0A04   (bit 9 DSPDMA set while the transfer runs)  ... xN
 *   R  CSR      -> 0x0824   (DSPDMA clears, bit 5 ARAM-DMA-done sets)
 *   W  CSR       = 0x0824   (write-1-to-clear the ARAM interrupt)
 *
 * DSPDMA (bit 9) is read-only and reflects the transfer being in flight. Real
 * hardware (and Dolphin) hold it for however many cycles the copy takes, so the
 * IPL's poll loop reads 0x0A04 a hardware-timing-dependent number of times
 * before it sees 0x0824. We do NOT model Gekko cycle timing, so we hold DSPDMA
 * for a small nominal number of CSR polls; the poll-aware oracle diff
 * (oracle/mmio_diff.py) collapses the identical poll reads on both sides, so the
 * exact count is immaterial to value+order matching (PRINCIPLES: diff by
 * value+order, never by timing). The transfer itself is performed for real.
 */
#ifndef GCN_DSP_DSP_H
#define GCN_DSP_DSP_H

#include "cpu/cpu.h"

#define GCN_DSP_BASE  0xCC005000u
#define GCN_DSP_SIZE  0x200u          /* DSP + AR + AI DMA register block */

/* Register offsets (YAGCD, Flipper DSP interface). */
#define GCN_DSP_MBOX_IN_H   0x00u     /* CPU->DSP mailbox high            */
#define GCN_DSP_MBOX_IN_L   0x02u     /* CPU->DSP mailbox low             */
#define GCN_DSP_MBOX_OUT_H  0x04u     /* DSP->CPU mailbox high (r/o)      */
#define GCN_DSP_MBOX_OUT_L  0x06u     /* DSP->CPU mailbox low  (r/o)      */
#define GCN_DSP_CSR         0x0Au     /* control/status register          */
#define GCN_DSP_AR_SIZE     0x12u     /* ARAM size/mode                   */
#define GCN_DSP_AR_MMADDR   0x20u     /* ARAM DMA main-memory address     */
#define GCN_DSP_AR_ARADDR   0x24u     /* ARAM DMA ARAM address            */
#define GCN_DSP_AR_CNT      0x28u     /* ARAM DMA control/length (kicks)  */

/* DSP CSR bits (Dolphin UDSPControl). */
#define GCN_DSP_CSR_RES      0x0001u  /* reset DSP (write-1, auto-clears) */
#define GCN_DSP_CSR_PIINT    0x0002u  /* assert PI interrupt to CPU       */
#define GCN_DSP_CSR_HALT     0x0004u  /* halt the DSP core                */
#define GCN_DSP_CSR_AIDINT   0x0008u  /* audio DMA interrupt (W1C)        */
#define GCN_DSP_CSR_AIDMASK  0x0010u  /* audio DMA interrupt mask         */
#define GCN_DSP_CSR_ARINT    0x0020u  /* ARAM DMA interrupt (W1C)         */
#define GCN_DSP_CSR_ARMASK   0x0040u  /* ARAM DMA interrupt mask          */
#define GCN_DSP_CSR_DSPINT   0x0080u  /* DSP mailbox interrupt (W1C)      */
#define GCN_DSP_CSR_DSPMASK  0x0100u  /* DSP mailbox interrupt mask       */
#define GCN_DSP_CSR_DMA      0x0200u  /* ARAM DMA in progress (read-only) */
#define GCN_DSP_CSR_INITCODE 0x0400u  /* init-code state                  */
#define GCN_DSP_CSR_INIT     0x0800u  /* DSP init                         */

/* CSR bits owned by the DSP core (Dolphin DSP.h DSP_CONTROL_MASK): RESET,
 * PIINT(assert-int), HALT, INITCODE, INIT. These are read from / written to the
 * real DSP via the dsp_lle C API; the remaining CSR bits (interrupt status +
 * masks + the ARAM-DMA in-progress bit) are CPU-side hardware, kept here. */
#define GCN_DSP_CONTROL_MASK  0x0C07u

/* Nominal number of CSR polls the ARAM DMA stays "in progress" for. The real
 * duration is Gekko cycle timing we don't model; the poll-aware diff collapses
 * the identical poll reads, so any value >= 1 matches by value+order. */
#define GCN_DSP_ARAM_DMA_NOMINAL_POLLS  4u

typedef struct {
    u16   reg[GCN_DSP_SIZE / 2];      /* generic 16-bit register backing  */
    u16   csr;                        /* CPU-side CSR bits (int status/masks) */
    bool  dma_active;                 /* ARAM DMA in flight               */
    u32   dma_polls_left;             /* CSR polls until it "completes"    */
} GcnDsp;

/* irom/coef are the raw big-endian DSP ROM bytes (dsp_rom.bin / dsp_coef.bin);
 * mem1 is the guest MEM1 backing the DSP DMAs against. */
void gcn_dsp_init(GcnDsp* dsp, const u8* irom, const u8* coef, u8* mem1, u32 mem1_size);
void gcn_dsp_free(GcnDsp* dsp);
void gcn_dsp_tick(u32 ppc_cycles);   /* advance the DSP core (called per block) */
u32  gcn_dsp_read(void* user, CPUState* cpu, u32 addr, u8 size);
void gcn_dsp_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size);

#endif /* GCN_DSP_DSP_H */
