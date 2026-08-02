/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu/cpu.h"
#include "memory/memory.h"

#include <stdio.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

int main(void) {
    CPUState cpu;
    CHECK(cpu_init(&cpu));
    u32 l1_size = 0;
    u8* l1 = gcn_mem_locked_l1(&cpu, &l1_size);
    CHECK(l1 != NULL);
    CHECK(l1_size == GCN_L1_CACHE_SIZE);

    u32 avail = 0;
    u8* first = gcn_mem_resolve(&cpu, GCN_L1_CACHE_BASE, &avail);
    CHECK(first == l1);
    CHECK(avail == GCN_L1_CACHE_SIZE);
    CHECK(gcn_mem_resolve(
              &cpu, GCN_L1_CACHE_BASE + GCN_L1_CACHE_SIZE - 1u, &avail) ==
          l1 + GCN_L1_CACHE_SIZE - 1u);
    CHECK(avail == 1u);
    CHECK(gcn_mem_resolve(
              &cpu, GCN_L1_CACHE_BASE + GCN_L1_CACHE_SIZE, &avail) == NULL);

    mem_write8(&cpu, GCN_L1_CACHE_BASE + 3u, 0xA5u);
    mem_write16(&cpu, GCN_L1_CACHE_BASE + 4u, 0x1234u);
    mem_write32(&cpu, GCN_L1_CACHE_BASE + 8u, 0x89ABCDEFu);
    mem_write64(&cpu, GCN_L1_CACHE_BASE + 16u, 0x0123456789ABCDEFull);
    CHECK(mem_read8(&cpu, GCN_L1_CACHE_BASE + 3u) == 0xA5u);
    CHECK(mem_read16(&cpu, GCN_L1_CACHE_BASE + 4u) == 0x1234u);
    CHECK(mem_read32(&cpu, GCN_L1_CACHE_BASE + 8u) == 0x89ABCDEFu);
    CHECK(mem_read64(&cpu, GCN_L1_CACHE_BASE + 16u) ==
          0x0123456789ABCDEFull);
    CHECK(cpu.ram[3] == 0u);

    cpu_reset(&cpu);
    CHECK(gcn_mem_locked_l1(&cpu, &l1_size) == l1);
    CHECK(l1_size == GCN_L1_CACHE_SIZE);
    CHECK(mem_read32(&cpu, GCN_L1_CACHE_BASE + 8u) == 0u);

    cpu_free(&cpu);
    CHECK(gcn_mem_locked_l1(&cpu, &l1_size) == NULL);
    CHECK(l1_size == 0u);
    puts("locked L1 memory: PASS");
    return 0;
}
