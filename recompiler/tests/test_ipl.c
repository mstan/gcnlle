#include "frontend/container/ipl.h"
#include "common/types.h"
#include <stdio.h>
#include <string.h>

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        return 0; \
    } \
} while (0)

static int write_blob(const char* path, const u8* data, size_t size) {
    FILE* out = fopen(path, "wb");
    if (!out)
        return 0;
    int ok = fwrite(data, 1, size, out) == size;
    ok = fclose(out) == 0 && ok;
    return ok;
}

// li r3,1 ; blr  -- two valid PPC words, repeated.
static void fill_code(u8* buf, size_t size) {
    for (size_t i = 0; i + 8 <= size; i += 8) {
        write_be32(buf + i, 0x38600001u);
        write_be32(buf + i + 4, 0x4E800020u);
    }
}

static int test_valid_default(void) {
    const char* path = "test_ipl_valid.bin";
    u8 buf[64];
    memset(buf, 0, sizeof(buf));
    fill_code(buf, sizeof(buf));
    CHECK(write_blob(path, buf, sizeof(buf)), "failed to write blob");

    IPLFile ipl;
    CHECK(ipl_load(&ipl, path, IPL_DEFAULT_BASE, IPL_DEFAULT_ENTRY),
          "default-base load failed");
    CHECK(ipl.base_address == IPL_DEFAULT_BASE, "bad base 0x%08X", ipl.base_address);
    CHECK(ipl.entry_point == IPL_DEFAULT_ENTRY, "bad entry 0x%08X", ipl.entry_point);
    CHECK(ipl.code_size == sizeof(buf), "bad code_size 0x%X", ipl.code_size);
    CHECK(ipl.file_data != NULL, "no data");
    ipl_free(&ipl);
    remove(path);
    return 1;
}

static int test_custom_base_entry(void) {
    const char* path = "test_ipl_custom.bin";
    u8 buf[64];
    memset(buf, 0, sizeof(buf));
    fill_code(buf, sizeof(buf));
    CHECK(write_blob(path, buf, sizeof(buf)), "failed to write blob");

    IPLFile ipl;
    // entry 0x20 into a 0x40 image at base 0x80000000.
    CHECK(ipl_load(&ipl, path, 0x80000000u, 0x80000020u),
          "custom base/entry load failed");
    CHECK(ipl.entry_point == 0x80000020u, "bad entry 0x%08X", ipl.entry_point);
    ipl_free(&ipl);
    remove(path);
    return 1;
}

static int test_entry_outside_rejected(void) {
    const char* path = "test_ipl_entry_out.bin";
    u8 buf[64];
    memset(buf, 0, sizeof(buf));
    fill_code(buf, sizeof(buf));
    CHECK(write_blob(path, buf, sizeof(buf)), "failed to write blob");

    IPLFile ipl;
    // entry past the end of a 0x40 image.
    int loaded = ipl_load(&ipl, path, 0x80000000u, 0x80000040u);
    if (loaded) ipl_free(&ipl);
    remove(path);
    CHECK(!loaded, "entry past image end should be rejected");
    return 1;
}

static int test_unaligned_rejected(void) {
    const char* path = "test_ipl_unaligned.bin";
    u8 buf[64];
    memset(buf, 0, sizeof(buf));
    fill_code(buf, sizeof(buf));
    CHECK(write_blob(path, buf, sizeof(buf)), "failed to write blob");

    IPLFile ipl;
    int loaded = ipl_load(&ipl, path, 0x80000000u, 0x80000001u);
    if (loaded) ipl_free(&ipl);
    remove(path);
    CHECK(!loaded, "unaligned entry should be rejected");
    return 1;
}

int main(void) {
    int ok = 1;
    ok &= test_valid_default();
    ok &= test_custom_base_entry();
    ok &= test_entry_outside_rejected();
    ok &= test_unaligned_rejected();

    if (!ok)
        return 1;

    printf("IPL frontend tests passed\n");
    return 0;
}
