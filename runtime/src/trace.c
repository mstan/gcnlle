/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runtime-side trace producer. Emits oracle/trace_format.h records to
 * $GCN_TRACE_OUT for differential comparison against the Dolphin oracle tap.
 */
#include "trace/trace.h"
#include "trace_format.h"   /* shared with the Dolphin tap; via ../oracle include path */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE*    g_tf  = NULL;
static uint64_t g_seq = 0;

void gcn_trace_init(void) {
    const char* path = getenv("GCN_TRACE_OUT");
    if (!path || !*path) return;
    g_tf = fopen(path, "wb");
    if (!g_tf) {
        fprintf(stderr, "gcn trace: cannot open %s (tracing disabled)\n", path);
        return;
    }
    gcn_trace_file_header h;
    gcn_trace_file_header_init(&h, GCN_TRACE_PROD_RUNTIME, 0u);
    if (fwrite(&h, 1, sizeof h, g_tf) != sizeof h) {
        fprintf(stderr, "gcn trace: header write failed\n");
        fclose(g_tf); g_tf = NULL; return;
    }
    g_seq = 0;
    fprintf(stderr, "gcn trace: writing RUNTIME trace to %s\n", path);
}

int gcn_trace_active(void) { return g_tf != NULL; }

void gcn_trace_close(void) {
    if (g_tf) { fclose(g_tf); g_tf = NULL; }
}

/* Classify a Flipper MMIO address into its register block (matches the Dolphin
 * tap's classification so records line up). DI/SI/EXI/AI share the 0xCC006xxx
 * page and split by offset. */
static uint8_t mmio_block(u32 a) {
    if (a < 0xCC000000u || a >= 0xCC010000u) return GCN_MMIO_OTHER;
    switch (a & 0xFFFFF000u) {
        case 0xCC000000u: return GCN_MMIO_CP;
        case 0xCC001000u: return GCN_MMIO_PE;
        case 0xCC002000u: return GCN_MMIO_VI;
        case 0xCC003000u: return GCN_MMIO_PI;
        case 0xCC004000u: return GCN_MMIO_MI;
        case 0xCC005000u: return GCN_MMIO_DSP;
        case 0xCC006000u: {
            u32 off = a & 0xFFFu;
            if (off < 0x400u) return GCN_MMIO_DI;
            if (off < 0x800u) return GCN_MMIO_SI;
            if (off < 0xC00u) return GCN_MMIO_EXI;
            return GCN_MMIO_AI;
        }
        default: return GCN_MMIO_OTHER;
    }
}

void gcn_trace_mmio(u32 pc, u32 addr, u32 value, u8 size, int is_write) {
    if (!g_tf) return;
    gcn_trace_record r;
    memset(&r, 0, sizeof r);           /* zero-fill union tail for stable diffs */
    r.hdr.type = (uint16_t)GCN_TR_MMIO;
    r.hdr.size = (uint16_t)GCN_TRACE_RECORD_SIZE;
    r.hdr.cpu  = 0u;
    r.hdr.seq  = g_seq++;
    r.hdr.tb   = 0u;
    r.u.mmio.pc       = pc;
    r.u.mmio.addr     = addr;
    r.u.mmio.value    = value;
    r.u.mmio.size     = size;
    r.u.mmio.is_write = (uint8_t)(is_write ? 1 : 0);
    r.u.mmio.block    = mmio_block(addr);
    fwrite(&r, 1, sizeof r, g_tf);
}
