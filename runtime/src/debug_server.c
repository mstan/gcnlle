/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * TCP debug server (see include/debug/debug_server.h). Winsock, non-blocking,
 * pumped from the dispatch loop. JSON-over-newline. Query surface over the
 * always-on rings (debug/rings.h) + live CPU/RAM state.
 *
 * Deliberately small: one listen socket, a few clients, a hand-rolled parser
 * for the flat {"cmd":..,"key":num,..} requests our own client emits. It grows
 * command-by-command alongside the device models, the same way the psxrecomp
 * surface did.
 */
#include "debug/debug_server.h"
#include "debug/rings.h"
#include "dsp_lle_c.h"     /* dsp_state: live DSP pc/control/mailbox peeks */
#include "dsp/dsp.h"       /* gcn_dsp_flush: catch a batched core up before dsp_state peeks it */
#include "gx/gx.h"         /* gcn_gx_pipeline_drain — screenshot join (G3) */
#include "gx/gx_raster.h"  /* retained TEV/texture snapshots */
#include "gx/gx_vulkan.h"  /* beads-u2x.1 TLUT/texture residency ring */
#include "vi/vi.h"         /* screenshot: XFB scanout geometry */
#include "vi/yuy2.h"       /* screenshot: YUY2->RGB (shared with host_window.c) */
#include "si/si.h"         /* set_input: injected pad-report surface */
#include "di/di.h"         /* insert_disc/eject_disc: runtime disc mount lifecycle */
#include "host/host_audio.h" /* audio_state: WASAPI queue/signal acceptance */
#include "host/host_window.h" /* present_state: GCN_PRESENT_STATS=1 cadence counters */
#include "memory/memory.h"   /* coherent MEM1/MEM2/locked-L1 debug reads */
#include "debug/snapshot.h"  /* SNAPSHOT_RESUME: GCN_SNAPSHOT_SAVE hooks the checkpoint park below */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GCN_DBG_MAX_CLIENTS 4
#define GCN_DBG_LINE_CAP    (64 * 1024)      /* max request line               */
#define GCN_DBG_RESP_CAP    (4 * 1024 * 1024) /* max response (ring dumps)      */
#define GCN_COSIM_CPU_BLOB_CAP (16 * 1024)

typedef struct {
    SOCKET sock;
    int    len;                    /* bytes buffered in `line`                 */
    char   line[GCN_DBG_LINE_CAP];
} Client;

static int      s_enabled;
static SOCKET   s_listen = INVALID_SOCKET;
static Client   s_clients[GCN_DBG_MAX_CLIENTS];
static GcnDebugCtx s_ctx;
static int      s_quit;
static char     s_resp[GCN_DBG_RESP_CAP];
static int      s_cosim_enabled;
static int      s_cosim_parked;
static u64      s_cosim_budget;
static u64      s_cosim_instructions;
static int      s_cosim_stop_pc_armed;
static u32      s_cosim_stop_pc;
static int      s_checkpoint_armed;
static int      s_checkpoint_parked;
static u32      s_checkpoint_pc;
static int      s_checkpoint_have_gpr;
static u32      s_checkpoint_gpr;
static u32      s_checkpoint_gpr_value;
static int      s_checkpoint_have_lr;
static u32      s_checkpoint_lr;

static int env_u32(const char* name, u32* out) {
    const char* value = getenv(name);
    if (!value || !*value) return 0;
    char* end = NULL;
    unsigned long long parsed = strtoull(value, &end, 0);
    if (end == value || *end != '\0' || parsed > 0xFFFFFFFFull) {
        fprintf(stderr,
                "gcn checkpoint: invalid %s='%s' (expected a 32-bit integer)\n",
                name, value);
        return -1;
    }
    *out = (u32)parsed;
    return 1;
}

/* ---- tiny JSON field extractors (flat objects only) ---- */

/* Locate `"key"`, skip to the value after the colon. Returns pointer at the
 * first value char, or NULL. */
static const char* json_find_value(const char* s, const char* key) {
    char pat[64];
    int kl = snprintf(pat, sizeof pat, "\"%s\"", key);
    if (kl <= 0) return NULL;
    const char* p = strstr(s, pat);
    if (!p) return NULL;
    p += kl;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Parse an unsigned integer field (decimal or 0x-hex). Returns 1 if found. */
static int json_uint(const char* s, const char* key, u32* out) {
    const char* p = json_find_value(s, key);
    if (!p) return 0;
    *out = (u32)strtoul(p, NULL, 0);
    return 1;
}

/* Copy a string field into out (without quotes). Returns 1 if found. */
static int json_str(const char* s, const char* key, char* out, int cap) {
    const char* p = json_find_value(s, key);
    if (!p || *p != '"') return 0;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < cap - 1) out[i++] = *p++;
    out[i] = 0;
    return 1;
}

/* ---- response send (blocking-ish, bounded; localhost dev use) ---- */

static void client_close(Client* c) {
    if (c->sock != INVALID_SOCKET) { closesocket(c->sock); c->sock = INVALID_SOCKET; }
    c->len = 0;
}

static void send_all(Client* c, const char* buf, int len) {
    int off = 0, guard = 0;
    while (off < len) {
        int w = send(c->sock, buf + off, len - off, 0);
        if (w == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                if (++guard > 100000) { client_close(c); return; }  /* client stalled */
                continue;
            }
            client_close(c);
            return;
        }
        off += w; guard = 0;
    }
}

/* ---- command dispatch ---- */

static u8* ram_ptr(u32 guest_addr, u32* avail) {
    CPUState* cpu = s_ctx.cpu;
    if (!cpu) return NULL;
    return gcn_mem_resolve(cpu, guest_addr, avail);
}

/* Canonical byte-order-independent FNV-1a. This intentionally hashes fields
 * one by one instead of raw structs: padding, callbacks, host pointers and
 * other implementation-only state must never enter a co-sim comparison. */
static void hash_bytes(u64* h, const void* src, size_t len) {
    const u8* p = (const u8*)src;
    for (size_t i = 0; i < len; i++) {
        *h ^= p[i];
        *h *= 1099511628211ull;
    }
}

