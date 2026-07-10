/*
 * trace_format_check.c — trivial compile/layout check for trace_format.h.
 * Builds an executable that verifies (a) the header compiles clean as C11 and
 * (b) all _Static_asserts on record/payload sizes hold on this toolchain.
 * It also does a round-trip sanity write/read of one record in memory.
 */
#include "trace_format.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    gcn_trace_file_header fh;
    gcn_trace_file_header_init(&fh, GCN_TRACE_PROD_RUNTIME, 0u);

    if (fh.magic != GCN_TRACE_FILE_MAGIC) {
        printf("FAIL: file magic mismatch\n");
        return 1;
    }
    if (fh.record_size != GCN_TRACE_RECORD_SIZE || fh.hdr_size != GCN_TRACE_FHDR_SIZE) {
        printf("FAIL: header size fields wrong\n");
        return 1;
    }

    /* Build a retired-instruction record and round-trip it through a buffer. */
    gcn_trace_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.hdr.type = GCN_TR_RETIRED;
    rec.hdr.size = (uint16_t)GCN_TRACE_RECORD_SIZE;
    rec.hdr.seq  = 42u;
    rec.u.retired.pc = 0x81300000u;
    rec.u.retired.npc = 0x81300004u;
    rec.u.retired.gpr[1] = 0x817FE8F0u; /* r1 = stack pointer */

    unsigned char buf[GCN_TRACE_RECORD_SIZE];
    memcpy(buf, &rec, sizeof(buf));

    gcn_trace_record back;
    memcpy(&back, buf, sizeof(back));
    if (back.hdr.seq != 42u || back.u.retired.gpr[1] != 0x817FE8F0u) {
        printf("FAIL: record round-trip mismatch\n");
        return 1;
    }

    printf("OK: trace_format.h  record=%d bytes  fhdr=%d bytes  retired=%zu bytes\n",
           GCN_TRACE_RECORD_SIZE, GCN_TRACE_FHDR_SIZE, sizeof(gcn_tr_retired));
    return 0;
}
