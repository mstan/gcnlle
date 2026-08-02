/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsp/dsp.h"

#include <stdio.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

int main(void) {
    GcnDsp dsp;
    gcn_dsp_reset_registers(&dsp);

    CHECK(gcn_dsp_reg_read16(&dsp, GCN_DSP_AR_MODE) == 1u);
    CHECK(gcn_dsp_reg_read16(&dsp, GCN_DSP_AR_REFRESH) == 156u);

    gcn_dsp_reg_write16(&dsp, GCN_DSP_AR_MODE, 0u);
    CHECK(gcn_dsp_reg_read16(&dsp, GCN_DSP_AR_MODE) == 1u);

    gcn_dsp_reg_write16(&dsp, GCN_DSP_AR_INFO, 0xFFFFu);
    gcn_dsp_reg_write16(&dsp, GCN_DSP_AR_REFRESH, 0xFFFFu);
    CHECK(gcn_dsp_reg_read16(&dsp, GCN_DSP_AR_INFO) == 0x007Fu);
    CHECK(gcn_dsp_reg_read16(&dsp, GCN_DSP_AR_REFRESH) == 0x07FFu);

    gcn_dsp_reg_write16(&dsp, GCN_DSP_AR_MMADDR, 0xFFFFu);
    gcn_dsp_reg_write16(&dsp, GCN_DSP_AR_MMADDR + 2u, 0xFFFFu);
    gcn_dsp_reg_write16(&dsp, GCN_DSP_AR_CNT, 0xFFFFu);
    gcn_dsp_reg_write16(&dsp, GCN_DSP_AR_CNT_LO, 0xFFFFu);
    CHECK(gcn_dsp_reg_read16(&dsp, GCN_DSP_AR_MMADDR) == 0x03FFu);
    CHECK(gcn_dsp_reg_read16(&dsp, GCN_DSP_AR_MMADDR + 2u) == 0xFFE0u);
    CHECK(gcn_dsp_reg_read16(&dsp, GCN_DSP_AR_CNT) == 0x83FFu);
    CHECK(gcn_dsp_reg_read16(&dsp, GCN_DSP_AR_CNT_LO) == 0xFFE0u);

    dsp.dma_active = true;
    dsp.dma_cycles_left = 2u * GCN_DSP_ARAM_TICKS_PER_32B;
    dsp.csr = GCN_DSP_CSR_ARMASK;
    CHECK(!gcn_dsp_aram_advance(&dsp, dsp.dma_cycles_left - 1u));
    CHECK(dsp.dma_active);
    CHECK(dsp.dma_cycles_left == 1u);
    CHECK(!(dsp.csr & GCN_DSP_CSR_ARINT));
    CHECK(gcn_dsp_aram_advance(&dsp, 1u));
    CHECK(!dsp.dma_active);
    CHECK(dsp.dma_cycles_left == 0u);
    CHECK(dsp.csr & GCN_DSP_CSR_ARINT);
    CHECK(!gcn_dsp_aram_advance(&dsp, 1u));

    puts("dsp registers: PASS");
    return 0;
}