static void hash_u32(u64* h, u32 v) {
    u8 b[4] = {
        (u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v
    };
    hash_bytes(h, b, sizeof b);
}

static void hash_u64(u64* h, u64 v) {
    u8 b[8] = {
        (u8)(v >> 56), (u8)(v >> 48), (u8)(v >> 40), (u8)(v >> 32),
        (u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v
    };
    hash_bytes(h, b, sizeof b);
}

static u64 hash_memory(const u8* p, u32 len) {
    u64 h = 1469598103934665603ull;
    if (p && len)
        hash_bytes(&h, p, len);
    return h;
}

typedef struct {
    u8*    data;
    size_t cap;
    size_t len;
} CanonicalBytes;

static void canonical_u32(CanonicalBytes* out, u32 v) {
    if (!out || out->len + 4u > out->cap)
        return;
    out->data[out->len++] = (u8)(v >> 24);
    out->data[out->len++] = (u8)(v >> 16);
    out->data[out->len++] = (u8)(v >> 8);
    out->data[out->len++] = (u8)v;
}

static void canonical_u64(CanonicalBytes* out, u64 v) {
    if (!out || out->len + 8u > out->cap)
        return;
    out->data[out->len++] = (u8)(v >> 56);
    out->data[out->len++] = (u8)(v >> 48);
    out->data[out->len++] = (u8)(v >> 40);
    out->data[out->len++] = (u8)(v >> 32);
    out->data[out->len++] = (u8)(v >> 24);
    out->data[out->len++] = (u8)(v >> 16);
    out->data[out->len++] = (u8)(v >> 8);
    out->data[out->len++] = (u8)v;
}

/* Serialize exactly the CPU fields covered by cosim_state's CPU sub-hash.
 * This is deliberately independent of the host struct layout so Gate 4 can
 * compare the canonical bytes rather than merely trusting equal hashes. */
static size_t canonical_cpu_arch(const CPUState* cpu, u8* data, size_t cap) {
    CanonicalBytes out = { data, cap, 0u };
    if (!cpu || !data)
        return 0u;
    for (int i = 0; i < 32; i++) canonical_u32(&out, cpu->gpr[i]);
    for (int i = 0; i < 32; i++) {
        u64 bits;
        memcpy(&bits, &cpu->fpr[i], sizeof bits);
        canonical_u64(&out, bits);
    }
    for (int i = 0; i < 32; i++) {
        u64 bits;
        memcpy(&bits, &cpu->ps1[i], sizeof bits);
        canonical_u64(&out, bits);
    }
    canonical_u32(&out, cpu->pc);
    canonical_u32(&out, cpu->lr);
    canonical_u32(&out, cpu->ctr);
    canonical_u32(&out, cpu->cr);
    canonical_u32(&out, cpu->xer);
    canonical_u32(&out, cpu->fpscr);
    canonical_u32(&out, cpu->msr);
    canonical_u32(&out, cpu->srr0);
    canonical_u32(&out, cpu->srr1);
    canonical_u32(&out, cpu->dar);
    canonical_u32(&out, cpu->dsisr);
    canonical_u32(&out, cpu->ear);
    canonical_u32(&out, cpu->hid2);
    canonical_u64(&out, cpu->timebase);
    for (int i = 0; i < 16; i++) canonical_u32(&out, cpu->sr[i]);
    for (int i = 0; i < 8; i++) canonical_u32(&out, cpu->gqr[i]);
    for (int i = 0; i < 1024; i++) canonical_u32(&out, cpu->spr[i]);
    canonical_u32(&out, cpu->exception);
    canonical_u32(&out, cpu->program_exception);
    canonical_u32(&out, cpu->tlb_last_vps);
    canonical_u32(&out, cpu->tlb_last_index);
    canonical_u32(&out, cpu->tlb_invalidate_count);
    canonical_u32(&out, cpu->external_addr);
    canonical_u32(&out, cpu->external_value);
    canonical_u32(&out, cpu->external_rid);
    canonical_u32(&out, cpu->external_read_count);
    canonical_u32(&out, cpu->external_write_count);
    canonical_u32(&out, cpu->reserve_addr);
    canonical_u32(&out, cpu->reserve_valid ? 1u : 0u);
    for (int i = 0; i < 512; i++) {
        canonical_u32(&out, cpu->locked_cache_tag[i]);
        canonical_u32(&out, cpu->locked_cache_valid[i] ? 1u : 0u);
    }
    canonical_u64(&out, cpu->cycles);
    return out.len;
}

static u64 hash_cpu_arch(const CPUState* cpu) {
    u8 data[GCN_COSIM_CPU_BLOB_CAP];
    size_t len = canonical_cpu_arch(cpu, data, sizeof data);
    return hash_memory(data, (u32)len);
}

static void handle_line(Client* c, const char* line) {
    char cmd[48] = {0};
    u32 id = 0; int have_id = json_uint(line, "id", &id);
    if (!json_str(line, "cmd", cmd, sizeof cmd)) {
        int n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                         "{\"ok\":false,\"error\":\"missing cmd\"}\n");
        send_all(c, s_resp, n);
        return;
    }

    int n = 0;
    CPUState* cpu = s_ctx.cpu;

    if (!strcmp(cmd, "ping") || !strcmp(cmd, "frame")) {
        n = snprintf(s_resp, GCN_DBG_RESP_CAP,
            "{\"ok\":true,\"block\":%llu,\"pc\":%u}\n",
            (unsigned long long)gcn_ring_block_index(), cpu ? cpu->pc : 0);
    }
    else if (!strcmp(cmd, "cosim_status")) {
        n = snprintf(s_resp, GCN_DBG_RESP_CAP,
            "{\"ok\":true,\"enabled\":%s,\"parked\":%s,"
            "\"instruction\":%llu,\"budget\":%llu,\"cycles\":%llu,\"pc\":%u,"
            "\"stop_pc_armed\":%s,\"stop_pc\":%u}\n",
            s_cosim_enabled ? "true" : "false",
            s_cosim_parked ? "true" : "false",
            (unsigned long long)s_cosim_instructions,
            (unsigned long long)s_cosim_budget,
            (unsigned long long)(cpu ? cpu->cycles : 0u),
            cpu ? cpu->pc : 0u,
            s_cosim_stop_pc_armed ? "true" : "false", s_cosim_stop_pc);
    }
    else if (!strcmp(cmd, "checkpoint_status")) {
        n = snprintf(s_resp, GCN_DBG_RESP_CAP,
            "{\"ok\":true,\"armed\":%s,\"parked\":%s,\"pc\":%u,"
            "\"have_gpr\":%s,\"gpr\":%u,\"gpr_value\":%u,"
            "\"have_lr\":%s,\"lr\":%u,"
            "\"live_pc\":%u,\"block\":%llu}\n",
            s_checkpoint_armed ? "true" : "false",
            s_checkpoint_parked ? "true" : "false",
            s_checkpoint_pc,
            s_checkpoint_have_gpr ? "true" : "false",
            s_checkpoint_gpr, s_checkpoint_gpr_value,
            s_checkpoint_have_lr ? "true" : "false", s_checkpoint_lr,
            cpu ? cpu->pc : 0u,
            (unsigned long long)gcn_ring_block_index());
    }
    else if (!strcmp(cmd, "checkpoint_arm")) {
        u32 pc = 0, gpr = 0, gpr_value = 0, lr = 0;
        int have_pc = json_uint(line, "pc", &pc);
        int have_gpr = json_uint(line, "gpr", &gpr);
        int have_gpr_value = json_uint(line, "gpr_value", &gpr_value);
        int have_lr = json_uint(line, "lr", &lr);
        if (!have_pc) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"missing pc\"}\n");
        } else if (have_gpr != have_gpr_value || (have_gpr && gpr >= 32u)) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"gpr and gpr_value must both be present; gpr must be 0..31\"}\n");
        } else if (s_checkpoint_parked) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"resume the current checkpoint before arming another\"}\n");
        } else {
            s_checkpoint_pc = pc;
            s_checkpoint_have_gpr = have_gpr;
            s_checkpoint_gpr = gpr;
            s_checkpoint_gpr_value = gpr_value;
            s_checkpoint_have_lr = have_lr;
            s_checkpoint_lr = lr;
            s_checkpoint_armed = 1;
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":true,\"armed\":true,\"pc\":%u,"
                "\"have_gpr\":%s,\"gpr\":%u,\"gpr_value\":%u,"
                "\"have_lr\":%s,\"lr\":%u}\n",
                pc, have_gpr ? "true" : "false", gpr, gpr_value,
                have_lr ? "true" : "false", lr);
        }
    }
    else if (!strcmp(cmd, "checkpoint_resume")) {
        int was_parked = s_checkpoint_parked;
        s_checkpoint_armed = 0;
        s_checkpoint_parked = 0;
        n = snprintf(s_resp, GCN_DBG_RESP_CAP,
            "{\"ok\":true,\"resumed\":%s}\n",
            was_parked ? "true" : "false");
    }
    else if (!strcmp(cmd, "checkpoint_continue")) {
        u32 pc = 0, gpr = 0, gpr_value = 0, lr = 0;
        int have_pc = json_uint(line, "pc", &pc);
        int have_gpr = json_uint(line, "gpr", &gpr);
        int have_gpr_value = json_uint(line, "gpr_value", &gpr_value);
        int have_lr = json_uint(line, "lr", &lr);
        if (!have_pc) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"missing pc\"}\n");
        } else if (have_gpr != have_gpr_value || (have_gpr && gpr >= 32u)) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"gpr and gpr_value must both be present; gpr must be 0..31\"}\n");
        } else if (!s_checkpoint_parked) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"guest is not parked\"}\n");
        } else {
            /*
             * Install the next condition before releasing the current park.
             * A coordinator can therefore cross an arbitrarily short native
             * interval without an arm-after-resume race.
             */
            s_checkpoint_pc = pc;
            s_checkpoint_have_gpr = have_gpr;
            s_checkpoint_gpr = gpr;
            s_checkpoint_gpr_value = gpr_value;
            s_checkpoint_have_lr = have_lr;
            s_checkpoint_lr = lr;
            s_checkpoint_armed = 1;
            s_checkpoint_parked = 0;
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":true,\"continued\":true,\"armed\":true,\"pc\":%u,"
                "\"have_gpr\":%s,\"gpr\":%u,\"gpr_value\":%u,"
                "\"have_lr\":%s,\"lr\":%u}\n",
                pc, have_gpr ? "true" : "false", gpr, gpr_value,
                have_lr ? "true" : "false", lr);
        }
    }
    else if (!strcmp(cmd, "cosim_step")) {
        u32 count = 1;
        json_uint(line, "count", &count);
        if (!s_cosim_enabled) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"GCN_COSIM is not enabled\"}\n");
        } else if (!s_cosim_parked) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"guest is not parked\"}\n");
        } else if (count == 0u) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"count must be nonzero\"}\n");
        } else {
            s_cosim_stop_pc_armed = 0;
            s_cosim_budget = count;
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":true,\"granted\":%u,\"instruction\":%llu}\n",
                count, (unsigned long long)s_cosim_instructions);
        }
    }
    else if (!strcmp(cmd, "cosim_run_to")) {
        u32 pc = 0, max_instructions = 10000000u;
        int have_pc = json_uint(line, "pc", &pc);
        json_uint(line, "max_instructions", &max_instructions);
        if (!s_cosim_enabled) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"GCN_COSIM is not enabled\"}\n");
        } else if (!s_cosim_parked) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"guest is not parked\"}\n");
        } else if (!have_pc || max_instructions == 0u) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"pc and nonzero max_instructions are required\"}\n");
        } else if (cpu && cpu->pc == pc) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":true,\"already_there\":true,\"pc\":%u,"
                "\"instruction\":%llu}\n",
                pc, (unsigned long long)s_cosim_instructions);
        } else {
            s_cosim_stop_pc = pc;
            s_cosim_stop_pc_armed = 1;
            s_cosim_budget = max_instructions;
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":true,\"target_pc\":%u,\"max_instructions\":%u,"
                "\"instruction\":%llu}\n",
                pc, max_instructions,
                (unsigned long long)s_cosim_instructions);
        }
    }
    else if (!strcmp(cmd, "cosim_state") || !strcmp(cmd, "state_hash")) {
        if (!cpu) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"no cpu\"}\n");
        } else if (s_cosim_enabled && !s_cosim_parked) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"guest must be parked for a coherent snapshot\"}\n");
        } else {
            /* GX can still own pending XFB/DL accesses in a non-cosim query.
             * Co-sim itself is single-threaded, but the drain is harmless and
             * preserves the ordinary synchronous snapshot contract. */
            gcn_gx_pipeline_drain();
            u64 cpu_hash = hash_cpu_arch(cpu);
            u64 mem1_hash = hash_memory(cpu->ram, cpu->ram_size);
            u64 mem2_hash = hash_memory(cpu->mem2, cpu->mem2_size);
            u32 l1_size = 0;
            u8* l1 = gcn_mem_locked_l1(cpu, &l1_size);
            u64 l1_hash = hash_memory(l1, l1_size);
            u64 combined = 1469598103934665603ull;
            hash_u64(&combined, cpu_hash);
            hash_u64(&combined, mem1_hash);
            hash_u64(&combined, mem2_hash);
            hash_u64(&combined, l1_hash);
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":true,\"instruction\":%llu,\"cycles\":%llu,\"pc\":%u,"
                "\"hash\":\"%016llx\",\"sub\":{\"cpu\":\"%016llx\","
                "\"mem1\":\"%016llx\",\"mem2\":\"%016llx\","
                "\"l1\":\"%016llx\"},"
                "\"layout\":{\"mem1\":{\"base\":2147483648,\"size\":%u},"
                "\"mem2\":{\"base\":2415919104,\"size\":%u},"
                "\"l1\":{\"base\":3758096384,\"size\":%u}},"
                "\"coverage\":{\"cpu\":true,\"mem1\":true,\"mem2\":true,"
                "\"l1\":true,\"devices\":false},\"complete\":false}\n",
                (unsigned long long)s_cosim_instructions,
                (unsigned long long)cpu->cycles, cpu->pc,
                (unsigned long long)combined,
                (unsigned long long)cpu_hash,
                (unsigned long long)mem1_hash,
                (unsigned long long)mem2_hash,
                (unsigned long long)l1_hash,
                cpu->ram_size, cpu->mem2_size, l1_size);
        }
    }
    else if (!strcmp(cmd, "cosim_cpu_bytes")) {
        if (!cpu) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"no cpu\"}\n");
        } else if (!s_cosim_enabled || !s_cosim_parked) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"CPU byte audit requires a parked co-sim guest\"}\n");
        } else {
            u8 data[GCN_COSIM_CPU_BLOB_CAP];
            size_t len = canonical_cpu_arch(cpu, data, sizeof data);
            u64 hash = hash_memory(data, (u32)len);
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":true,\"encoding\":\"hex\",\"length\":%llu,"
                "\"hash\":\"%016llx\",\"hex\":\"",
                (unsigned long long)len, (unsigned long long)hash);
            static const char hex[] = "0123456789abcdef";
            for (size_t i = 0; i < len && n + 2 < GCN_DBG_RESP_CAP; i++) {
                s_resp[n++] = hex[data[i] >> 4];
                s_resp[n++] = hex[data[i] & 15u];
            }
            n += snprintf(s_resp + n, GCN_DBG_RESP_CAP - n, "\"}\n");
        }
    }
    else if (!strcmp(cmd, "cosim_pages")) {
        char space[16] = "mem1";
        u32 start = 0, count = 64;
        json_str(line, "space", space, sizeof space);
        json_uint(line, "start", &start);
        json_uint(line, "count", &count);
        if (count > 256u) count = 256u;
        const u8* base = NULL;
        u32 size = 0;
        if (cpu && !strcmp(space, "mem1")) {
            base = cpu->ram; size = cpu->ram_size;
        } else if (cpu && !strcmp(space, "mem2")) {
            base = cpu->mem2; size = cpu->mem2_size;
        } else if (cpu && !strcmp(space, "l1")) {
            base = gcn_mem_locked_l1(cpu, &size);
        }
        const u32 page_size = 4096u;
        u32 pages = (size + page_size - 1u) / page_size;
        if (!base || start >= pages) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"page range unavailable\"}\n");
        } else {
            if (count > pages - start) count = pages - start;
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":true,\"space\":\"%s\",\"page_size\":%u,"
                "\"start\":%u,\"hashes\":[", space, page_size, start);
            for (u32 i = 0; i < count; i++) {
                u32 off = (start + i) * page_size;
                u32 len = size - off;
                if (len > page_size) len = page_size;
                u64 h = hash_memory(base + off, len);
                n += snprintf(s_resp + n, GCN_DBG_RESP_CAP - n,
                    "%s\"%016llx\"", i ? "," : "", (unsigned long long)h);
            }
            n += snprintf(s_resp + n, GCN_DBG_RESP_CAP - n, "]}\n");
        }
    }
    else if (!strcmp(cmd, "cosim_inject")) {
        char kind[16] = {0};
        u32 index = 0, addr = 0, xor_mask = 1u, value_hi = 0, value_lo = 0;
        json_str(line, "kind", kind, sizeof kind);
        json_uint(line, "index", &index);
        json_uint(line, "addr", &addr);
        json_uint(line, "xor", &xor_mask);
        json_uint(line, "value_hi", &value_hi);
        json_uint(line, "value_lo", &value_lo);
        if (!s_cosim_enabled || !s_cosim_parked || !cpu) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"injection requires a parked co-sim guest\"}\n");
        } else if (!strcmp(kind, "gpr") && index < 32u) {
            cpu->gpr[index] ^= xor_mask;
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":true,\"kind\":\"gpr\",\"index\":%u,\"value\":%u}\n",
                index, cpu->gpr[index]);
        } else if (!strcmp(kind, "ram")) {
            u32 avail = 0;
            u8* p = ram_ptr(addr, &avail);
            if (!p || avail == 0u) {
                n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                    "{\"ok\":false,\"error\":\"injection address unavailable\"}\n");
            } else {
                p[0] ^= (u8)xor_mask;
                n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                    "{\"ok\":true,\"kind\":\"ram\",\"addr\":%u,\"value\":%u}\n",
                    addr, (unsigned)p[0]);
            }
        } else if (!strcmp(kind, "timebase")) {
            cpu->timebase = ((u64)value_hi << 32) | value_lo;
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":true,\"kind\":\"timebase\","
                "\"value_hi\":%u,\"value_lo\":%u}\n",
                value_hi, value_lo);
        } else {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"kind must be gpr, ram, or timebase\"}\n");
        }
    }
    else if (!strcmp(cmd, "get_registers") || !strcmp(cmd, "regs")) {
        if (!cpu) { n = snprintf(s_resp, GCN_DBG_RESP_CAP, "{\"ok\":false,\"error\":\"no cpu\"}\n"); }
        else {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP, "{\"ok\":true,\"gpr\":[");
            for (int i = 0; i < 32; i++)
                n += snprintf(s_resp + n, GCN_DBG_RESP_CAP - n, "%s%u", i ? "," : "", cpu->gpr[i]);
            n += snprintf(s_resp + n, GCN_DBG_RESP_CAP - n,
                "],\"pc\":%u,\"lr\":%u,\"ctr\":%u,\"cr\":%u,\"xer\":%u,\"msr\":%u,"
                "\"srr0\":%u,\"srr1\":%u,\"dar\":%u,\"dsisr\":%u,\"exception\":%u,"
                "\"timebase\":%llu}\n",
                cpu->pc, cpu->lr, cpu->ctr, cpu->cr, cpu->xer, cpu->msr,
                cpu->srr0, cpu->srr1, cpu->dar, cpu->dsisr, cpu->exception,
                (unsigned long long)cpu->timebase);
        }
    }
    else if (!strcmp(cmd, "read_ram") || !strcmp(cmd, "dump_ram")) {
        u32 addr = 0, len = 0;
        json_uint(line, "addr", &addr); json_uint(line, "len", &len);
        if (len > 65536) len = 65536;
        /* Preserve the synchronous debug-snapshot contract: a pending GX
         * copy may write MEM1, including the XFB, on its worker. */
        gcn_gx_pipeline_drain();
        u32 avail = 0; u8* p = ram_ptr(addr, &avail);
        if (!p) { n = snprintf(s_resp, GCN_DBG_RESP_CAP, "{\"ok\":false,\"error\":\"addr out of range\"}\n"); }
        else {
            if (len > avail) len = avail;
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":true,\"addr\":%u,\"len\":%u,\"hex\":\"", addr, len);
            static const char hexd[] = "0123456789abcdef";
            for (u32 i = 0; i < len && n < GCN_DBG_RESP_CAP - 4; i++) {
                s_resp[n++] = hexd[p[i] >> 4];
                s_resp[n++] = hexd[p[i] & 0xF];
            }
            n += snprintf(s_resp + n, GCN_DBG_RESP_CAP - n, "\"}\n");
        }
    }
    else if (!strcmp(cmd, "write_ram")) {
        u32 addr = 0; char hex[8192] = {0};
        json_uint(line, "addr", &addr);
        json_str(line, "hex", hex, sizeof hex);
        /* The GX worker may still be reading display-list, vertex, texture,
         * or TLUT bytes from MEM1. Join before a debugger mutates them. */
        gcn_gx_pipeline_drain();
        u32 avail = 0; u8* p = ram_ptr(addr, &avail);
        int nbytes = (int)strlen(hex) / 2;
        if (!p) { n = snprintf(s_resp, GCN_DBG_RESP_CAP, "{\"ok\":false,\"error\":\"addr out of range\"}\n"); }
        else {
            int wrote = 0;
            for (int i = 0; i < nbytes && (u32)i < avail; i++) {
                char b[3] = { hex[2*i], hex[2*i+1], 0 };
                p[i] = (u8)strtoul(b, NULL, 16); wrote++;
            }
            n = snprintf(s_resp, GCN_DBG_RESP_CAP, "{\"ok\":true,\"wrote\":%d}\n", wrote);
        }
    }
    else if (!strcmp(cmd, "mmio_dump")) {
        u32 count = 256, addr = 0, rw = 0;
        int have_addr = json_uint(line, "addr", &addr);
        int have_rw = json_uint(line, "rw", &rw);
        json_uint(line, "count", &count);
        n = gcn_ring_mmio_json(s_resp, GCN_DBG_RESP_CAP, (int)count,
                               addr, have_addr, have_rw ? (int)rw : -1);
    }
    else if (!strcmp(cmd, "block_dump")) {
        u32 count = 256; json_uint(line, "count", &count);
        n = gcn_ring_block_json(s_resp, GCN_DBG_RESP_CAP, (int)count);
    }
    else if (!strcmp(cmd, "gpr_probe_dump")) {
        u32 count = 256; json_uint(line, "count", &count);
        n = gcn_ring_gpr_probe_json(s_resp, GCN_DBG_RESP_CAP, (int)count);
    }
    else if (!strcmp(cmd, "pc_seen")) {
        u32 pc = 0;
        if (!json_uint(line, "pc", &pc)) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                         "{\"ok\":false,\"error\":\"missing pc\"}\n");
        } else {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                         "{\"ok\":true,\"pc\":%u,\"seen\":%s}\n",
                         pc, gcn_ring_pc_seen(pc) ? "true" : "false");
        }
    }
    else if (!strcmp(cmd, "event_dump")) {
        u32 count = 256; json_uint(line, "count", &count);
        n = gcn_ring_event_json(s_resp, GCN_DBG_RESP_CAP, (int)count);
    }
    else if (!strcmp(cmd, "fifo_dump")) {
        /* GX gather-pipe burst recorder (ROADMAP M2 packet inventory). */
        u32 count = 64; json_uint(line, "count", &count);
        n = gcn_ring_fifo_json(s_resp, GCN_DBG_RESP_CAP, (int)count);
    }
    else if (!strcmp(cmd, "watch_dump")) {
        u32 count = 256; json_uint(line, "count", &count);
        gcn_ring_watch_dump_stderr((int)count);
        n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                     "{\"ok\":true,\"dumped\":%u,\"stream\":\"stderr\"}\n",
                     count);
    }
    else if (!strcmp(cmd, "card_traffic")) {
        /* EXI memory-card transaction recorder (ROADMAP M4). Sparse always-on
         * ring — the directory read/save read/write command stream stays
         * queryable long after it happened (unlike the shallow MMIO ring). */
        u32 count = 256; json_uint(line, "count", &count);
        n = gcn_ring_memcard_json(s_resp, GCN_DBG_RESP_CAP, (int)count);
    }
    else if (!strcmp(cmd, "dsp_state")) {
        /* Live DSP-LLE core state: pc, control reg, and non-consuming peeks of
         * both mailboxes (bit 31 = mail pending). Diagnoses CPU<->DSP task
         * handshake stalls without perturbing the run. Flush first — a
         * batched core's owed debt must be run before these peeks, or the
         * answer reports a stale pc/mailbox. */
        gcn_dsp_flush();
        n = snprintf(s_resp, GCN_DBG_RESP_CAP,
            "{\"ok\":true,\"pc\":%u,\"control\":%u,"
            "\"mbox_dsp_to_cpu\":%u,\"mbox_cpu_to_dsp\":%u}\n",
            (unsigned)dsp_lle_pc(), (unsigned)dsp_lle_read_control(),
            (unsigned)dsp_lle_peek_mbox_dsp(), (unsigned)dsp_lle_peek_mbox_cpu());
    }
    else if (!strcmp(cmd, "rtc_state")) {
        /* Query the same latch the IPL reads. This advances only from current
         * emulated CPU cycles; it never samples the host clock. Exposing both
         * anchors makes the one-shot boot-sync contract directly auditable. */
        if (!cpu || !s_ctx.exi) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"RTC unavailable\"}\n");
        } else {
            u32 counter = gcn_exi_rtc_latch(s_ctx.exi, cpu->cycles);
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":true,\"counter\":%u,\"running\":%s,"
                "\"anchor_cycles\":%llu,\"cpu_cycles\":%llu}\n",
                counter, s_ctx.exi->rtc.running ? "true" : "false",
                (unsigned long long)s_ctx.exi->rtc.anchor_cycles,
                (unsigned long long)cpu->cycles);
        }
    }
    else if (!strcmp(cmd, "audio_state")) {
        GcnHostAudioStats a;
        gcn_host_audio_get_stats(&a);
        u32 aid_source = s_ctx.dsp ? s_ctx.dsp->aid_source : 0u;
        u32 aid_cur_addr = s_ctx.dsp ? s_ctx.dsp->aid_cur_addr : 0u;
        u32 aid_blocks_left = s_ctx.dsp ? s_ctx.dsp->aid_blocks_left : 0u;
        u32 aid_ctrl = s_ctx.dsp ? s_ctx.dsp->aid_ctrl : 0u;
        n = snprintf(s_resp, GCN_DBG_RESP_CAP,
            "{\"ok\":true,\"enabled\":%s,\"device_open\":%s,"
            "\"sample_rate\":%u,\"peak\":%u,\"buffered_frames\":%u,"
            "\"frames_received\":%llu,\"audible_frames\":%llu,"
            "\"buffers_submitted\":%llu,\"underruns\":%llu,"
            "\"wait_milliseconds\":%llu,\"dropped_frames\":%llu,"
            "\"aid_source\":%u,\"aid_cur_addr\":%u,"
            "\"aid_blocks_left\":%u,\"aid_ctrl\":%u}\n",
            a.enabled ? "true" : "false", a.device_open ? "true" : "false",
            a.sample_rate, a.peak, a.buffered_frames,
            (unsigned long long)a.frames_received,
            (unsigned long long)a.audible_frames,
            (unsigned long long)a.buffers_submitted,
            (unsigned long long)a.underruns,
            (unsigned long long)a.wait_milliseconds,
            (unsigned long long)a.dropped_frames,
            aid_source, aid_cur_addr, aid_blocks_left, aid_ctrl);
    }
    else if (!strcmp(cmd, "present_state")) {
        /* Mirrors audio_state above: a flat JSON dump of the GCN_PRESENT_STATS=1
         * VI/presenter cadence counters (host_window.h's GcnPresentStats) plus
         * the VI field tick count (vi.h) — the release-gate "VI/presenter
         * cadence" metrics (fields/s, distinct vs coalesced presents, present
         * latency) in one query. All-zero/false when the env var is unset;
         * this endpoint itself has no cost gate of its own since it only runs
         * on an explicit debug-client query, never on the hot path. */
        GcnPresentStats p;
        gcn_host_window_get_stats(&p);
        u64 vi_fields = gcn_vi_get_field_count();
        double latency_avg_ms = p.latency_samples
            ? p.latency_sum_ms / (double)p.latency_samples : 0.0;
        n = snprintf(s_resp, GCN_DBG_RESP_CAP,
            "{\"ok\":true,\"enabled\":%s,\"vi_fields\":%llu,"
            "\"posts\":%llu,\"distinct\":%llu,\"coalesced\":%llu,"
            "\"latency_avg_ms\":%.3f,\"latency_max_ms\":%.3f}\n",
            p.enabled ? "true" : "false",
            (unsigned long long)vi_fields,
            (unsigned long long)p.posts,
            (unsigned long long)p.distinct,
            (unsigned long long)p.coalesced,
            latency_avg_ms, p.latency_max_ms);
    }
    else if (!strcmp(cmd, "xfb_pub_count")) {
        n = snprintf(s_resp, GCN_DBG_RESP_CAP,
            "{\"ok\":true,\"pub_count\":%llu,\"generation\":%llu}\n",
            (unsigned long long)gcn_gx_xfb_pub_count(),
            (unsigned long long)gcn_gx_xfb_generation());
    }
    else if (!strcmp(cmd, "gx_draw_state")) {
        /* Retire the GX queue before reading the retained per-config draw
         * snapshots. Each active texture reports a draw-time content hash and
         * physical MEM1 range, which can then be localized with cosim_pages
         * and fetched with read_ram. */
        gcn_gx_pipeline_drain();
        n = gx_raster_debug_draw_state_json(s_resp, GCN_DBG_RESP_CAP);
        if (n < 0)
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"GX snapshot unavailable\"}\n");
    }
    else if (!strcmp(cmd, "tlut_ring_dump")) {
        /* beads-u2x.1 TLUT-COW corruption hunt: prints the last `count`
         * entries of the always-on (opt-in GCN_GX_VK_TLUT_TRACE=1)
         * TLUT/texture residency ring to stderr (this process's err.log),
         * not into the JSON response -- the ring can be thousands of
         * entries, far past a sane response size, and the launch protocol
         * already tails err.log. */
        u32 count = 0; json_uint(line, "count", &count);
        int dumped = gx_vulkan_tlut_ring_dump(count);
        n = snprintf(s_resp, GCN_DBG_RESP_CAP,
            "{\"ok\":true,\"dumped\":%s,\"note\":\"see stderr/err.log\"}\n",
            dumped ? "true" : "false");
    }
    else if (!strcmp(cmd, "screenshot") || !strcmp(cmd, "screenshot_file")) {
        /* Decode the XFB the VI is scanning out into a PPM on disk. Geometry
         * comes from the guest-programmed VI registers (gcn_vi_xfb_info,
         * including default-progressive interlaced-field reconstruction); the
         * XFB pixel format is YUY2 — 4 bytes [Y0,U,Y1,V] per 2 px — converted
         * with the inverse BT.601 matrix exactly as Dolphin's XFB decode
         * (TextureConversionShader.cpp:1009-1035). If the guest has not
         * programmed an XFB, report that honestly (no fake image). */
        u32 fb_addr, fb_w, fb_h, fb_stride;
        char path[512] = {0};
        /* G3: retire pipelined GX work (pending EFB->XFB copies) so the
         * screenshot reads the same bytes the synchronous design would. */
        gcn_gx_pipeline_drain();
        if (!json_str(line, "path", path, sizeof path))
            snprintf(path, sizeof path, "_work/screenshot.ppm");
        if (!gcn_vi_xfb_info(&fb_addr, &fb_w, &fb_h, &fb_stride)) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"VI has no XFB programmed (base/width/ACV zero)\"}\n");
        } else if (!cpu || !cpu->ram ||
                   (u64)fb_addr + (u64)fb_stride * fb_h > cpu->ram_size) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"XFB out of MEM1 range\",\"addr\":%u}\n", fb_addr);
        } else {
            FILE* f = fopen(path, "wb");
            if (!f) {
                n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                    "{\"ok\":false,\"error\":\"cannot open output path\"}\n");
            } else {
                fprintf(f, "P6\n%u %u\n255\n", fb_w, fb_h);
                u64 luma_sum = 0;
                for (u32 y = 0; y < fb_h; y++) {
                    const u8* row = cpu->ram + fb_addr + (u64)y * fb_stride;
                    for (u32 x = 0; x < fb_w; x++) {
                        const u8* px = row + (x / 2u) * 4u;
                        u8 rgb[3];
                        u8 Y = gcn_yuy2_to_rgb(px, (int)(x & 1u), &rgb[0], &rgb[1], &rgb[2]);
                        fwrite(rgb, 1, 3, f);
                        luma_sum += Y;
                    }
                }
                fclose(f);
                /* mean_luma lets a client assert non-black without fetching
                 * the image (16 = XFB black level). */
                n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                    "{\"ok\":true,\"path\":\"%s\",\"width\":%u,\"height\":%u,"
                    "\"xfb_addr\":%u,\"mean_luma\":%.1f,\"frame\":%llu}\n",
                    path, fb_w, fb_h, fb_addr,
                    (double)luma_sum / ((double)fb_w * fb_h),
                    (unsigned long long)gcn_gx_frame_count());
            }
        }
    }
    else if (!strcmp(cmd, "set_input")) {
        /* Debug-surface pad injection (ROADMAP M3/M4): drives the menu through
         * the SI model instead of the hardcoded-neutral report. Every field is
         * optional — unspecified means "leave unchanged" — except "reset":1,
         * which snaps back to the power-on-neutral report (buttons=0, sticks
         * centered 0x80, triggers 0) and ignores every other field. Applies to
         * the single SI instance registered by gcn_si_init (boot.c's GcnSi). */
        u32 buttons = 0, stick_x = 0, stick_y = 0, substick_x = 0, substick_y = 0;
        u32 trigger_l = 0, trigger_r = 0, reset = 0;
        int have_buttons    = json_uint(line, "buttons", &buttons);
        int have_stick_x    = json_uint(line, "stick_x", &stick_x);
        int have_stick_y    = json_uint(line, "stick_y", &stick_y);
        int have_substick_x = json_uint(line, "substick_x", &substick_x);
        int have_substick_y = json_uint(line, "substick_y", &substick_y);
        int have_trigger_l  = json_uint(line, "trigger_l", &trigger_l);
        int have_trigger_r  = json_uint(line, "trigger_r", &trigger_r);
        json_uint(line, "reset", &reset);

        if (!gcn_si_debug_set_input(have_buttons, buttons, have_stick_x, stick_x,
                                     have_stick_y, stick_y, have_substick_x, substick_x,
                                     have_substick_y, substick_y, have_trigger_l, trigger_l,
                                     have_trigger_r, trigger_r, (int)reset)) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"SI not initialized yet\"}\n");
        } else {
            GcnSiPadInput cur;
            gcn_si_debug_get_input(&cur);
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":true,\"buttons\":%u,\"stick_x\":%u,\"stick_y\":%u,"
                "\"substick_x\":%u,\"substick_y\":%u,\"trigger_l\":%u,\"trigger_r\":%u}\n",
                (unsigned)cur.buttons, (unsigned)cur.stick_x, (unsigned)cur.stick_y,
                (unsigned)cur.substick_x, (unsigned)cur.substick_y,
                (unsigned)cur.trigger_l, (unsigned)cur.trigger_r);
        }
    }
    else if (!strcmp(cmd, "insert_disc")) {
        /* ROADMAP M5: the deterministic "menu -> disc screen" driver. Mounts
         * (or replaces the mounted) disc image and fires the cover edges
         * exactly like gcn_di_set_disc always does (mirrors Dolphin's Change
         * Disc / SetDisc — see di.h). Never reads the image into RAM. */
        char path[512] = {0};
        if (!json_str(line, "path", path, sizeof path)) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP, "{\"ok\":false,\"error\":\"missing path\"}\n");
        } else if (!s_ctx.di) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP, "{\"ok\":false,\"error\":\"DI not initialized\"}\n");
        } else if (!gcn_di_set_disc(s_ctx.di, path)) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"cannot mount disc\",\"path\":\"%s\"}\n", path);
        } else {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP, "{\"ok\":true,\"mounted\":\"%s\"}\n", path);
        }
    }
    else if (!strcmp(cmd, "eject_disc")) {
        if (!s_ctx.di) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP, "{\"ok\":false,\"error\":\"DI not initialized\"}\n");
        } else {
            gcn_di_eject_disc(s_ctx.di);
            n = snprintf(s_resp, GCN_DBG_RESP_CAP, "{\"ok\":true,\"ejected\":true}\n");
        }
    }
    else if (!strcmp(cmd, "quit")) {
        s_quit = 1;
        n = snprintf(s_resp, GCN_DBG_RESP_CAP, "{\"ok\":true,\"quit\":true}\n");
    }
    else {
        n = snprintf(s_resp, GCN_DBG_RESP_CAP,
            "{\"ok\":false,\"error\":\"unknown cmd\",\"cmd\":\"%s\"}\n", cmd);
    }

    /* Splice the request id into the response if one was supplied. Static scratch
     * (single-threaded main-loop pump) — a stack buffer this size would overflow. */
    static char s_withid[GCN_DBG_RESP_CAP];
    if (have_id && n > 1 && s_resp[0] == '{') {
        int m = snprintf(s_withid, sizeof s_withid, "{\"id\":%u,%s", id, s_resp + 1);
        if (m > 0 && m < (int)sizeof s_withid) { memcpy(s_resp, s_withid, m); n = m; }
    }
    send_all(c, s_resp, n);
}

