/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu/aot_module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

int main(void) {
    CPUState cpu;
    cpu_init(&cpu);
    const GcnAotRange chunks[] = {
        {0x80001000u, 0x80001004u},
        {0x80002000u, 0x80002008u},
    };
    u64 hashes[] = {
        gcn_aot_fnv1a64(cpu.ram + 0x1000u, 4u),
        gcn_aot_fnv1a64(cpu.ram + 0x2000u, 8u),
    };
    u8 states[2] = {0};
    GcnAotModule module = {chunks, hashes, states, 2};
    u32 index = 99;

    CHECK(gcn_aot_module_dispatchable(&module, &cpu, 0x80001000u, &index));
    CHECK(index == 0u && module.verified == 1u);
    CHECK(gcn_aot_module_dispatchable(&module, &cpu, 0x00001000u, &index));

    cpu.ram[0x1000u] = 1u;
    CHECK(gcn_aot_module_dispatchable(&module, &cpu, 0x80001000u, NULL));
    gcn_aot_module_invalidate(&module, 0x80001000u, 32u);
    CHECK(!gcn_aot_module_dispatchable(&module, &cpu, 0x80001000u, NULL));
    CHECK(module.failed == 1u);

    cpu.ram[0x1000u] = 0u;
    gcn_aot_module_invalidate(&module, 0x00001000u, 32u);
    CHECK(gcn_aot_module_dispatchable(&module, &cpu, 0xC0001000u, NULL));
    CHECK(!gcn_aot_module_dispatchable(&module, &cpu, 0x80003000u, NULL));

    CHECK(gcn_aot_module_dispatchable(&module, &cpu, 0x80002000u, NULL));
    CHECK(module.verified == 2u);
    gcn_aot_module_invalidate(&module, 0x80001000u, UINT32_MAX);
    CHECK(module.verified == 0u);

    cpu_free(&cpu);
    puts("aot module: PASS");
    return 0;
}
