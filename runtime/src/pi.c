/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Processor Interface (PI) model. See include/pi/pi.h for scope. A plain R/W
 * register file over the PI block, with the one hardware-fixed register —
 * PI_REVISION (0x2C, read-only chipset revision) — returning the console
 * revision constant instead of stored state.
 */
#include "pi/pi.h"
#include "debug/rings.h"
#include "dsp/dsp.h"   /* gcn_dsp_flush: catch the DSP core up before INTSR/INTMR observe it */

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

void gcn_pi_set_fifo_reset_hook(GcnPi* pi, GcnPiFifoResetFn fn) {
    pi->fifo_reset_hook = fn;
}

void gcn_pi_set_reset_drive_hook(GcnPi* pi, GcnPiResetDriveFn fn) {
    pi->reset_drive_hook = fn;
}

void gcn_pi_set_interrupt(GcnPi* pi, u32 cause_mask, int set) {
    if (set)
        pi->intsr |= cause_mask;
    else
        pi->intsr &= ~cause_mask;
}

#define PI_MSR_EE 0x00008000u   /* MSR[EE] (PPC bit 16), mirror of cpu_glue.c */

/* PI gather-pipe FIFO pointer mask. The base/end/write-pointer registers ignore
 * the upper address bits AND the low 5 bits (32-byte / GATHER_PIPE_SIZE
 * alignment) in hardware, so a burst-advanced write pointer reaches `end`
 * EXACTLY and the END->BASE wrap in gp.c fires. Transcribed from Dolphin
 * ProcessorInterface.cpp:91-110 (PI_FIFO_BASE/END/WPTR write handlers), where
 *   mask = CommandProcessor::GetPhysicalAddressMask(IsWii) & 0xFFFFFFE0.
 * GameCube (Flipper) physical-address mask is 0x03FFFFFF (CommandProcessor.h:155),
 * so the effective mask is 0x03FFFFE0. Without it the guest-written END
 * (0x0098307C = base+size-4, low bits set) is stored raw, the write-pointer wrap
 * `wptr == end` never matches a 32-aligned pointer, the write pointer marches
 * past the buffer while the read side (cp.c, masked to 0xFFE0) wraps at the
 * aligned end, and the GX reader picks up stale FIFO-buffer-start data. */
#define PI_FIFO_PTR_MASK 0x03FFFFE0u

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
    if (off == GCN_PI_INTSR) {
        /* The guest can poll the cause word with MSR[EE]=0 (no exception
         * delivery), so a batched DSP core must be caught up before this read
         * — the DSP cause bit (0x40, INT_CAUSE_DSP) has to be fresh even
         * though gcn_pi_deliver_external never ran to trigger a flush
         * indirectly. */
        gcn_dsp_flush_lazy_pi();
        return pi->intsr;               /* live interrupt-cause word */
    }
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
    if (off == GCN_PI_INTMR) {
        /* Unmasking a cause makes a pending-but-stale DSP interrupt (batched
         * debt not yet run) newly deliverable, so catch the core up before the
         * new mask takes effect — otherwise a real pending DSP condition could
         * be missed for one more block. */
        gcn_dsp_flush_lazy_pi();
        pi->reg[pi_index(addr)] = value;
        return;
    }
    if (off == GCN_PI_FIFO_RESET) {
        /* ProcessorInterface.cpp:112-119 (GXAbortFrame): bit 0 resets the
         * gather pipe. Read-only register otherwise (InvalidRead). */
        pi->reg[pi_index(addr)] = value;
        if ((value & 1u) && pi->fifo_reset_hook)
            pi->fifo_reset_hook();
        return;
    }
    if (off == GCN_PI_RESETCODE) {
        /* ProcessorInterface.cpp:145-160 PI_RESET_CODE ComplexWrite. GC path
         * only (!IsWii is always true here): a write with bit 2 clear
         * re-spins the DVD drive (ResetDrive(true)) — see pi.h's
         * reset_drive_hook doc comment and di.h's "drive re-spin trigger". */
        pi->reg[pi_index(addr)] = value;
        if ((~value & 0x4u) && pi->reset_drive_hook)
            pi->reset_drive_hook();
        return;
    }
    if (off == GCN_PI_FIFO_BASE || off == GCN_PI_FIFO_END ||
        off == GCN_PI_FIFO_WPTR) {
        /* Gather-pipe FIFO pointers: hardware ignores upper + low-5 bits so the
         * burst-advanced write pointer lands on `end` exactly and wraps
         * (ProcessorInterface.cpp:91-110). See PI_FIFO_PTR_MASK. */
        pi->reg[pi_index(addr)] = value & PI_FIFO_PTR_MASK;
        return;
    }
    pi->reg[pi_index(addr)] = value;
}