static void set_nonblocking(SOCKET s) {
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
}

int gcn_debug_server_start(const GcnDebugCtx* ctx) {
    const char* penv = getenv("GCN_DEBUG_PORT");
    if (!penv || !*penv) return 0;         /* disabled — rings still record */
    int port = atoi(penv);
    if (port <= 0 || port > 65535) return 0;

    s_ctx = *ctx;
    const char* cosim = getenv("GCN_COSIM");
    s_cosim_enabled = cosim && *cosim && *cosim != '0';
    s_cosim_parked = 0;
    s_cosim_budget = 0;
    s_cosim_instructions = 0;
    s_cosim_stop_pc_armed = 0;
    s_cosim_stop_pc = 0;
    s_checkpoint_armed = 0;
    s_checkpoint_parked = 0;
    s_checkpoint_pc = 0;
    s_checkpoint_have_gpr = 0;
    s_checkpoint_gpr = 0;
    s_checkpoint_gpr_value = 0;
    s_checkpoint_have_lr = 0;
    s_checkpoint_lr = 0;
    u32 auto_pc = 0, auto_gpr = 0, auto_gpr_value = 0, auto_lr = 0;
    int have_auto_pc = env_u32("GCN_CHECKPOINT_PC", &auto_pc);
    int have_auto_gpr = env_u32("GCN_CHECKPOINT_GPR", &auto_gpr);
    int have_auto_gpr_value =
        env_u32("GCN_CHECKPOINT_GPR_VALUE", &auto_gpr_value);
    int have_auto_lr = env_u32("GCN_CHECKPOINT_LR", &auto_lr);
    if (have_auto_pc > 0 && have_auto_lr >= 0 &&
        ((have_auto_gpr == 0 && have_auto_gpr_value == 0) ||
         (have_auto_gpr > 0 && have_auto_gpr_value > 0 && auto_gpr < 32u))) {
        s_checkpoint_armed = 1;
        s_checkpoint_pc = auto_pc;
        s_checkpoint_have_gpr = have_auto_gpr > 0;
        s_checkpoint_gpr = auto_gpr;
        s_checkpoint_gpr_value = auto_gpr_value;
        s_checkpoint_have_lr = have_auto_lr > 0;
        s_checkpoint_lr = auto_lr;
    } else if (have_auto_pc != 0 || have_auto_gpr != 0 ||
               have_auto_gpr_value != 0 || have_auto_lr != 0) {
        fprintf(stderr,
                "gcn checkpoint: launch-time auto-arm disabled; "
                "GCN_CHECKPOINT_PC must be valid and GCN_CHECKPOINT_GPR/"
                "GCN_CHECKPOINT_GPR_VALUE must be supplied together with "
                "GPR in 0..31\n");
    }
    for (int i = 0; i < GCN_DBG_MAX_CLIENTS; i++) s_clients[i].sock = INVALID_SOCKET;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;

    s_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen == INVALID_SOCKET) { WSACleanup(); return -1; }
    int yes = 1;
    setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* localhost only */
    addr.sin_port = htons((u_short)port);
    if (bind(s_listen, (struct sockaddr*)&addr, sizeof addr) == SOCKET_ERROR ||
        listen(s_listen, 4) == SOCKET_ERROR) {
        closesocket(s_listen); s_listen = INVALID_SOCKET; WSACleanup(); return -1;
    }
    set_nonblocking(s_listen);
    s_enabled = 1; s_quit = 0;
    fprintf(stdout, "gcn debug: TCP debug server on 127.0.0.1:%d "
                    "(always-on rings; JSON-over-newline)\n", port);
    if (s_checkpoint_armed) {
        fprintf(stdout, "gcn checkpoint: launch-time auto-arm pc=0x%08X",
                s_checkpoint_pc);
        if (s_checkpoint_have_gpr)
            fprintf(stdout, " r%u=0x%08X",
                    s_checkpoint_gpr, s_checkpoint_gpr_value);
        if (s_checkpoint_have_lr)
            fprintf(stdout, " lr=0x%08X", s_checkpoint_lr);
        fputc('\n', stdout);
    }
    if (s_cosim_enabled)
        fprintf(stdout, "gcn cosim: deterministic interpreter path armed; "
                        "parked before reset-vector instruction 0\n");
    fflush(stdout);
    return port;
}

