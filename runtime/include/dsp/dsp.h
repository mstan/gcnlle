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

/* Audio DMA (MEM1 -> AI FIFO) registers (Dolphin DSP.cpp:63-67). */
#define GCN_DSP_AID_START_HI    0x30u /* source address hi (GCN: & 0x03FF) */
#define GCN_DSP_AID_START_LO    0x32u /* source address lo (& 0xFFE0)      */
#define GCN_DSP_AID_BLOCKS_LEN  0x34u /* "ever used?" — plain storage      */
#define GCN_DSP_AID_CTRL        0x36u /* enable (bit 15) + block count     */
#define GCN_DSP_AID_BLOCKS_LEFT 0x3Au /* read-only countdown               */

#define GCN_DSP_AID_ENABLE      0x8000u
#define GCN_DSP_AID_NUMBLOCKS   0x7FFFu

/* Audio-DMA block pacing. Real hardware consumes one 32-byte block at 4 kHz
 * (DSP.cpp:423 — 32 bytes at 4 kHz == 4 bytes at 32 kHz 16-bit stereo), i.e.
 * every 486 MHz / 4 kHz = 121500 core cycles. The dispatch loop ticks the DSP
 * once per block (96 device-clock cycles), so one audio block elapses every
 * 121500/96 ~= 1266 ticks. Nominal timing — the poll-collapsed oracle diff is
 * insensitive to the exact rate; what matters is that AID interrupts PACE the
 * guest's audio frame loop. */
#define GCN_DSP_AID_TICKS_PER_BLOCK 1266u

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

/* Level change on the DSP->PI interrupt line (level: 1 assert, 0 deassert). */
typedef void (*GcnDspIrqFn)(void* user, int level);

typedef struct {
    u16   reg[GCN_DSP_SIZE / 2];      /* generic 16-bit register backing  */
    u16   csr;                        /* CPU-side CSR bits (int status/masks) */
    bool  dma_active;                 /* ARAM DMA in flight               */
    u32   dma_polls_left;             /* CSR polls until it "completes"    */

    /* Audio DMA engine (Dolphin DSP.cpp AudioDMA / UpdateAudioDMA). The data
     * sink (host audio out) is deferred; this models the addressing and the
     * AID interrupt pacing the guest's audio frame loop is clocked by. */
    u32   aid_source;                 /* programmed source address         */
    u16   aid_ctrl;                   /* enable + block count              */
    u32   aid_cur_addr;               /* current block address             */
    u16   aid_blocks_left;            /* remaining 32-byte blocks          */
    u8    aid_int_pending;            /* fifo-start AID int, fires next tick */
    u32   aid_accum;                  /* tick accumulator (block pacing)   */

    int   irq_level;                  /* last DSP->PI line level (edge detect for the ring) */
    GcnDspIrqFn irq;                  /* sink for the DSP interrupt line (boot.c -> PI) */
    void*       irq_user;
} GcnDsp;

/* irom/coef are the raw big-endian DSP ROM bytes (dsp_rom.bin / dsp_coef.bin);
 * mem1 is the guest MEM1 backing the DSP DMAs against. */
void gcn_dsp_init(GcnDsp* dsp, const u8* irom, const u8* coef, u8* mem1, u32 mem1_size);
void gcn_dsp_set_irq(GcnDsp* dsp, GcnDspIrqFn fn, void* user);
void gcn_dsp_free(GcnDsp* dsp);
void gcn_dsp_tick(u32 ppc_cycles);   /* advance the DSP core (called per block) */
/* Run all owed DSP cycles now. Called before any CPU observation of DSP state
 * (DSP MMIO, PI INTSR/INTMR, debug queries). No-op when batching is off or
 * nothing is owed. Preserves the /6 remainder as still-owed. See dsp.c. */
void gcn_dsp_flush(void);
/* Rate-limited flush for READ-side observation points (DSP-block MMIO reads,
 * PI INTSR read / INTMR write): runs the core only once >= GCN_DSP_FLUSH_MIN
 * (default 96) PPC cycles are owed — the uniform-charging world's
 * per-poll-iteration staleness, preserved quantitatively now that derived
 * cycle accuracy makes real poll loops observe many times more often per
 * emulated second. WRITE paths keep the full gcn_dsp_flush (mail/CSR
 * ordering). GCN_DSP_FLUSH_MIN=0 restores flush-every-observation (A/B). */
void gcn_dsp_flush_lazy(void);
/* PI-side variant of the lazy flush (INTSR read / INTMR write). Identical to
 * gcn_dsp_flush_lazy except its declined (<min) branch does NOT wait out an
 * in-flight GCN_DSP_THREAD grant: those PI paths read/write only CPU-side PI
 * state — never core state — and interrupt visibility rides the per-tick
 * latch in both the threaded and synchronous designs, so there is nothing a
 * drain would make coherent there. Draining anyway (the first threaded
 * implementation did) serializes the CPU against the worker at every guest
 * INTSR idle-loop poll and forfeits nearly the whole async window. DSP-block
 * MMIO reads must keep the draining gcn_dsp_flush_lazy: their declined
 * branch still reads core state (mailboxes/CSR) directly afterwards.
 * Single-threaded (GCN_DSP_THREAD unset) the two are byte-identical. */
void gcn_dsp_flush_lazy_pi(void);
/* Raise the lazy-flush threshold (derived mode only — dispatch.c calls this
 * with the uniform world's 96-cycle per-iteration staleness once it resolves
 * that real per-block cycle counts are in effect). Never called => 0 =>
 * gcn_dsp_flush_lazy is byte-identical to gcn_dsp_flush (legacy contract).
 * GCN_DSP_FLUSH_MIN overrides either way. */
void gcn_dsp_set_flush_min(u32 ppc_cycles);
u32  gcn_dsp_read(void* user, CPUState* cpu, u32 addr, u8 size);
void gcn_dsp_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size);

#endif /* GCN_DSP_DSP_H */
