/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Processor Interface (PI) model. See include/pi/pi.h for scope. A plain R/W
 * register file over the PI block, with the one hardware-fixed register —
 * PI_REVISION (0x2C, read-only chipset revision) — returning the console
 * revision constant instead of stored state.
 */
#include "pi/pi.h"
#include "debug/rings.h"

#include <string.h>

/* The dispatch loop delivers interrupts against one PI (like the one DSP core
 * and the one VI); registered at init. */
static GcnPi* s_pi = NULL;

void gcn_pi_init(GcnPi* pi) {
    memset(pi, 0, sizeof *pi);
    /* Power-on cause word (Dolphin ProcessorInterface Init:54): reset button
     * released (RST_BUTTON high) + the VI bit, which stays set until the
     * first VI interrupt-register evaluation deasserts the line. */
    pi->intsr = GCN_PI_INT_RST_BUTTON | GCN_PI_INT_VI;
    s_pi = pi;
}

static u32 pi_index(u32 addr) {
    return (addr - GCN_PI_BASE) >> 2;   /* bus guarantees addr in [base, base+size) */
}

void gcn_pi_set_interrupt(GcnPi* pi, u32 cause_mask, int set) {
    if (set)
        pi->intsr |= cause_mask;
    else
        pi->intsr &= ~cause_mask;
}

#define PI_MSR_EE 0x00008000u   /* MSR[EE] (PPC bit 16), mirror of cpu_glue.c */

void gcn_pi_deliver_external(CPUState* cpu) {
    if (!s_pi)
        return;
    if (cpu->exception)
        return;                       /* an exception is already mid-delivery */
    if (!(cpu->msr & PI_MSR_EE))
        return;
    u32 pending = s_pi->intsr & s_pi->reg[GCN_PI_INTMR >> 2];
    if (!pending)
        return;
    /* Block boundaries are the runtime's safe points: ctx->pc is the next
     * instruction to execute, which is exactly SRR0 for an external interrupt.
     * ppc_take_exception clears MSR[EE], so re-delivery waits for rfi; a
     * still-pending (unacked) cause then re-delivers — level-triggered. */
    gcn_ring_event(GCN_EV_IRQ_RAISE, /*source*/ 0xEEu /* CPU vectoring 0x500 */,
                   pending, cpu->pc);
    ppc_take_exception(cpu, PPC_EXC_EXTERNAL, PPC_VECTOR_EXTERNAL, cpu->pc, 0);
}

u32 gcn_pi_read(void* user, CPUState* cpu, u32 addr, u8 size) {
    (void)cpu; (void)size;
    GcnPi* pi = (GcnPi*)user;
    u32 off = addr - GCN_PI_BASE;
    if (off == GCN_PI_INTSR)
        return pi->intsr;               /* live interrupt-cause word */
    if (off == GCN_PI_REVISION)
        return GCN_PI_REVISION_RETAIL;  /* read-only chipset revision */
    return pi->reg[pi_index(addr)];
}

void gcn_pi_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size) {
    (void)cpu; (void)size;
    GcnPi* pi = (GcnPi*)user;
    u32 off = addr - GCN_PI_BASE;
    if (off == GCN_PI_INTSR) {
        pi->intsr &= ~value;            /* write-1-to-clear; level sources re-assert */
        return;
    }
    pi->reg[pi_index(addr)] = value;
}