void gcn_debug_server_pump(void) {
    if (!s_enabled) return;

    /* Accept any pending connections into a free slot. */
    for (;;) {
        SOCKET cs = accept(s_listen, NULL, NULL);
        if (cs == INVALID_SOCKET) break;
        int slot = -1;
        for (int i = 0; i < GCN_DBG_MAX_CLIENTS; i++)
            if (s_clients[i].sock == INVALID_SOCKET) { slot = i; break; }
        if (slot < 0) { closesocket(cs); continue; }   /* full */
        set_nonblocking(cs);
        s_clients[slot].sock = cs;
        s_clients[slot].len = 0;
    }

    /* Drain ready bytes from each client, dispatch complete lines. */
    for (int i = 0; i < GCN_DBG_MAX_CLIENTS; i++) {
        Client* c = &s_clients[i];
        if (c->sock == INVALID_SOCKET) continue;

        for (;;) {
            int room = GCN_DBG_LINE_CAP - 1 - c->len;
            if (room <= 0) { c->len = 0; room = GCN_DBG_LINE_CAP - 1; }  /* overlong: reset */
            int r = recv(c->sock, c->line + c->len, room, 0);
            if (r == 0) { client_close(c); break; }         /* peer closed */
            if (r == SOCKET_ERROR) {
                if (WSAGetLastError() != WSAEWOULDBLOCK) client_close(c);
                break;
            }
            c->len += r;
            c->line[c->len] = 0;

            /* Process each newline-terminated request in the buffer. */
            int start = 0;
            for (int j = 0; j < c->len; j++) {
                if (c->line[j] == '\n') {
                    c->line[j] = 0;
                    if (j > start) handle_line(c, c->line + start);
                    if (c->sock == INVALID_SOCKET) break;   /* closed mid-dispatch */
                    start = j + 1;
                }
            }
            if (c->sock == INVALID_SOCKET) break;
            /* Shift any partial trailing line to the front. */
            if (start > 0) {
                c->len -= start;
                memmove(c->line, c->line + start, c->len);
            }
        }
    }
}

