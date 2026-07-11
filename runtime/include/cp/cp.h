/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Command Processor (CP) model — the Flipper GX FIFO front-end at 0xCC000000.
 *
 * The CP owns the GX command FIFO: a base/end window in main memory, a
 * read/write pointer pair, hi/lo watermarks, a breakpoint, and a status/control
 * pair whose bits gate the CP->PI interrupt (INT_CAUSE_CP = 0x800). Every
 * register, mask and formula below is transcribed from Dolphin's
 * CommandProcessorManager — our independent oracle — never invented:
 *   register offsets       : CommandProcessor.h:56-97 (STATUS..CLKS_PER_VTX_OUT)
 *   status/ctrl/clear bits  : CommandProcessor.h:104-153 (UCPStatusReg/UCPCtrlReg/
 *                             UCPClearReg)
 *   FIFO reg write masks    : CommandProcessor.cpp:130-163 (LO halves 0xffe0, HI
 *                             halves 0x03ff on GCN via GetPhysicalAddressMask>>16)
 *   metrics constants       : CommandProcessor.cpp:165-191 (all 0 except
 *                             CLKS_PER_VTX_OUT = 4)
 *   STATUS computed on read : CommandProcessor.cpp:552-564 (SetCpStatusRegister)
 *   CTRL write (val & 0x3F)  : CommandProcessor.cpp:201-208 + SetCpControlRegister
 *                             :574-595 (decode into the bFF_* enables)
 *   CLEAR write no-op        : CommandProcessor.cpp:210-217 + SetCpClearRegister
 *                             :597-601 (Dolphin intentionally does nothing)
 *   CPU-side status/interrupt: CommandProcessor.cpp:514-550 (SetCPStatusFromCPU)
 *                             + UpdateInterrupts:409-426 (single-core path)
 *   gather-pipe burst        : CommandProcessor.cpp:339-407 (GatherPipeBursted,
 *                             single-core) — driven from gp.c, see below
 *   INT_CAUSE_CP value       : CommandProcessor.h:99-102
 *
 * STATUS (0x00) is ALWAYS computed from live FIFO pointer state on read — never
 * a stored fake "idle". ReadIdle/CommandIdle/watermark bits fall straight out of
 * the SetCpStatusRegister formula over the guest-written pointers/watermarks.
 *
 * CPU-side interrupt (real, not faked — GX_PLAN "(a)"): the hi/lo watermark
 * breach is computed purely from CPReadWriteDistance vs the watermark registers,
 * which the CPU side owns. When gp.c bursts advance the distance (or the guest
 * writes a pointer/watermark/CTRL register), we recompute the breach and, gated
 * exactly as Dolphin gates it (GPReadEnable + the respective *IntEnable bits),
 * drive INT_CAUSE_CP into the PI. No GPU consumer is needed for this line.
 *
 * DELIBERATELY DEFERRED (diverges loudly if exercised, never silently faked):
 *   - No FIFO consumer: the read pointer does NOT advance and
 *     CPReadWriteDistance climbs unbounded. Dolphin ASSERTs on overflow
 *     (CommandProcessor.cpp:390-394); we do not auto-advance the read pointer.
 *     If the guest nears the hi-watermark or polls STATUS for idle it diverges
 *     loudly — the signal the FIFO interpreter is due (GX_PLAN deferral #2).
 *   - Breakpoint interrupt: bFF_Breakpoint is a GPU-side flag (SetCPStatusFromGPU
 *     :442-476); with no consumer it stays 0, so the BP interrupt is dormant.
 */
#ifndef GCN_CP_CP_H
#define GCN_CP_CP_H

#include "cpu/cpu.h"

#define GCN_CP_BASE  0xCC000000u
#define GCN_CP_SIZE  0x1000u

/* Register offsets (CommandProcessor.h:56-97). */
#define GCN_CP_STATUS              0x00u   /* RO, computed on read              */
#define GCN_CP_CTRL                0x02u   /* RW, val & 0x3F                    */
#define GCN_CP_CLEAR               0x04u   /* W (no-op), reads 0                */
#define GCN_CP_PERF_SELECT         0x06u   /* RW, mask 0x0007                   */
#define GCN_CP_UNK_0A              0x0Au   /* RW, mask 0x00FF                   */
#define GCN_CP_FIFO_BASE_LO        0x20u
#define GCN_CP_FIFO_BASE_HI        0x22u
#define GCN_CP_FIFO_END_LO         0x24u
#define GCN_CP_FIFO_END_HI         0x26u
#define GCN_CP_FIFO_HI_WM_LO       0x28u
#define GCN_CP_FIFO_HI_WM_HI       0x2Au
#define GCN_CP_FIFO_LO_WM_LO       0x2Cu
#define GCN_CP_FIFO_LO_WM_HI       0x2Eu
#define GCN_CP_FIFO_RW_DIST_LO     0x30u
#define GCN_CP_FIFO_RW_DIST_HI     0x32u
#define GCN_CP_FIFO_WPTR_LO        0x34u
#define GCN_CP_FIFO_WPTR_HI        0x36u
#define GCN_CP_FIFO_RPTR_LO        0x38u
#define GCN_CP_FIFO_RPTR_HI        0x3Au
#define GCN_CP_FIFO_BP_LO          0x3Cu
#define GCN_CP_FIFO_BP_HI          0x3Eu
#define GCN_CP_CLKS_PER_VTX_OUT    0x64u   /* metrics constant = 4              */

#define GCN_CP_FIFO_REG_FIRST      0x20u   /* [FIRST, LAST] are the FIFO regs   */
#define GCN_CP_FIFO_REG_LAST       0x3Eu

/* FIFO register write masks (CommandProcessor.cpp:130-163, GameCube). */
#define GCN_CP_WMASK_LO   0xFFE0u          /* WMASK_LO_ALIGN_32BIT              */
#define GCN_CP_WMASK_HI   0x03FFu          /* GetPhysicalAddressMask(false)>>16 */

/* CTRL register bits (UCPCtrlReg, CommandProcessor.h:122-137). */
#define GCN_CP_CTRL_GPREAD_ENABLE   0x0001u  /* bit 0 */
#define GCN_CP_CTRL_BP_ENABLE       0x0002u  /* bit 1 */
#define GCN_CP_CTRL_OVF_INT_ENABLE  0x0004u  /* bit 2 (FifoOverflowIntEnable)   */
#define GCN_CP_CTRL_UNF_INT_ENABLE  0x0008u  /* bit 3 (FifoUnderflowIntEnable)  */
#define GCN_CP_CTRL_GPLINK_ENABLE   0x0010u  /* bit 4 */
#define GCN_CP_CTRL_BP_INT          0x0020u  /* bit 5 */
#define GCN_CP_CTRL_MASK            0x003Fu  /* val & 0x3F (CommandProcessor.cpp:204) */

/* STATUS register bits (UCPStatusReg, CommandProcessor.h:104-119). */
#define GCN_CP_STATUS_OVF_HI_WM     0x0001u  /* bit 0 OverflowHiWatermark       */
#define GCN_CP_STATUS_UNF_LO_WM     0x0002u  /* bit 1 UnderflowLoWatermark      */
#define GCN_CP_STATUS_READ_IDLE     0x0004u  /* bit 2 ReadIdle                  */
#define GCN_CP_STATUS_CMD_IDLE      0x0008u  /* bit 3 CommandIdle               */
#define GCN_CP_STATUS_BREAKPOINT    0x0010u  /* bit 4 Breakpoint                */

#define GCN_CP_GATHER_PIPE_SIZE     32u      /* GPFifo::GATHER_PIPE_SIZE        */

/* CP->PI interrupt cause (CommandProcessor.h:99-102; == GCN_PI_INT_CP 0x800). */
#define GCN_CP_INT_CAUSE            0x0800u

/* Level change on the CP->PI interrupt line (level: 1 assert, 0 deassert). */
typedef void (*GcnCpIrqFn)(void* user, int level);

typedef struct {
    u16 reg[GCN_CP_SIZE / 2];    /* raw 16-bit backing (CTRL read-back, FIFO regs,
                                  * PERF/UNK, metrics constants)                */
    /* Control enables decoded from CTRL (SetCpControlRegister:574-595). */
    int gp_read_enable;          /* bFF_GPReadEnable  */
    int bp_enable;               /* bFF_BPEnable      */
    int hi_wm_int;               /* bFF_HiWatermarkInt (FifoOverflowIntEnable)  */
    int lo_wm_int;               /* bFF_LoWatermarkInt (FifoUnderflowIntEnable) */
    int gp_link_enable;          /* bFF_GPLinkEnable  */
    int bp_int;                  /* bFF_BPInt         */
    /* Watermark breach flags (recomputed each evaluation). */
    int hi_wm;                   /* bFF_HiWatermark   */
    int lo_wm;                   /* bFF_LoWatermark   */
    int breakpoint;              /* bFF_Breakpoint (GPU-side; 0, no consumer)   */
    int irq_level;               /* last CP->PI line level (edge detect for ring) */
    GcnCpIrqFn irq;
    void*      irq_user;
} GcnCp;

void gcn_cp_init(GcnCp* cp);
void gcn_cp_set_irq(GcnCp* cp, GcnCpIrqFn fn, void* user);
u32  gcn_cp_read(void* user, CPUState* cpu, u32 addr, u8 size);
void gcn_cp_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size);

