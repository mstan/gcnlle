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

#ifdef __cplusplus
}
#endif

#endif /* GCN_DSP_LLE_C_H */
