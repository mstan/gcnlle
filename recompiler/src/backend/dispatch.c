#include "backend/dispatch.h"
#include <stdlib.h>
#include <string.h>

void emit_chunk_prototype(FILE* out, u32 func_addr) {
    fprintf(out, "void func_%08X(CPUState* ctx);\n", func_addr);
}

void function_list_free(FunctionList* list) {
    free(list->ranges);
    list->ranges = NULL;
    list->count = 0;
    list->capacity = 0;
}

int function_list_add(FunctionList* list, u32 start, u32 end) {
    if (list->count == list->capacity) {
        u32 new_capacity = list->capacity ? list->capacity * 2u : 64u;
        FunctionRange* new_ranges =
            (FunctionRange*)realloc(list->ranges, new_capacity * sizeof(*new_ranges));
        if (!new_ranges) {
            fprintf(stderr, "error: out of memory\n");
            return 0;
        }
        list->ranges = new_ranges;
        list->capacity = new_capacity;
    }

    list->ranges[list->count].start = start;
    list->ranges[list->count].end = end;
    list->count++;
    return 1;
}

static int cmp_range_start(const void* a, const void* b) {
    u32 sa = ((const FunctionRange*)a)->start;
    u32 sb = ((const FunctionRange*)b)->start;
    if (sa < sb) return -1;
    if (sa > sb) return 1;
    return 0;
}

static int u32_is_pow2(u32 x) { return x != 0u && (x & (x - 1u)) == 0u; }
static u32 u32_log2(u32 x) { u32 n = 0u; while ((x >>= 1) != 0u) n++; return n; }

/* A maximal run of consecutive (sorted) functions that share one nonzero size
 * and tile perfectly (each end == the next start). Such a run is dispatched in
 * O(1) via a table indexed by (address-base)/stride; runs of length 1 stay a
 * plain range check. */
typedef struct { u32 first; u32 count; u32 base; u32 stride; u32 end; int table; } DispRun;

