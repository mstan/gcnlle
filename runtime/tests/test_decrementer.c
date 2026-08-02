/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu/cpu.h"
#include "cpu/timing.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

int main(void) {
    CPUState cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.pc = 0x80001234u;
    cpu.msr = 0x00008000u; /* MSR[EE] */

    ppc_mtspr(&cpu, 22u, 10u, cpu.pc);
    ppc_decrementer_tick(&cpu, 9u);
    CHECK(cpu.spr[22] == 1u);

    ppc_decrementer_tick(&cpu, 2u);
    CHECK(cpu.spr[22] == 0xFFFFFFFFu);
    ppc_deliver_decrementer(&cpu);
    CHECK(cpu.exception == PPC_EXC_DECREMENTER);
    CHECK(cpu.pc == PPC_VECTOR_DECREMENTER);
    CHECK(cpu.srr0 == 0x80001234u);
    CHECK(!(cpu.msr & 0x00008000u));

    memset(&cpu, 0, sizeof cpu);
    cpu.pc = 0x80005678u;
    ppc_mtspr(&cpu, 22u, 0u, cpu.pc);
    ppc_decrementer_tick(&cpu, 1u);
    ppc_deliver_decrementer(&cpu);
    CHECK(cpu.pc == 0x80005678u);
    cpu.msr = 0x00008000u;
    ppc_deliver_decrementer(&cpu);
    CHECK(cpu.pc == PPC_VECTOR_DECREMENTER);
    CHECK(cpu.srr0 == 0x80005678u);

    ppc_mtspr(&cpu, 22u, 0u, cpu.pc);
    ppc_decrementer_tick(&cpu, 1u);
    ppc_mtspr(&cpu, 22u, 123u, cpu.pc);
    CHECK(cpu.spr[22] == 123u);

    puts("decrementer timing: PASS");
    return 0;
}
