#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/dispatch.h"

#define BASE 0x80003000u

static int pass_count = 0;
static int fail_count = 0;

static void check(int condition, const char* name) {
    printf("DISPATCH,%s,%s\n", name, condition ? "PASS" : "FAIL");
    if (condition)
        pass_count++;
    else
        fail_count++;
}

static char* emit_dispatch_to_string(void) {
    FunctionList funcs = {0};
    FILE* f = NULL;
    char* buf = NULL;

    if (!function_list_add(&funcs, BASE, BASE + 0x40u) ||
        !function_list_add(&funcs, BASE + 0x1000u, BASE + 0x1020u)) {
        function_list_free(&funcs);
        return NULL;
    }

    f = tmpfile();
    if (!f) {
        function_list_free(&funcs);
        return NULL;
    }

    emit_chunk_prototype(f, BASE);
    emit_chunk_prototype(f, BASE + 0x1000u);
    emit_dispatch_helpers(f, &funcs, BASE);
    function_list_free(&funcs);
    fflush(f);

    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    buf = (char*)malloc((size_t)size + 1u);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* A run of consecutive, equal-size, perfectly-tiling functions must collapse to
 * one O(1) dispatch table indexed by (address-base)>>log2(stride), not N range
 * checks. Uses three 0x4000 chunks so the stride is a power of two (shift). */
static char* emit_table_dispatch_to_string(void) {
    FunctionList funcs = {0};
    FILE* f = NULL;
    char* buf = NULL;

    if (!function_list_add(&funcs, 0x80100000u, 0x80104000u) ||
        !function_list_add(&funcs, 0x80104000u, 0x80108000u) ||
        !function_list_add(&funcs, 0x80108000u, 0x8010C000u)) {
        function_list_free(&funcs);
        return NULL;
    }

    f = tmpfile();
    if (!f) {
        function_list_free(&funcs);
        return NULL;
    }

    emit_chunk_prototype(f, 0x80100000u);
    emit_chunk_prototype(f, 0x80104000u);
    emit_chunk_prototype(f, 0x80108000u);
    emit_dispatch_helpers(f, &funcs, 0x80100000u);
    function_list_free(&funcs);
    fflush(f);

    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    buf = (char*)malloc((size_t)size + 1u);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

int main(void) {
    char* code = emit_dispatch_to_string();
    if (!code) {
        check(0, "emit dispatch helpers");
        printf("DISPATCH,total,%d passed %d failed\n", pass_count, fail_count);
        return 1;
    }

    check(strstr(code, "dolrecomp_find_original") != NULL,
          "emits original lookup helper");
    check(strstr(code, "dolrecomp_call_original") != NULL,
          "emits original call helper");
    check(strstr(code, "return func_80003000;") != NULL &&
          strstr(code, "return func_80004000;") != NULL,
          "original lookup covers generated chunks");
    check(strstr(code, "ctx->pc = address;") != NULL,
          "call helpers set the entry pc");
    check(strstr(code, "if (ppc_host_call(ctx, address)) return 1;") != NULL,
          "public dispatcher checks host replacements first");
    check(strstr(code, "dolrecomp_physical_pc_alias") != NULL &&
          strstr(code, "if (ppc_host_call(ctx, alias)) return 1;") != NULL &&
          strstr(code, "if (dolrecomp_call_original(ctx, alias)) return 1;") != NULL,
          "public dispatcher retries physical MEM1 aliases");
    check(strstr(code, "if (dolrecomp_call_original(ctx, address)) return 1;") != NULL,
          "public dispatcher can fall back to original code");

    free(code);

    char* tbl = emit_table_dispatch_to_string();
    if (!tbl) {
        check(0, "emit table dispatch helpers");
        printf("DISPATCH,total,%d passed %d failed\n", pass_count, fail_count);
        return 1;
    }
    check(strstr(tbl, "s_dolrecomp_tbl_0[3] = {") != NULL,
          "tiling run emits an O(1) dispatch table");
    check(strstr(tbl, "return s_dolrecomp_tbl_0[(address - 0x80100000u) >> 14u];") != NULL,
          "table lookup indexes by (address-base)>>log2(stride)");
    check(strstr(tbl, "func_80100000,") != NULL && strstr(tbl, "func_80108000,") != NULL,
          "table covers all chunks in the run");
    check(strstr(tbl, "return func_80104000;") == NULL,
          "run members collapse into the table, not per-chunk range checks");
    free(tbl);

    printf("DISPATCH,total,%d passed %d failed\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
