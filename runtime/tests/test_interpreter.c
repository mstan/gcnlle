#include "cpu/interpreter.h"
#include "cpu/native_code.h"
#include "debug/rings.h"

#include <math.h>
#include <stdio.h>

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static void put_insn(CPUState* cpu, u32 address, u32 raw) {
    write_be32(cpu->ram + (address & 0x1FFFFFFFu), raw);
}

int main(void) {
    CPUState cpu;
    CHECK(cpu_init(&cpu));
    gcn_rings_init();
    cpu.pc = 0x80001000u;

    put_insn(&cpu, 0x80001000u, 0x38600005u); /* li r3,5 */
    put_insn(&cpu, 0x80001004u, 0x38830007u); /* addi r4,r3,7 */
    put_insn(&cpu, 0x80001008u, 0x90800100u); /* stw r4,0x100(0) */
    put_insn(&cpu, 0x8000100Cu, 0x80A00100u); /* lwz r5,0x100(0) */
    put_insn(&cpu, 0x80001010u, 0x2C05000Cu); /* cmpwi r5,12 */
    put_insn(&cpu, 0x80001014u, 0x41820008u); /* beq +8 */
    put_insn(&cpu, 0x80001018u, 0x38C00001u); /* li r6,1 (skipped) */
    put_insn(&cpu, 0x8000101Cu, 0x38C00002u); /* li r6,2 */
    put_insn(&cpu, 0x80001020u, 0x7C671B79u); /* or. r7,r3,r3 */
    CHECK(!gcn_native_code_is_invalid(0x80001000u));

    CHECK(gcn_interpreter_note_native_miss(&cpu) == 1);
    CHECK(gcn_interpreter_note_native_miss(&cpu) == 0);
    for (int i = 0; i < 7; i++)
        CHECK(gcn_interpreter_step(&cpu));
    CHECK(cpu.gpr[3] == 5u);
    CHECK(cpu.gpr[4] == 12u);
    CHECK(cpu.gpr[5] == 12u);
    CHECK(cpu.gpr[6] == 2u);
    CHECK(cpu.pc == 0x80001020u);
    CHECK(gcn_interpreter_step(&cpu));
    CHECK(cpu.gpr[7] == 5u);
    CHECK((cpu.cr >> 28) == 4u);
    mem_write32(&cpu, 0x80002000u, 0x60000000u, cpu.pc);
    CHECK(gcn_native_code_is_invalid(0x80002000u));
    CHECK(gcn_native_code_invalid_page_count() >= 2u); /* stores at 0x100 and 0x2000 */

    cpu.pc = 0x80001100u;
    cpu.msr |= 0x2000u;
    cpu.fpr[2] = 1.25;
    cpu.fpr[3] = 2.5;
    cpu.ps1[2] = 4.0;
    cpu.ps1[3] = 8.0;
    put_insn(&cpu, 0x80001100u, 0xEC22182Au); /* fadds f1,f2,f3 */
    put_insn(&cpu, 0x80001104u, 0x1022182Au); /* ps_add f1,f2,f3 */
    CHECK(gcn_interpreter_step(&cpu));
    CHECK(fabs(cpu.fpr[1] - 3.75) < 0.000001);
    CHECK(gcn_interpreter_step(&cpu));
    CHECK(fabs(cpu.fpr[1] - 3.75) < 0.000001);
    CHECK(fabs(cpu.ps1[1] - 12.0) < 0.000001);

    /* Data-cache zero must not evict native/I-cache code. The following icbi
     * is the architectural point at which newly written instructions become
     * visible and the native candidate is fenced out. */
    cpu.pc = 0x80001108u;
    cpu.gpr[8] = 0x80003000u;
    put_insn(&cpu, 0x80001108u, 0x7C0047ECu); /* dcbz 0,r8 */
    put_insn(&cpu, 0x8000110Cu, 0x7C0047ACu); /* icbi 0,r8 */
    write_be32(cpu.ram + 0x3000u, 0x60000000u);
    CHECK(!gcn_native_code_is_invalid(0x80003000u));
    CHECK(gcn_interpreter_step(&cpu));
    CHECK(read_be32(cpu.ram + 0x3000u) == 0u);
    CHECK(!gcn_native_code_is_invalid(0x80003000u));
    CHECK(gcn_interpreter_step(&cpu));
    CHECK(gcn_native_code_is_invalid(0x80003000u));

    CHECK(gcn_interpreter_instruction_count() == 12u);
    CHECK(gcn_interpreter_unique_miss_count() == 1u);
    cpu_free(&cpu);
    return failures ? 1 : 0;
}