const GcnDebugCtx* gcn_debug_server_ctx(void) { return &s_ctx; }

int gcn_debug_server_quit_requested(void) { return s_quit; }

int gcn_debug_server_cosim_enabled(void) { return s_cosim_enabled; }

int gcn_debug_server_checkpoint_before_block(void) {
    CPUState* cpu = s_ctx.cpu;
    if (!s_enabled || !s_checkpoint_armed || !cpu ||
        cpu->pc != s_checkpoint_pc)
        return !s_quit;
    if (s_checkpoint_have_gpr &&
        cpu->gpr[s_checkpoint_gpr] != s_checkpoint_gpr_value)
        return !s_quit;
    if (s_checkpoint_have_lr && cpu->lr != s_checkpoint_lr)
        return !s_quit;

    s_checkpoint_armed = 0;
    s_checkpoint_parked = 1;
    fprintf(stdout,
        "gcn checkpoint: parked before pc=0x%08X at block=%llu%s\n",
        cpu->pc, (unsigned long long)gcn_ring_block_index(),
        (s_checkpoint_have_gpr || s_checkpoint_have_lr)
            ? " (register condition matched)" : "");
    fflush(stdout);

    /* SNAPSHOT_RESUME pass A (docs/SNAPSHOT_RESUME.md): the checkpoint park
     * above is exactly the "clean dispatcher boundary" the spec requires —
     * dispatch.c's call site (gcn_debug_server_checkpoint_before_block,
     * dispatch.c:591) runs strictly before gcn_dispatch_native/
     * gcn_interpreter_step for the pending block, so no instruction is ever
     * mid-execution here. GCN_SNAPSHOT_SAVE=<path> captures the full
     * machine state right now; GCN_SNAPSHOT_EXIT=1 exits immediately after
     * (success or failure) instead of falling through to the interactive
     * TCP park loop below. */
    const char* snap_path = getenv("GCN_SNAPSHOT_SAVE");
    if (snap_path && *snap_path) {
        char why[256];
        int rc = gcn_snapshot_save(snap_path, cpu, why, sizeof why);
        if (rc == GCN_SNAPSHOT_OK)
            fprintf(stdout, "gcn snapshot: saved to '%s'\n", snap_path);
        else
            fprintf(stderr, "gcn snapshot: FAILED (rc=%d): %s\n", rc, why);
        fflush(stdout);
        fflush(stderr);
        const char* exit_env = getenv("GCN_SNAPSHOT_EXIT");
        if (exit_env && *exit_env && *exit_env != '0')
            exit(rc == GCN_SNAPSHOT_OK ? 0 : 1);
    }

    while (s_enabled && !s_quit && s_checkpoint_parked) {
        gcn_debug_server_pump();
        Sleep(1);
    }
    return !s_quit;
}

