/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Video Interface (VI) model — register file + cycle-driven beam counter +
 * the four display-interrupt comparators (M2 device model, pre-rendering).
 *
 * The Flipper VI block (0xCC002000) is a grid of 16-bit registers at even
 * offsets (YAGCD 0xCC002000; Dolphin VideoInterface.h is the authoritative
 * register map and beam formula — every derived quantity below is transcribed
 * from VideoInterface.cpp, never invented).
 *
 * What the IPL does with it during boot: it writes the whole timing block
 * (vertical/horizontal timing, vblank, burst blanking, XFB base, filter
 * coefficients, clock, fbwidth), then busy-waits on VI_VERTICAL_BEAM_POSITION
 * (0x2C, guest pc 0x8130050C) until the beam sweeps. The beam therefore has to
 * MOVE, driven by guest time, computed from the timing values the IPL itself
 * wrote:
 *
 *   ticks_per_sample   = 2 * core_clock / (clock reg ? 54 MHz : 27 MHz)
 *   ticks_per_halfline = ticks_per_sample * HLW          (HTR0_LO[9:0])
 *   halflines per field = 3*EQU + PRB + 2*ACV + PSB      (per odd/even field)
 *   VI_VERTICAL_BEAM_POSITION reads 1 + half_line_count/2
 *
 * "ticks" are Gekko core cycles (486 MHz). The tick source is the MONOTONIC
 * device clock advanced per retired block by dispatch.c — deliberately NOT the
 * architectural time base: the guest can write the TB (mttbl/mttbu; the IPL
 * zeroes it during OS init), but the VI beam runs off its own 27 MHz pixel
 * clock, which keeps counting through a TB write. Beam state advances at block
 * granularity (~96 cycles/block « 15444 cycles/halfline), which is far below
 * the register's observable resolution. The absolute rate is nominal M0
 * timing — the oracle diff is value+order with poll collapse, insensitive to it.
 *
 * The four display-interrupt registers (DI0 pre-retrace 0x30, DI1 post-retrace
 * 0x34, DI2 0x38, DI3 0x3C) each hold a VCT/HCT comparator, an IR_MASK and a
 * latched IR_INT status bit. When the beam passes a comparator, IR_INT latches;
 * any (IR_INT && IR_MASK) asserts the level-triggered VI line into the PI
 * interrupt cause (INT_CAUSE_VI) via the irq callback — re-evaluated on every
 * halfline and on every interrupt-register write, exactly like Dolphin's
 * UpdateInterrupts. Software acks by writing the HI half with IR_INT clear.
 *
 * NOT modeled here (deferred until the beam/DI unblock the menu render loop):
 * XFB scanout to pixels, the debug-server screenshot, and CPU delivery of the
 * external-interrupt exception (the PI cause/mask word is modeled; delivery is
 * the next wall — see pi.h).
 */
#ifndef GCN_VI_VI_H
#define GCN_VI_VI_H

#include "cpu/cpu.h"

#define GCN_VI_BASE  0xCC002000u
#define GCN_VI_SIZE  0x100u          /* VI register block (16-bit regs at even offsets) */

/* 16-bit register offsets (Dolphin VideoInterface.h enum, YAGCD names). A
 * 32-bit architectural register X spans [X_HI, X_LO] = [off, off+2]: the HI
 * halfword holds bits 31:16 of the big-endian 32-bit value. */
