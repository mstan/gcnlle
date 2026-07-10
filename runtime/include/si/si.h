/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Serial Interface (SI) model — controllers + the SI/EXI clock lock.
 *
 * The SI drives the four controller ports. Register + bit semantics are
 * transcribed from Dolphin's SerialInterface (oracle/dolphin/.../SI/SI.{h,cpp}
 * and SI_DeviceGCController.cpp), our independent oracle.
 *
 * Two things the IPL boot path exercises:
 *   - SIEXILK (0xCC00643C): the SI/EXI clock lock — plain read/write control
 *     register (early boot writes it and reads it back).
 *   - SICOMCSR (0xCC006434) RDSTINT: the IPL reads the communication CSR and the
 *     oracle returns bit 28 (RDSTINT) set. RDSTINT is DERIVED: it is 1 whenever
 *     any channel's RDST ("read status") bit is set (SI.cpp UpdateInterrupts).
 *     RDST is latched by Dolphin's continuous VI-driven background poll whenever
 *     a connected controller's GetData() returns Success (SI.cpp:536) and cleared
 *     by reading that channel's input buffer (SI.cpp:344-357).
 *
 * Timing-free RDST model (PRINCIPLES: diff by value+order, never by timing): we
 * do NOT model the VI poll clock. Instead we exploit its invariant — a connected
 * controller returns Success on every poll, and polls happen continuously — so a
 * connected channel's RDST reads 1 at every SICOMCSR check, and is only
 * transiently 0 between an input-buffer read and the next check. We latch RDST
 * for connected channels, clear it on an input-buffer read, and re-arm it on the
 * next SICOMCSR read (a poll would certainly have run in between). Dolphin's
 * default config connects a standard controller on channel 0 only, so ch0 reads
 * RDST=1 (=> RDSTINT) and ch1-3 read 0. A wrong controller-presence choice
 * diverges loudly from the oracle — presence here is the modeled hardware state,
 * never a fudge.
 */
#ifndef GCN_SI_SI_H
#define GCN_SI_SI_H

#include "cpu/cpu.h"

#define GCN_SI_BASE      0xCC006400u
#define GCN_SI_SIZE      0x100u        /* channel regs + control + 128B I/O buf */
#define GCN_SI_CHANNELS  4u
#define GCN_SI_CH_STRIDE 0x0Cu         /* out / in_hi / in_lo per channel */

/* Register offsets (YAGCD / Dolphin SI.cpp:37-56). */
#define GCN_SI_POLL      0x30u         /* SIPOLL   */
#define GCN_SI_COMCSR    0x34u         /* SICOMCSR: communication control/status */
#define GCN_SI_SISR      0x38u         /* SISR: status (per-channel RDST + errors) */
#define GCN_SI_EXILK     0x3Cu         /* SIEXILK: SI/EXI clock lock */
#define GCN_SI_IOBUF     0x80u         /* SI I/O buffer (128 bytes) */

/* SICOMCSR bits (raw u32; SI.h:156-180). */
#define GCN_SI_CSR_TSTART     0x00000001u
#define GCN_SI_CSR_CHANNEL    0x00000006u  /* transfer channel select */
#define GCN_SI_CSR_INLNGTH    0x00007F00u
#define GCN_SI_CSR_OUTLNGTH   0x007F0000u
#define GCN_SI_CSR_RDSTINTMSK 0x08000000u
#define GCN_SI_CSR_RDSTINT    0x10000000u  /* read-status interrupt (derived) */
#define GCN_SI_CSR_COMERR     0x20000000u  /* communication error (no response) */
#define GCN_SI_CSR_TCINTMSK   0x40000000u
#define GCN_SI_CSR_TCINT      0x80000000u  /* transfer-complete interrupt (W1C) */

/* Writable/persistent SICOMCSR fields (the rest are status or W1C). */
#define GCN_SI_CSR_PERSIST \
    (GCN_SI_CSR_CHANNEL | GCN_SI_CSR_INLNGTH | GCN_SI_CSR_OUTLNGTH | \
     GCN_SI_CSR_RDSTINTMSK | GCN_SI_CSR_TCINTMSK)

/* SISR bits (SI.h:183-219). WR triggers SendCommand on all channels. */
#define GCN_SI_SR_WR          0x80000000u
/* RDST bit for channel n (SI.cpp GetRDSTBit): ch0=0x20000000 .. ch3=0x20. */
#define GCN_SI_RDST_BIT(n)    (0x20000000u >> ((n) * 8u))

/* Controller mode (SI_DeviceGCController.h:49): default 3. */
#define GCN_SI_CTRL_MODE_DEFAULT 3u

typedef struct {
    u32 out;         /* SI_CHANNEL_n_OUT  (poll/command word)      */
    u32 in_hi;       /* SI_CHANNEL_n_IN_HI                          */
    u32 in_lo;       /* SI_CHANNEL_n_IN_LO                          */
    u8  connected;   /* 1 = standard GC controller present          */
    u8  rdst;        /* latched read-status for this channel        */
    u8  mode;        /* controller reporting mode                   */
} GcnSiChannel;

/* Level change on the SI->PI interrupt line (level: 1 assert, 0 deassert). */
typedef void (*GcnSiIrqFn)(void* user, int level);

typedef struct {
    GcnSiChannel ch[GCN_SI_CHANNELS];
    u32 poll;        /* SIPOLL   */
    u32 comcsr;      /* SICOMCSR persistent bits (masks/lengths/TCINT/COMERR) */
    u32 sisr;        /* SISR error/WR bits (RDST computed on read)  */
    u32 exilk;       /* SIEXILK  */
    u8  iobuf[128];  /* SI I/O buffer for TSTART transfers          */
    int irq_level;   /* last SI->PI line level (edge detect for the ring) */
    GcnSiIrqFn irq;  /* sink for the SI interrupt line (boot.c -> PI) */
    void*      irq_user;
} GcnSi;

void gcn_si_init(GcnSi* si);
/* Register the SI interrupt-line sink (boot.c trampoline into PI INT_CAUSE_SI). */
void gcn_si_set_irq(GcnSi* si, GcnSiIrqFn fn, void* user);
u32  gcn_si_read(void* user, CPUState* cpu, u32 addr, u8 size);
void gcn_si_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size);

#endif /* GCN_SI_SI_H */
