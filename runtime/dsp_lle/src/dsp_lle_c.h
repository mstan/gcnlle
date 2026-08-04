/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * C API over the vendored Dolphin DSP-LLE interpreter core. runtime/src/dsp.c
 * (the 0xCC005000 MMIO device) calls these; the mailbox + control-register
 * semantics mirror Dolphin's DSPLLE HW driver, wired to the real firmware. */
#ifndef GCN_DSP_LLE_C_H
#define GCN_DSP_LLE_C_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the DSP core with the real IROM/DROM (big-endian ROM bytes) and the
 * host MEM1 backing (physical, 0-based) the DSP DMAs against. */
void     dsp_lle_init(const uint8_t* irom_be, const uint8_t* coef_be,
                      uint8_t* mem1, uint32_t mem1_size);
void     dsp_lle_shutdown(void);

/* DSP control register (the DSP-owned bits, DSP_CONTROL_MASK). */
uint16_t dsp_lle_read_control(void);
uint16_t dsp_lle_write_control(uint16_t value);

/* Mailboxes. cpu_mailbox != 0 selects the CPU->DSP box, 0 the DSP->CPU box. */
uint16_t dsp_lle_read_mbox_hi(int cpu_mailbox);
uint16_t dsp_lle_read_mbox_lo(int cpu_mailbox);
void     dsp_lle_write_mbox_hi(uint16_t value);
void     dsp_lle_write_mbox_lo(uint16_t value);
uint32_t dsp_lle_peek_mbox_cpu(void);

/* Advance the DSP; ppc_cycles is the PPC-side period (DSP runs ~1/6th). */
void     dsp_lle_update(int ppc_cycles);

/* Raw CR_HALT test on control_reg — the exact condition dsp_lle_update's
 * halt-skip fast path checks (NOT dsp_lle_read_control, which carries
 * CR_INIT_CODE timebase side effects). Lets the GCN_DSP_THREAD grant path
 * discard halted-core windows without a worker round trip; caller must hold
 * the core quiesced (worker drained). */
int      dsp_lle_halted(void);


/* Observability (debug_server "dsp_state"): current DSP program counter and a
 * non-consuming peek of the DSP->CPU mailbox (Dolphin Mailbox::DSP — the box
 * the DSP posts into via DMBH/DMBL; bit 31 = mail pending). The CPU->DSP box
 * peek is dsp_lle_peek_mbox_cpu above (Mailbox::CPU). */
uint16_t dsp_lle_pc(void);
uint32_t dsp_lle_peek_mbox_dsp(void);

/* Consume a pending DSP->CPU interrupt request (1 if one fired since last call). */
int      dsp_lle_take_interrupt(void);

/* The 16 MB ARAM, shared with the CPU-side AR DMA engine (dsp.c). */
uint8_t* dsp_lle_aram(void);
uint32_t dsp_lle_aram_size(void);

/* SNAPSHOT_RESUME pass A (docs/SNAPSHOT_RESUME.md): hand-rolled, exhaustive
 * field-by-field save of the DSP-LLE core, deliberately NOT routed through
 * SDSP::DoState/DSPCore::DoState (Core/DSP/DSPCore.cpp:390-418,600-606) —
 * Common/ChunkFile.h's PointerWrap is a no-op stub in this runtime (every
 * Do/DoArray/DoPOD is an empty template, IsReadMode() always false), so
 * DoState is never actually invoked anywhere in the wrapper. This struct
 * covers everything DoState's own field list would have serialized PLUS the
 * two gaps found in the SAVE-side survey: DSPCore::m_core_state (never
 * DoState'd upstream) and Accelerator::m_reads_stopped (internal ACCOV latch,
 * "not exposed via any register", also never DoState'd upstream) — plus the
 * cross-thread g_dsp_int_pending flag (dsp_lle_c.cpp), which is runtime-only
 * state with no SDSP/DSPCore equivalent at all. irom/coef are deliberately
 * NOT included: both are static ROM content reloaded from the same on-disk
 * dumps at every gcn_dsp_init, never mutated at runtime (DSPCore.cpp's own
 * DoState excludes them for the same reason). Sizes are fixed compile-time
 * constants (DSP_IRAM_SIZE/DSP_DRAM_SIZE/DSP_STACK_DEPTH, DSPCore.h) — kept
 * as literal array sizes here (rather than including DSPCore.h from a public
 * C header) with a static_assert in the .cpp pinning them against upstream. */
