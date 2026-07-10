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
 *            W 0xCC003024 = 1, then = 3  (reset-code sequencing)
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
 * Any PI register whose real behaviour differs from plain read-back (e.g. the
 * FIFO write-pointer advancing on GX traffic, or INTSR reflecting live
 * interrupt state) will DIVERGE from the oracle loudly when the menu exercises
 * it — that divergence is the signal to model it, never a silent fake
 * (PRINCIPLES: Runtime Boundaries).
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
#define GCN_PI_RESETCODE  0x24u       /* reset code / reset sequencing   */
#define GCN_PI_REVISION   0x2Cu       /* chipset revision (read-only)    */

/* Flipper/PI chipset revision, retail GameCube. Read-only constant returned by
 * PI_REVISION; observed from the Dolphin oracle and matching YAGCD's console-
 * type register. The IPL reads it once (stage-2) to identify the hardware. */
#define GCN_PI_REVISION_RETAIL  0x246500B1u

typedef struct { u32 reg[GCN_PI_SIZE / 4]; } GcnPi;

void gcn_pi_init(GcnPi* pi);
u32  gcn_pi_read(void* user, CPUState* cpu, u32 addr, u8 size);
void gcn_pi_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size);

#endif /* GCN_PI_PI_H */
