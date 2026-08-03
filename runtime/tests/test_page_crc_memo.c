/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Identity-contract test for the native-miss page-CRC memo
 * (interpreter.c: miss_page_crc + native_code.c: content-stale bitmap).
 *
 * The miss identity key is (pc, page CRC). The memo must serve a cached CRC
 * for duplicate misses on an unchanged page, and recompute -- yielding the
 * same identity behavior as the pre-memo per-miss hash -- the moment any
 * writer in the gcn_native_code_invalidate funnel (guest store, DMA, icbi)
 * touches the page. Observable contract, all via public API:
 *   - gcn_interpreter_note_native_miss returns 1 for a NEW (pc, crc)
 *     identity and 0 for a duplicate;
 *   - gcn_interpreter_page_crc_recomputes counts actual page hashes.
 *
 * NOTE: cpu_init()'s locked-L1 cache model is a process-wide singleton (see
 * test_cache_loop_batch.c) -- a single CPUState is used throughout.
 */
#include "cpu/interpreter.h"
#include "cpu/native_code.h"
#include "debug/rings.h"
#include "memory/memory.h"

#include <stdint.h>
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
    gcn_rings_init();

    CPUState cpu;
    CHECK(cpu_init(&cpu));
    gcn_native_code_reset();

    const u32 page_base = 0x80005000u;   /* one 4 KiB page of scratch code */
    for (u32 off = 0; off < 4096u; off += 4u)
        put_insn(&cpu, page_base + off, 0x60000000u /* nop */);

    /* Case 1: first miss on a page hashes it; duplicate misses at the same
     * pc AND at a different pc in the same page reuse the memo. */
    cpu.pc = page_base;
    u64 r0 = gcn_interpreter_page_crc_recomputes();
    CHECK(gcn_interpreter_note_native_miss(&cpu) == 1);   /* new identity   */
    u64 r1 = gcn_interpreter_page_crc_recomputes();
    CHECK(r1 == r0 + 1u);                                 /* hashed once    */
    CHECK(gcn_interpreter_note_native_miss(&cpu) == 0);   /* duplicate      */
    cpu.pc = page_base + 8u;
    CHECK(gcn_interpreter_note_native_miss(&cpu) == 1);   /* new pc, same crc */
    CHECK(gcn_interpreter_note_native_miss(&cpu) == 0);
    CHECK(gcn_interpreter_page_crc_recomputes() == r1);   /* zero rehashes  */

    /* Case 2: a guest store through memory.c (the funnel) changes the page
     * content -> next miss recomputes and mints a NEW identity at an
     * already-seen pc. */
    mem_write8_legacy(&cpu, page_base + 0x100u, 0xABu);
    cpu.pc = page_base;
    u64 r2 = gcn_interpreter_page_crc_recomputes();
    CHECK(gcn_interpreter_note_native_miss(&cpu) == 1);   /* new crc        */
    CHECK(gcn_interpreter_page_crc_recomputes() == r2 + 1u);

    /* Case 3: icbi-style invalidation with UNCHANGED bytes -> the memo must
     * recompute (stale bit was set) but land on the same CRC, so the miss
     * stays a duplicate. This is exactly what a dynamically-written flush
     * loop does to its own page every iteration. */
    gcn_native_code_invalidate(page_base, 32u);
    u64 r3 = gcn_interpreter_page_crc_recomputes();
    CHECK(gcn_interpreter_note_native_miss(&cpu) == 0);   /* same identity  */
    CHECK(gcn_interpreter_page_crc_recomputes() == r3 + 1u);
    /* ...and once the stale bit is consumed, further duplicates are free. */
    CHECK(gcn_interpreter_note_native_miss(&cpu) == 0);
    CHECK(gcn_interpreter_page_crc_recomputes() == r3 + 1u);

    /* Case 4: gcn_native_code_reset marks everything stale -- the next miss
     * rehashes even though the bytes never changed. */
    gcn_native_code_reset();
    u64 r4 = gcn_interpreter_page_crc_recomputes();
    CHECK(gcn_interpreter_note_native_miss(&cpu) == 0);
    CHECK(gcn_interpreter_page_crc_recomputes() == r4 + 1u);

    /* Case 5: a store that only touches a NEIGHBORING page must not stale
     * this one -- duplicate misses stay memoized. */
    mem_write8_legacy(&cpu, page_base + 4096u + 0x10u, 0x55u);
    u64 r5 = gcn_interpreter_page_crc_recomputes();
    CHECK(gcn_interpreter_note_native_miss(&cpu) == 0);
    CHECK(gcn_interpreter_page_crc_recomputes() == r5);

    /* Case 6: dcbz changes page bytes WITHOUT invalidating the native-code
     * fence (cpu_glue.c models the icache-staleness window there) -- the
     * memo must still see it via gcn_native_code_content_dirty: rehash, new
     * identity, fence untouched. */
    CHECK(!gcn_native_code_is_invalid(page_base + 0x200u));
    ppc_dcbz(&cpu, page_base + 0x200u, page_base);
    CHECK(!gcn_native_code_is_invalid(page_base + 0x200u));  /* fence untouched */
    u64 r6 = gcn_interpreter_page_crc_recomputes();
    CHECK(gcn_interpreter_note_native_miss(&cpu) == 1);      /* new identity   */
    CHECK(gcn_interpreter_page_crc_recomputes() == r6 + 1u);

    cpu_free(&cpu);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("page-crc memo identity contract: OK\n");
    return 0;
}