#define GCN_VI_VTR                0x00u  /* vertical timing: EQU[3:0], ACV[13:4]   */
#define GCN_VI_DCR                0x02u  /* display control (ENB/RST/NIN/FMT...)   */
#define GCN_VI_HTR0_HI            0x04u  /* horizontal timing 0: HCE/HCS           */
#define GCN_VI_HTR0_LO            0x06u  /* horizontal timing 0: HLW[9:0]          */
#define GCN_VI_HTR1_HI            0x08u
#define GCN_VI_HTR1_LO            0x0Au
#define GCN_VI_VTO_HI             0x0Cu  /* odd-field vblank:  PSB[9:0] in HI      */
#define GCN_VI_VTO_LO             0x0Eu  /*                    PRB[9:0] in LO      */
#define GCN_VI_VTE_HI             0x10u  /* even-field vblank: PSB[9:0] in HI      */
#define GCN_VI_VTE_LO             0x12u  /*                    PRB[9:0] in LO      */
#define GCN_VI_BBOI_HI            0x14u  /* burst blanking odd interval            */
#define GCN_VI_BBEI_HI            0x18u  /* burst blanking even interval           */
#define GCN_VI_FB_LEFT_TOP_HI     0x1Cu  /* XFB top field base (POFF/CLRPOFF in HI)*/
#define GCN_VI_FB_RIGHT_TOP_HI    0x20u  /* 3D mode only                           */
#define GCN_VI_FB_LEFT_BOTTOM_HI  0x24u  /* XFB bottom field base                  */
#define GCN_VI_FB_RIGHT_BOTTOM_HI 0x28u  /* 3D mode only                           */
#define GCN_VI_VBEAM              0x2Cu  /* vertical beam position (computed, r/o) */
#define GCN_VI_HBEAM              0x2Eu  /* horizontal beam position (computed, r/o)*/
#define GCN_VI_DI0                0x30u  /* display interrupt 0 (pre-retrace)      */
#define GCN_VI_DI1                0x34u  /* display interrupt 1 (post-retrace)     */
#define GCN_VI_DI2                0x38u  /* display interrupt 2                    */
#define GCN_VI_DI3                0x3Cu  /* display interrupt 3                    */
#define GCN_VI_CLOCK              0x6Cu  /* pixel clock: 0 = 27 MHz, 1 = 54 MHz    */
#define GCN_VI_FBWIDTH            0x70u  /* horizontal stepping / src width        */

/* Display-control (DCR) bits. */
#define GCN_VI_DCR_ENB   0x0001u    /* enable video timing generation           */
#define GCN_VI_DCR_RST   0x0002u    /* reset: clears the DI regs, never stored  */
/* Bits stored on a DCR write: everything Dolphin copies (ENB, NIN, DLR,
 * LE0, LE1, FMT) — i.e. all defined bits except RST. */
#define GCN_VI_DCR_STORE_MASK 0x03FDu

/* Display-interrupt register fields. HI halfword (bits 31:16 of the u32):
 * VCT[10:0], IR_MASK bit 12, IR_INT bit 15 (latched status, ack by writing 0).
 * LO halfword: HCT[10:0]. */
#define GCN_VI_DI_VCT_MASK  0x07FFu
#define GCN_VI_DI_HCT_MASK  0x07FFu
#define GCN_VI_DI_IR_MASK   0x1000u
#define GCN_VI_DI_IR_INT    0x8000u

/* Timing constants (Dolphin SystemTimers: GC core clock). */
#define GCN_VI_CORE_CLOCK      486000000u

/* Level change on the VI->PI interrupt line (level: 1 assert, 0 deassert). */
typedef void (*GcnViIrqFn)(void* user, int level);

typedef struct {
    u16 reg[GCN_VI_SIZE / 2];     /* 16-bit register backing (off >> 1)       */
    u32 half_line_count;          /* halflines into the current frame         */
    u64 now_cycles;               /* device clock at the last tick            */
    u64 last_cycles;              /* cycle stamp of the last halfline edge    */
    u64 ticks_last_line_start;    /* cycle stamp of the current full line     */
    int irq_level;                /* last VI->PI line level (edge detect for the event ring) */
    GcnViIrqFn irq;               /* sink for the VI interrupt line (boot.c -> PI) */
    void*      irq_user;
} GcnVi;

void gcn_vi_init(GcnVi* vi);
void gcn_vi_set_irq(GcnVi* vi, GcnViIrqFn fn, void* user);
/* Advance the beam to `core_cycles` — the monotonic device clock from the
 * dispatch loop (called per block). MUST be monotonic; never the guest TB. */
void gcn_vi_tick(u64 core_cycles);
u32  gcn_vi_read(void* user, CPUState* cpu, u32 addr, u8 size);
void gcn_vi_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size);

#endif /* GCN_VI_VI_H */