/* Called by gp.c on each 32-byte gather-pipe burst (CommandProcessor.cpp
 * GatherPipeBursted:339-407, single-core). Runs the CPU-side status evaluation
 * (which may drive INT_CAUSE_CP), then — iff GPLinkEnable — mirrors the PI
 * write pointer into CPWritePointer and adds 32 to CPReadWriteDistance. */
void gcn_cp_gather_pipe_bursted(GcnCp* cp, u32 pi_write_pointer);

/* ---- GPU-side FIFO consumer support (used by gx.c) ----
 * Fifo.cpp RunGpuLoop:286-354 is the single-core GPU loop; these expose the
 * pieces of it the (synchronous) consumer needs without gx.c reaching into the
 * static cp_u32 helper. Read-only getters plus one mutator that advances the
 * read side of the FIFO exactly as the GPU thread does. */
u32  gcn_cp_fifo_read_pointer(const GcnCp* cp);   /* CPReadPointer (physical)      */
u32  gcn_cp_fifo_rw_distance(const GcnCp* cp);     /* CPReadWriteDistance           */
int  gcn_cp_at_breakpoint(const GcnCp* cp);        /* AtBreakpoint (Fifo.cpp:404-411)*/

/* Consume one 32-byte chunk from the GPU side (Fifo.cpp:326-354): advance the
 * read pointer wrapping CPEnd->CPBase, subtract GATHER_PIPE_SIZE from the RW
 * distance (clamped at 0 — Dolphin ASSERTs on underflow, we saturate + note),
 * then re-run the CPU/GPU status + interrupt evaluation. Call AFTER reading the
 * 32 bytes at the current read pointer. */
void gcn_cp_gpu_consume_chunk(GcnCp* cp);

#endif /* GCN_CP_CP_H */
