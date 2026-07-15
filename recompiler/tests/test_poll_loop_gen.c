/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Generator half of the exact lwz/cmplwi/beq poll-loop test.  The emitted
 * chunk contains the smallest accepted class shape followed by an external
 * sentinel branch:
 *
 *   lwz r4,0(r3); cmplwi cr2,r4,0; beq cr2,loop; b 0xDEAD1000
 *
 * test_poll_loop_harness.c runs it with a derived deadline (the compact loop
 * fires) and a zero deadline (the ordinary instruction bodies fire).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/emitter.h"
#include "../src/common/types.h"

#define BASE 0x80004000u

int main(int argc, char** argv) {
    FILE* out = stdout;
    if (argc > 1) {
        out = fopen(argv[1], "w");
        if (!out) {
            perror(argv[1]);
            return 1;
        }
    }

    PPCInst insts[4];
    memset(insts, 0, sizeof(insts));

    insts[0].op = PPC_OP_LWZ;
    insts[0].address = BASE;
    insts[0].rD = 4;
    insts[0].rA = 3;

    insts[1].op = PPC_OP_CMPLI;
    insts[1].address = BASE + 4u;
    insts[1].crfD = 2;
    insts[1].rA = 4;
    insts[1].l = 0;
    insts[1].uimm = 0;

    insts[2].op = PPC_OP_BC;
    insts[2].address = BASE + 8u;
    insts[2].bo = 12;
    insts[2].bi = 10; /* cr2 EQ */
    insts[2].branch_target = BASE;

    insts[3].op = PPC_OP_B;
    insts[3].address = BASE + 12u;
    insts[3].branch_target = 0xDEAD1000u;

    emit_header(out);
    emit_function(out, insts, 4, BASE);
    emit_footer(out);

    if (out != stdout && fclose(out) != 0) {
        perror(argv[1]);
        return 1;
    }
    return 0;
}
