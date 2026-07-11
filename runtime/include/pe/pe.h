/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pixel Engine (PE) model — the Flipper GPU's per-pixel/blend config block and
 * the token/finish interrupt latches, at 0xCC001000.
 *
 * Register map + semantics are transcribed from Dolphin's PixelEngineManager
 * (VideoCommon/PixelEngine.h:29-177, PixelEngine.cpp:27-252) — our independent
 * oracle — never invented:
 *   register offsets      : PixelEngine.h:29-58 (PE_ZCONF..PE_TOKEN_REG)
 *   directly-mapped RW     : PixelEngine.cpp:80-96 (ZCONF/ALPHACONF/DSTALPHACONF/
 *                            ALPHAMODE/ALPHAREAD are DirectRead/DirectWrite<u16>)
 *   CTRL write semantics   : PixelEngine.cpp:124-144 (ack the write-only token/
 *                            finish bits, store the two enable bits, force the
 *                            token/finish bits to 0, then UpdateInterrupts)
 *   TOKEN register (RO)    : PixelEngine.cpp:147 (DirectRead, InvalidWrite)
 *   interrupt line         : PixelEngine.cpp:161-172 (UpdateInterrupts drives the
 *                            two PI causes independently)
 *   SetToken/SetFinish     : PixelEngine.cpp:229-252 (entry points for the FIFO
 *                            parser; see gcn_pe_set_token/finish below)
 *   INT_CAUSE_PE_* values   : PixelEngine.cpp:29-31
 *
 * WHY THIS EXISTS NOW: the recompiled IPL's GX-init path writes PE ZCONF/
 * ALPHACONF/DSTALPHACONF/ALPHAMODE (0xCC001000..0x08) then reads CTRL back at
 * 0xCC00100A (and ALPHACONF at 0xCC001002). With no PE model those read-backs
 * hit the unmapped fallback (return 0) and the init path stalls. This model
 * gives the faithful read-back + interrupt state.
 *
 * INTERRUPTS ARE DORMANT THIS INCREMENT (faithful, not faked): the two signal
 * latches (m_signal_token/finish_interrupt) are set ONLY by SetToken/SetFinish,
 * which the video backend calls when the GPU processes a draw-done token/finish
 * command out of the FIFO. There is no FIFO parser yet, so UpdateInterrupts
 * always drives both PE->PI lines to 0 — which is correct: hardware only fires
 * them from processed commands, and the menu masks PE_TOKEN/PE_FINISH anyway
 * (GX_PLAN "The decisive evidence"). Register writes NEVER raise them (that
 * would be fake-the-answer). gcn_pe_set_token/finish exist wired but uncalled.
 */
#ifndef GCN_PE_PE_H
#define GCN_PE_PE_H

#include "cpu/cpu.h"

#define GCN_PE_BASE  0xCC001000u
#define GCN_PE_SIZE  0x1000u        /* full PE block (Dolphin registers within it) */

/* Register offsets (PixelEngine.h:29-58). */
#define GCN_PE_ZCONF        0x00u
#define GCN_PE_ALPHACONF    0x02u
#define GCN_PE_DSTALPHACONF 0x04u
#define GCN_PE_ALPHAMODE    0x06u
#define GCN_PE_ALPHAREAD    0x08u
#define GCN_PE_CTRL         0x0Au   /* control (token/finish enables + W-only acks) */
#define GCN_PE_TOKEN        0x0Eu   /* last token value (read-only)                 */

/* CTRL register bits (UPECtrlReg, PixelEngine.h:170-177). */
#define GCN_PE_CTRL_TOKEN_ENABLE  0x0001u  /* bit 0 */
#define GCN_PE_CTRL_FINISH_ENABLE 0x0002u  /* bit 1 */
#define GCN_PE_CTRL_TOKEN         0x0004u  /* bit 2, write-only: acks token int  */
#define GCN_PE_CTRL_FINISH        0x0008u  /* bit 3, write-only: acks finish int */
#define GCN_PE_CTRL_STORE_MASK    0x0003u  /* only the two enable bits persist   */

/* PE->PI interrupt cause bits (PixelEngine.cpp:29-31; identical to the PI cause
 * bits GCN_PI_INT_PE_TOKEN/FINISH in pi.h). Passed to the irq callback so the
 * boot.c trampoline maps them straight to gcn_pi_set_interrupt. */
#define GCN_PE_INT_TOKEN   0x0200u
#define GCN_PE_INT_FINISH  0x0400u

/* Level change on a PE->PI interrupt line (cause = GCN_PE_INT_TOKEN/FINISH;
 * level: 1 assert, 0 deassert). Called once per cause on every evaluation,
 * mirroring Dolphin UpdateInterrupts' two independent SetInterrupt calls. */
typedef void (*GcnPeIrqFn)(void* user, u32 cause_mask, int level);

typedef struct {
    u16 reg[GCN_PE_SIZE / 2];   /* 16-bit register backing (off >> 1)             */
    u16 token;                  /* m_token (RO; set only by gcn_pe_set_token)     */
    int token_enable;           /* m_control.pe_token_enable                       */
    int finish_enable;          /* m_control.pe_finish_enable                      */
    int signal_token_interrupt; /* m_signal_token_interrupt                        */
    int signal_finish_interrupt;/* m_signal_finish_interrupt                       */
    int token_level;            /* last PE_TOKEN line level (edge detect for ring) */
    int finish_level;           /* last PE_FINISH line level                       */
    GcnPeIrqFn irq;
    void*      irq_user;
} GcnPe;

void gcn_pe_init(GcnPe* pe);
void gcn_pe_set_irq(GcnPe* pe, GcnPeIrqFn fn, void* user);
u32  gcn_pe_read(void* user, CPUState* cpu, u32 addr, u8 size);
void gcn_pe_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size);

/* Video-backend entry points (PixelEngine.cpp:229-252). WIRED BUT UNCALLED this
 * increment — the future FIFO parser calls them when the GPU processes a
 * token/finish command. Dolphin schedules the effect via CoreTiming; with no
 * scheduler here the effect is applied synchronously (token latch + interrupt
 * signal + UpdateInterrupts), which is the same observable end state. */
void gcn_pe_set_token(GcnPe* pe, u16 token, int interrupt);
void gcn_pe_set_finish(GcnPe* pe);

#endif /* GCN_PE_PE_H */