typedef struct {
    /* DSP_Regs r (DSPCore.h:244-285). */
    uint16_t r_ar[4];
    uint16_t r_ix[4];
    uint16_t r_wr[4];
    uint16_t r_st[4];
    uint16_t r_cr;
    uint16_t r_sr;
    uint64_t r_prod;        /* r.prod.val                                   */
    uint32_t r_ax[2];       /* r.ax[i].val                                  */
    uint64_t r_ac[2];       /* r.ac[i].val (low 48 bits meaningful)         */

    uint16_t pc;
    uint16_t control_reg;
    uint64_t control_reg_init_code_clear_time;
    uint8_t  reg_stack_ptrs[4];
    uint8_t  exceptions;             /* pending-exception bitmask            */
    uint8_t  external_interrupt_waiting;
    uint8_t  reset_dspjit_codespace; /* JIT-only; always 0 (Interpreter core)*/
    uint16_t reg_stacks[4][32];      /* DSP_STACK_DEPTH = 0x20                */

    uint16_t ifx_regs[256];          /* accelerator/DMA hardware regs         */
    uint32_t mailbox[2];             /* [0]=PeekMailbox(CPU) [1]=PeekMailbox(DSP) */

    uint16_t iram[4096];             /* DSP_IRAM_SIZE contents                */
    uint16_t dram[4096];             /* DSP_DRAM_SIZE contents                */

    int32_t  core_state;             /* DSP::State (Stopped/Running/Stepping) —
                                       * gap in upstream DoState.              */

    /* Accelerator (DSPAccelerator.h). */
    uint32_t accel_start_address;
    uint32_t accel_end_address;
    uint32_t accel_current_address;
    uint16_t accel_sample_format;
    int16_t  accel_gain;
    int16_t  accel_yn1;
    int16_t  accel_yn2;
    uint16_t accel_pred_scale;
    uint16_t accel_input;
    uint8_t  accel_reads_stopped;    /* gap in upstream DoState                */

    int32_t  dsp_int_pending;        /* g_dsp_int_pending — cross-thread
                                       * pending-not-yet-latched DSP->CPU
                                       * interrupt edge; no DoState equivalent
                                       * at all. */
} GcnDspLleSnapshot;

/* Fills *out from the live core. Returns 0 (out left untouched) if no core
 * exists yet (g_core == NULL) or out is NULL. Caller must have already
 * quiesced the GCN_DSP_THREAD worker (dsp_thread_drain, dsp.c) so nothing
 * mutates core state concurrently with this read. */
int dsp_lle_save_state(GcnDspLleSnapshot* out);

/* SNAPSHOT_RESUME pass B (restore side): the load-mirror of
 * dsp_lle_save_state. Writes every field back into the live core (must
 * already exist — call after dsp_lle_init), copies iram/dram contents, then
 * re-runs the same post-load re-analysis Core/DSP/DSPCore.cpp's own DoState
 * does after a real load (Host::CodeLoaded on the restored IRAM — IRAM CRC
 * + Analyzer::Analyze), since PointerWrap is a no-op stub here and that
 * path never fires on its own. Returns 0 (nothing done) if no core exists
 * yet or `in` is NULL. Caller must have the GCN_DSP_THREAD worker quiesced
 * (same precondition as dsp_lle_save_state). */
int dsp_lle_load_state(const GcnDspLleSnapshot* in);

#ifdef __cplusplus
}
#endif

#endif /* GCN_DSP_LLE_C_H */
