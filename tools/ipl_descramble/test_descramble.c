/*
 * Self-test for the IPL descrambler. Runs without a ROM:
 *   1. Keystream regression pin — the first 32 keystream bytes are fixed by
 *      the algorithm's constants, so this catches any accidental change to the
 *      LFSR math. (Cross-checked against gc-ipl wiki + Nintendont; see README.)
 *   2. Involution — the descrambler is XOR with a data-independent keystream,
 *      so applying it twice must return the original bytes exactly.
 *
 * Correctness against a *real* IPL (i.e. that this keystream is genuinely
 * segher's) is validated separately, gated on a user-supplied bios/ipl.bin,
 * by checking the descrambled image's known hash. See README.md.
 */
#include <stdio.h>
#include <string.h>

#include "descramble_core.h"

/* First 32 keystream bytes = ipl_descramble() applied to a zero buffer. */
static const uint8_t KEYSTREAM_HEAD[32] = {
    0x89, 0x7e, 0x47, 0x7f, 0xf4, 0x42, 0x3f, 0xe2,
    0xa1, 0x44, 0x32, 0xa6, 0x30, 0x13, 0xbc, 0xd1,
    0xdc, 0x12, 0xe0, 0xcc, 0xa5, 0x65, 0x36, 0x8c,
    0xdf, 0x2a, 0xba, 0x9a, 0xef, 0x28, 0x83, 0xad,
};

static int test_keystream_pin(void) {
    uint8_t buf[32] = {0};
    ipl_descramble(buf, sizeof buf);
    if (memcmp(buf, KEYSTREAM_HEAD, sizeof buf) != 0) {
        fprintf(stderr, "FAIL: keystream head mismatch\n  got: ");
        for (size_t i = 0; i < sizeof buf; i++) fprintf(stderr, "%02x ", buf[i]);
        fprintf(stderr, "\n");
        return 1;
    }
    printf("ok: keystream head pinned (32 bytes)\n");
    return 0;
}

static int test_involution(void) {
    uint8_t original[257];
    for (size_t i = 0; i < sizeof original; i++)
        original[i] = (uint8_t)(i * 37u + 11u);

    uint8_t work[257];
    memcpy(work, original, sizeof work);
    ipl_descramble(work, sizeof work);
    if (memcmp(work, original, sizeof work) == 0) {
        fprintf(stderr, "FAIL: one pass was a no-op (keystream all zero?)\n");
        return 1;
    }
    ipl_descramble(work, sizeof work);
    if (memcmp(work, original, sizeof work) != 0) {
        fprintf(stderr, "FAIL: descramble is not an involution\n");
        return 1;
    }
    printf("ok: involution over 257 bytes (odd length, not 8-aligned)\n");
    return 0;
}

int main(void) {
    int fails = 0;
    fails += test_keystream_pin();
    fails += test_involution();
    if (fails) { fprintf(stderr, "%d test(s) failed\n", fails); return 1; }
    printf("all descrambler self-tests passed\n");
    return 0;
}