void emit_dispatch_helpers(FILE* out, const FunctionList* funcs, u32 entry_point) {
    fprintf(out, "\n#define DOLRECOMP_ENTRY_POINT 0x%08Xu\n", entry_point);
    fprintf(out, "\ntypedef void (*DolRecompFunction)(CPUState* ctx);\n");
    /* Resolve pc -> recompiled function. The naive form is one range check per
     * function — O(N) per dispatch, and dispatch runs once per executed block, so
     * for a large image (the IPL is ~156 contiguous 0x4000 chunks) the hot path
     * walked dozens of compares every block. Instead, collapse each maximal run of
     * uniform-size, perfectly-tiling functions into a single O(1) table indexed by
     * (address-base)/stride; isolated functions keep a range check. Worst case (no
     * run merges) this reduces to the old chain, so it is never slower. */
    u32 n = funcs->count;
    FunctionRange* sorted = NULL;
    DispRun* runs = NULL;
    u32 run_count = 0;
    if (n) {
        sorted = (FunctionRange*)malloc(n * sizeof(*sorted));
        runs = (DispRun*)malloc(n * sizeof(*runs));
        if (!sorted || !runs) {
            fprintf(stderr, "error: out of memory\n");
            free(sorted);
            free(runs);
            return;
        }
        memcpy(sorted, funcs->ranges, n * sizeof(*sorted));
        qsort(sorted, n, sizeof(*sorted), cmp_range_start);

        int next_table = 0;
        for (u32 i = 0; i < n;) {
            u32 size = sorted[i].end - sorted[i].start;
            u32 j = i;
            while (size != 0u && j + 1u < n && sorted[j].end == sorted[j + 1].start &&
                   (sorted[j + 1].end - sorted[j + 1].start) == size) {
                j++;
            }
            DispRun rn;
            rn.first = i;
            rn.count = j - i + 1u;
            rn.base = sorted[i].start;
            rn.stride = size;
            rn.end = sorted[j].end;
            rn.table = (rn.count >= 2u) ? next_table++ : -1;
            runs[run_count++] = rn;
            i = j + 1u;
        }
    }

    /* Emit the per-run dispatch tables (file scope, referenced by the lookup). */
    for (u32 k = 0; k < run_count; k++) {
        if (runs[k].table < 0)
            continue;
        fprintf(out, "\nstatic const DolRecompFunction s_dolrecomp_tbl_%d[%u] = {\n",
                runs[k].table, runs[k].count);
        for (u32 m = 0; m < runs[k].count; m++) {
            if (m % 4u == 0u)
                fprintf(out, "    ");
            fprintf(out, "func_%08X,", sorted[runs[k].first + m].start);
            fprintf(out, (m % 4u == 3u || m + 1u == runs[k].count) ? "\n" : " ");
        }
        fprintf(out, "};\n");
    }

    fprintf(out, "\nstatic inline DolRecompFunction dolrecomp_find_original(u32 address) {\n");
    for (u32 k = 0; k < run_count; k++) {
        DispRun* rn = &runs[k];
        if (rn->table < 0) {
            fprintf(out,
                    "    if (address >= 0x%08Xu && address < 0x%08Xu && "
                    "((address - 0x%08Xu) & 3u) == 0u) return func_%08X;\n",
                    rn->base, rn->end, rn->base, rn->base);
        } else if (u32_is_pow2(rn->stride)) {
            fprintf(out,
                    "    if (address >= 0x%08Xu && address < 0x%08Xu && "
                    "((address - 0x%08Xu) & 3u) == 0u) return s_dolrecomp_tbl_%d[(address - 0x%08Xu) >> %uu];\n",
                    rn->base, rn->end, rn->base, rn->table, rn->base, u32_log2(rn->stride));
        } else {
            fprintf(out,
                    "    if (address >= 0x%08Xu && address < 0x%08Xu && "
                    "((address - 0x%08Xu) & 3u) == 0u) return s_dolrecomp_tbl_%d[(address - 0x%08Xu) / 0x%08Xu];\n",
                    rn->base, rn->end, rn->base, rn->table, rn->base, rn->stride);
        }
    }
    fprintf(out, "    return NULL;\n");
    fprintf(out, "}\n");
    free(runs);
    free(sorted);
    fprintf(out, "\nstatic inline int dolrecomp_call_original(CPUState* ctx, u32 address) {\n");
    fprintf(out, "    DolRecompFunction fn = dolrecomp_find_original(address);\n");
    fprintf(out, "    if (!fn) return 0;\n");
    fprintf(out, "    ctx->pc = address;\n");
    fprintf(out, "    fn(ctx);\n");
    fprintf(out, "    return 1;\n");
    fprintf(out, "}\n");
    fprintf(out, "\nstatic inline bool dolrecomp_physical_pc_alias(CPUState* ctx, u32 address, u32* alias_out) {\n");
    fprintf(out, "    if (address < ctx->ram_size) {\n");
    fprintf(out, "        *alias_out = address | GC_RAM_BASE;\n");
    fprintf(out, "        return *alias_out != address;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    return false;\n");
    fprintf(out, "}\n");
    fprintf(out, "\nstatic inline int dolrecomp_call(CPUState* ctx, u32 address) {\n");
    fprintf(out, "    u32 alias;\n");
    fprintf(out, "    ctx->pc = address;\n");
    /* Most static-recompiler deployments (including the IPL runtime) install
     * no HLE host-call replacement. Test the callback field inline so that
     * common case does not pay a non-inlinable ppc_host_call() round trip at
     * every returned guest block. The callback ABI and priority are unchanged
     * when a replacement is installed. */
    fprintf(out, "    if (ctx->host_call && ctx->host_call(ctx, address)) return 1;\n");
    fprintf(out, "    if (dolrecomp_call_original(ctx, address)) return 1;\n");
    fprintf(out, "    if (dolrecomp_physical_pc_alias(ctx, address, &alias)) {\n");
    fprintf(out, "        ctx->pc = alias;\n");
    fprintf(out, "        if (ctx->host_call && ctx->host_call(ctx, alias)) return 1;\n");
    fprintf(out, "        if (dolrecomp_call_original(ctx, alias)) return 1;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "\nstatic inline int dolrecomp_run_blocks(CPUState* ctx, u32 max_blocks) {\n");
    fprintf(out, "    u32 blocks = 0;\n");
    fprintf(out, "    while (max_blocks == 0u || blocks < max_blocks) {\n");
    fprintf(out, "        if (!dolrecomp_call(ctx, ctx->pc)) return 0;\n");
    fprintf(out, "        if (ctx->exception) return 0;\n");
    fprintf(out, "        blocks++;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    return 1;\n");
    fprintf(out, "}\n");
}
