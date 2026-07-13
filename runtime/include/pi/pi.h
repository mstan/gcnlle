/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal Processor Interface (PI) model.
 *
 * The Flipper PI block (0xCC003000) is the CPU-side glue: interrupt
 * cause/mask (INTSR/INTMR), the GP FIFO pointers that feed the GX command
 * processor, a reset-code register, and a read-only chipset-revision register.
 *
 * During IPL boot the oracle shows exactly these PI accesses:
 *   stage-1  W 0xCC003030 = 0x0245248A   (a config/mem-controller word)
 *            R 0xCC003024 -> 0           (reset code, read before any write)
 *            W 0xCC003024 = 1, then = 3  (reset-code sequencing — ROADMAP M5:
 *                                        the =3 write has bit 2 clear, which
 *                                        is the real DVD-drive re-spin trigger,
 *                                        see reset_drive_hook/GcnPiResetDriveFn
 *                                        below and di.h's "drive re-spin
 *                                        trigger" note)
 *   stage-2  R 0xCC00302C -> 0x246500B1  (Flipper/PI chipset revision)
 *            W 0xCC003004 = 0xF0/0xF8    (interrupt mask, once the menu runs)
 *
 * Every WRITE is faithfully carried by value+order regardless of the model (the
 * MMIO layer traces the value the guest wrote). Every READ except the revision
 * register reads back a plain R/W control register (init 0), which is exactly
 * what the oracle shows. The ONE register with hardware-fixed read semantics is
 * PI_REVISION (0x2C): it is read-only and returns the console/chipset revision
 * ID. We hardwire it to the value the Dolphin oracle returns for a retail
 * GameCube (matching YAGCD's documented console-type register); modelling it is
 * what pushes the stage-2 lockstep past pc 0x813004B0.
 *
 * INTSR (0x00) is NOT plain read-back: it is the live interrupt-cause word.
 * Device models assert/deassert cause bits through gcn_pi_set_interrupt (the
 * VI display-interrupt line is the first user); reads return the live word;
 * writes are write-1-to-clear (Dolphin ProcessorInterface.cpp:77-82, cause &=
 * ~val). Level-driven sources re-assert on their next evaluation, so a W1C ack
 * of a still-asserted line reappears — exactly the hardware behaviour. The
 * power-on value is INT_CAUSE_RST_BUTTON | INT_CAUSE_VI (ProcessorInterface
 * Init:54 — RST_BUTTON high means "reset button not pressed").
 *
 * Delivery: gcn_pi_deliver_external (called by the dispatch loop at every
 * block boundary — the runtime's safe point) vectors the CPU to 0x500 via
 * ppc_take_exception when (cause & mask) is nonzero and MSR[EE] is set. It is
 * level-triggered exactly like the hardware: an unacked cause re-delivers as
 * soon as the handler's rfi restores MSR[EE] (Dolphin UpdateException:
 * (cause & mask) -> EXCEPTION_EXTERNAL_INT).
 *
 * Any other PI register whose real behaviour differs from plain read-back
 * (e.g. the FIFO write-pointer advancing on GX traffic) will DIVERGE from the
 * oracle loudly when the menu exercises it — that divergence is the signal to
 * model it, never a silent fake (PRINCIPLES: Runtime Boundaries).
 */
#ifndef GCN_PI_PI_H
#define GCN_PI_PI_H

#include "cpu/cpu.h"

#define GCN_PI_BASE  0xCC003000u
#define GCN_PI_SIZE  0x100u          /* PI register block (64 x u32) */

/* Register offsets (YAGCD, Flipper PI). */
#define GCN_PI_INTSR      0x00u       /* interrupt cause                 */
#define GCN_PI_INTMR      0x04u       /* interrupt mask                  */
#define GCN_PI_FIFO_BASE  0x0Cu       /* GP FIFO base start              */
#define GCN_PI_FIFO_END   0x10u       /* GP FIFO base end                */
#define GCN_PI_FIFO_WPTR  0x14u       /* GP FIFO current write pointer   */
#define GCN_PI_FIFO_RESET 0x18u       /* GP FIFO reset (GXAbortFrame)    */
#define GCN_PI_RESETCODE  0x24u       /* reset code / reset sequencing   */
#define GCN_PI_REVISION   0x2Cu       /* chipset revision (read-only)    */

/* Flipper/PI chipset revision, retail GameCube. Read-only constant returned by
 * PI_REVISION; observed from the Dolphin oracle and matching YAGCD's console-
 * type register. The IPL reads it once (stage-2) to identify the hardware. */
#define GCN_PI_REVISION_RETAIL  0x246500B1u

/* Interrupt-cause bits (INTSR/INTMR; Dolphin ProcessorInterface.h). */
#define GCN_PI_INT_ERROR       0x00001u
#define GCN_PI_INT_RSW         0x00002u   /* reset switch                   */
#define GCN_PI_INT_DI          0x00004u   /* disc interface                 */
#define GCN_PI_INT_SI          0x00008u   /* serial interface               */
#define GCN_PI_INT_EXI         0x00010u
#define GCN_PI_INT_AI          0x00020u   /* audio-interface streaming      */
#define GCN_PI_INT_DSP         0x00040u
#define GCN_PI_INT_MEMORY      0x00080u
#define GCN_PI_INT_VI          0x00100u   /* video interface (display ints) */
#define GCN_PI_INT_PE_TOKEN    0x00200u
#define GCN_PI_INT_PE_FINISH   0x00400u
#define GCN_PI_INT_CP          0x00800u
#define GCN_PI_INT_DEBUG       0x01000u
#define GCN_PI_INT_HSP         0x02000u
#define GCN_PI_INT_RST_BUTTON  0x10000u   /* 1 = reset button NOT pressed   */

/* PI_FIFO_RESET write hook (ProcessorInterface.cpp:112-119). Set from boot.c to
 * gp.c's gcn_gp_reset so pi.c never takes a dependency on gp.h — the same
 * dependency direction as the irq trampolines. */
typedef void (*GcnPiFifoResetFn)(void);

/* ROADMAP M5: PI_RESET_CODE write hook (ProcessorInterface.cpp:145-160). GC
 * path (not Wii): a write whose value has bit 2 clear (`~value & 0x4`) calls
 * DVDInterface::ResetDrive(true) — the real hardware "re-spin the drive"
 * signal. The IPL itself writes this register (=1 then =3) very early in
 * stage-1 boot (see this header's trace note below); 3 has bit 2 clear, so
 * THIS write is what advances a freshly-mounted disc from DiscChangeDetected
 * to DiscIdNotRead — not a mount-time special case (see di.h). Set from
 * boot.c to di.c's gcn_di_reset_drive_spinup so pi.c never takes a dependency
 * on di.h — same no-arg idiom as GcnPiFifoResetFn/gcn_gp_reset. */
typedef void (*GcnPiResetDriveFn)(void);

typedef struct {
    u32 reg[GCN_PI_SIZE / 4];
    u32 intsr;                  /* live interrupt-cause word (not in reg[]) */
    GcnPiFifoResetFn fifo_reset_hook;  /* gather-pipe reset (PI_FIFO_RESET) */
    GcnPiResetDriveFn reset_drive_hook; /* DVD drive re-spin (PI_RESET_CODE) */
} GcnPi;

void gcn_pi_init(GcnPi* pi);
/* Register the gather-pipe reset hook fired on a PI_FIFO_RESET write. */
void gcn_pi_set_fifo_reset_hook(GcnPi* pi, GcnPiFifoResetFn fn);
/* Register the DVD drive re-spin hook fired on a PI_RESET_CODE write with bit
 * 2 clear (see GcnPiResetDriveFn above). */
void gcn_pi_set_reset_drive_hook(GcnPi* pi, GcnPiResetDriveFn fn);
u32  gcn_pi_read(void* user, CPUState* cpu, u32 addr, u8 size);
void gcn_pi_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size);

/* Assert (set=1) or deassert (set=0) interrupt-cause bits. Called by device
 * models (VI today; DI/SI/EXI/DSP as their interrupt lines get wired). */
void gcn_pi_set_interrupt(GcnPi* pi, u32 cause_mask, int set);

/* Take the external-interrupt exception (vector 0x500) if (cause & mask) is
 * pending, MSR[EE] is set and no exception is mid-delivery. Called by the
 * dispatch loop at every block boundary. */
void gcn_pi_deliver_external(CPUState* cpu);

#endif /* GCN_PI_PI_H */