int gcn_debug_server_cosim_before_instruction(void) {
    if (!s_cosim_enabled)
        return !s_quit;
    if (s_cosim_stop_pc_armed && s_ctx.cpu &&
        s_ctx.cpu->pc == s_cosim_stop_pc) {
        s_cosim_stop_pc_armed = 0;
        s_cosim_budget = 0;
    }
    s_cosim_parked = 1;
    while (s_enabled && !s_quit && s_cosim_budget == 0u) {
        gcn_debug_server_pump();
        Sleep(1);
    }
    s_cosim_parked = 0;
    return !s_quit;
}

void gcn_debug_server_cosim_after_instruction(void) {
    if (!s_cosim_enabled)
        return;
    s_cosim_instructions++;
    if (s_cosim_budget > 0u)
        s_cosim_budget--;
}

void gcn_debug_server_park(void) {
    if (!s_enabled) return;
    fprintf(stdout, "gcn debug: parked — serving debug queries; send \"quit\" to exit.\n");
    fflush(stdout);
    while (s_enabled && !s_quit) {
        gcn_debug_server_pump();
        Sleep(1);
    }
}

void gcn_debug_server_stop(void) {
    if (!s_enabled) return;
    for (int i = 0; i < GCN_DBG_MAX_CLIENTS; i++) client_close(&s_clients[i]);
    if (s_listen != INVALID_SOCKET) { closesocket(s_listen); s_listen = INVALID_SOCKET; }
    WSACleanup();
    s_enabled = 0;
    s_cosim_enabled = 0;
    s_cosim_parked = 0;
    s_cosim_budget = 0;
    s_cosim_stop_pc_armed = 0;
    s_checkpoint_armed = 0;
    s_checkpoint_parked = 0;
}
