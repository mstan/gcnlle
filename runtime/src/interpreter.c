/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Native-first Gekko interpreter fallback and append-only miss capture.
 *
 * Decode and architectural semantics are shared with this repository's own
 * DolRecomp frontend/helpers. No ModernGekko or Dolphin CPU implementation is
 * used. This is a fallback beneath validated native candidates, never a boot
 * engine selected ahead of them.
 */
#include "cpu/interpreter.h"

/* decoder.h normally supplies the recompiler's byte-for-byte mirrored type
 * header. The runtime contract already included its identical mirror above;
 * suppress the duplicate inline helper definitions in this translation unit. */
#define DOLRECOMP_TYPES_H
#include "backend/ppc_cycles.h"
#include "cpu/native_code.h"
#include "cpu/title_module.h"
#include "cpu/overlay_module.h"
#include "debug/rings.h"
#include "frontend/decoder.h"
#include "memory/memory.h"
#include "util/crc32.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define GCN_ACCESS _access
#define GCN_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define GCN_ACCESS access
#define GCN_MKDIR(path) mkdir((path), 0777)
#endif

#define MISS_CAP (1u << 19)
#define EDGE_CAP (1u << 18)
#define PAGE_SIZE 4096u
#define XER_SO 0x80000000u
#define XER_OV 0x40000000u
#define XER_CA 0x20000000u
#define XER_BC 0x0000007Fu
#define MSR_FP 0x00002000u

typedef struct {
    u32 pc;
    u32 crc;
    u8 used;
} MissKey;

typedef struct {
    u32 from;
    u32 to;
    u8 used;
} EdgeKey;

static MissKey s_misses[MISS_CAP];
static EdgeKey s_edges[EDGE_CAP];
static u64 s_instruction_count;
static u64 s_unique_miss_count;
static u64 s_edge_count;
static FILE* s_journal;
static char s_blob_dir[1024];
static int s_capture_initialized;
static int s_log_native_misses;

static u32 hash_pair(u32 a, u32 b) {
    u32 x = a * 0x9E3779B1u ^ b * 0x85EBCA77u;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    return x;
}

static void capture_init(void) {
    if (s_capture_initialized)
        return;
    s_capture_initialized = 1;
    atexit(gcn_interpreter_shutdown);
    const char* log_native_misses = getenv("GCN_INTERP_LOG_NATIVE_MISSES");
    s_log_native_misses = log_native_misses && *log_native_misses &&
                           strcmp(log_native_misses, "0") != 0;
    const char* journal = getenv("GCN_INTERP_JOURNAL");
    const char* blobs = getenv("GCN_INTERP_BLOB_DIR");
    if (blobs && *blobs) {
        snprintf(s_blob_dir, sizeof s_blob_dir, "%s", blobs);
        if (GCN_ACCESS(s_blob_dir, 0) != 0 && GCN_MKDIR(s_blob_dir) != 0 &&
            errno != EEXIST) {
            fprintf(stderr, "[native-miss] cannot create blob directory '%s': %s\n",
                    s_blob_dir, strerror(errno));
            s_blob_dir[0] = '\0';
        }
    }
    if (journal && *journal) {
        s_journal = fopen(journal, "ab");
        if (!s_journal)
            fprintf(stderr, "[native-miss] cannot append journal '%s': %s\n",
                    journal, strerror(errno));
    }
}

static const u8* executable_page(CPUState* cpu, u32 pc, u32* page_out) {
    u32 phys = pc & 0x1FFFFFFFu;
    u32 page = pc & ~(PAGE_SIZE - 1u);
    if (phys < cpu->ram_size && phys + PAGE_SIZE <= cpu->ram_size) {
        if (page_out) *page_out = page;
        return cpu->ram + (phys & ~(PAGE_SIZE - 1u));
    }
    if (pc >= GCN_ROM_WINDOW_BASE && cpu->rom_window) {
        u32 off = pc - GCN_ROM_WINDOW_BASE;
        if ((off & ~(PAGE_SIZE - 1u)) + PAGE_SIZE <= cpu->rom_window_size) {
            if (page_out) *page_out = page;
            return cpu->rom_window + (off & ~(PAGE_SIZE - 1u));
        }
    }
    if (page_out) *page_out = page;
    return NULL;
}

/* --- native-miss page-CRC memo --------------------------------------
 *
 * The miss identity key is (pc, page CRC): the CRC distinguishes different
 * dynamically-written code that lands at the same pc. Recomputing a 4 KiB
 * CRC32 on EVERY miss made duplicate misses in interpreted hot loops cost
 * ~35K host cycles each (the 2026-08-03 WW route census attributed 27.2% of
 * all pc-attributed cycles to the three blocks of one dynamically-written
 * icbi loop at 0x812FFF80, almost all of it this hash). A page's content can
 * only change through the gcn_native_code_invalidate funnel (guest stores
 * via memory.c, device DMA, icbi via cpu_glue.c and the batch fast path), so
 * native_code.c keeps a read-and-clear per-page staleness bit set in that
 * same funnel and the CRC is recomputed exactly when a recompute could
 * return a different value -- identical identity semantics, no per-duplicate
 * hashing. ROM-window pages are immutable once loaded: seen-only memo.
 *
 * DELIBERATE identity refinement: plain loads/stores emitted by the
 * recompiler are inlined (generated_abi.h dolrecomp_mem_write*_fast) and do
 * NOT pass the funnel -- exactly as they do not touch the native-code fence.
 * A page identity therefore refreshes at icache-relevant boundaries (icbi,
 * dcbz, string/multiple/atomic stores, interpreted stores, DMA and device
 * RAM writes), not on every data store into a mixed code/data page. On the
 * WW route this drops ~3,900 junk (pc, crc) identities minted purely by
 * low-mem/heap data churn between misses, while the distinct missed-pc set
 * and every icbi-delimited code state stay bit-identical (verified
 * 2026-08-03: golden XFB chain unchanged, missed-pc sets equal). Guest code
 * written by plain stores and executed WITHOUT icbi keeps its old identity
 * here for the same reason it keeps executing stale native code: the
 * runtime models the un-invalidated icache, consistently. */
#define CRC_RAM_PAGE_CAP (GC_MAIN_RAM_SIZE / PAGE_SIZE)
#define CRC_ROM_PAGE_CAP 1024u
static u32 s_page_crc[CRC_RAM_PAGE_CAP];
static u8 s_page_crc_seen[(CRC_RAM_PAGE_CAP + 7u) / 8u];
static u32 s_rom_page_crc[CRC_ROM_PAGE_CAP];
static u8 s_rom_page_crc_seen[(CRC_ROM_PAGE_CAP + 7u) / 8u];
static u64 s_page_crc_recomputes;

u64 gcn_interpreter_page_crc_recomputes(void) {
    return s_page_crc_recomputes;
}

static u32 miss_page_crc(CPUState* cpu, u32 pc, const u8* bytes) {
    if (!bytes)
        return 0u;
    u32 phys = pc & 0x1FFFFFFFu;
    if (phys < cpu->ram_size) {          /* executable_page's RAM branch */
        u32 pidx = phys / PAGE_SIZE;
        u8 mask = (u8)(1u << (pidx & 7u));
        /* take FIRST: the read-and-clear must happen even when the page has
         * never been seen, so a pre-set stale bit doesn't linger. */
        bool stale = gcn_native_code_page_content_stale_take(pc);
        if (!stale && pidx < CRC_RAM_PAGE_CAP &&
            (s_page_crc_seen[pidx >> 3] & mask))
            return s_page_crc[pidx];
        u32 crc = gcn_crc32(bytes, PAGE_SIZE);
        s_page_crc_recomputes++;
        if (pidx < CRC_RAM_PAGE_CAP) {
            s_page_crc[pidx] = crc;
            s_page_crc_seen[pidx >> 3] |= mask;
        }
        return crc;
    }
    /* executable_page's ROM-window branch */
    u32 pidx = (pc - GCN_ROM_WINDOW_BASE) / PAGE_SIZE;
    u8 mask = (u8)(1u << (pidx & 7u));
    if (pidx < CRC_ROM_PAGE_CAP && (s_rom_page_crc_seen[pidx >> 3] & mask))
        return s_rom_page_crc[pidx];
    u32 crc = gcn_crc32(bytes, PAGE_SIZE);
    s_page_crc_recomputes++;
    if (pidx < CRC_ROM_PAGE_CAP) {
        s_rom_page_crc[pidx] = crc;
        s_rom_page_crc_seen[pidx >> 3] |= mask;
    }
    return crc;
}

static void write_page_blob(u32 page, u32 crc, const u8* bytes) {
    if (!bytes || !s_blob_dir[0])
        return;
    char path[1200];
    snprintf(path, sizeof path, "%s/%08X_%08X.bin", s_blob_dir, page, crc);
    if (GCN_ACCESS(path, 0) == 0)
        return;
    char temp[1240];
    snprintf(temp, sizeof temp, "%s.tmp.%lu", path,
             (unsigned long)(s_unique_miss_count & 0xFFFFFFFFu));
    FILE* out = fopen(temp, "wb");
    if (!out)
        return;
    int wrote = fwrite(bytes, 1, PAGE_SIZE, out) == PAGE_SIZE;
    int closed = fclose(out) == 0;
    if (!wrote || !closed) {
        remove(temp);
        return;
    }
    if (rename(temp, path) != 0)
        remove(temp);
}

int gcn_interpreter_note_native_miss(CPUState* cpu) {
    capture_init();
    u32 page = 0;
    const u8* bytes = executable_page(cpu, cpu->pc, &page);
    u32 crc = miss_page_crc(cpu, cpu->pc, bytes);
    u32 slot = hash_pair(cpu->pc, crc) & (MISS_CAP - 1u);
    for (u32 probe = 0; probe < MISS_CAP; probe++) {
        MissKey* key = &s_misses[(slot + probe) & (MISS_CAP - 1u)];
        if (key->used && key->pc == cpu->pc && key->crc == crc)
            return 0;
        if (!key->used) {
            key->used = 1;
            key->pc = cpu->pc;
            key->crc = crc;
            break;
        }
        if (probe + 1u == MISS_CAP) {
            fprintf(stderr, "[native-miss] identity table exhausted\n");
            return 0;
        }
    }

    s_unique_miss_count++;
    u32 raw = mem_read32(cpu, cpu->pc, cpu->pc);
    PPCInst inst = ppc_decode(raw, cpu->pc);
    gcn_ring_event(GCN_EV_NATIVE_MISS, cpu->pc, crc, cpu->pc);
    if (s_log_native_misses) {
        fprintf(stderr,
                "[native-miss] #%llu pc=%08X lr=%08X cycles=%llu page=%08X "
                "crc32=%08X raw=%08X op=%s -> interpreter\n",
                (unsigned long long)s_unique_miss_count, cpu->pc, cpu->lr,
                (unsigned long long)cpu->cycles, page, crc, raw,
                ppc_op_name(inst.op));
        fflush(stderr);
    }

    write_page_blob(page, crc, bytes);
    if (s_journal) {
        fprintf(s_journal,
                "{\"version\":1,\"event\":\"native_miss\",\"seq\":%llu,"
                "\"pc\":%u,\"lr\":%u,\"cycles\":%llu,\"page\":%u,"
                "\"page_crc32\":\"%08x\",\"raw\":\"%08x\",\"op\":\"%s\"}\n",
                (unsigned long long)s_unique_miss_count, cpu->pc, cpu->lr,
                (unsigned long long)cpu->cycles, page, crc, raw,
                ppc_op_name(inst.op));
        fflush(s_journal);
    }
    return 1;
}

static void note_edge(u32 from, u32 to, u64 cycles) {
    if (to == from + 4u)
        return;
    u32 slot = hash_pair(from, to) & (EDGE_CAP - 1u);
    for (u32 probe = 0; probe < EDGE_CAP; probe++) {
        EdgeKey* key = &s_edges[(slot + probe) & (EDGE_CAP - 1u)];
        if (key->used && key->from == from && key->to == to)
            return;
        if (!key->used) {
            key->used = 1;
            key->from = from;
            key->to = to;
            break;
        }
        if (probe + 1u == EDGE_CAP)
            return;
    }
    s_edge_count++;
    gcn_ring_event(GCN_EV_INTERP_EDGE, from, to, from);
    if (s_journal) {
        fprintf(s_journal,
                "{\"version\":1,\"event\":\"interp_edge\",\"seq\":%llu,"
                "\"from\":%u,\"to\":%u,\"cycles\":%llu}\n",
                (unsigned long long)s_edge_count, from, to,
                (unsigned long long)cycles);
        fflush(s_journal);
    }
}

static u32 mask32(u8 mb, u8 me) {
    u32 mask = 0;
    u8 bit = mb;
    for (;;) {
        mask |= 0x80000000u >> bit;
        if (bit == me)
            return mask;
        bit = (u8)((bit + 1u) & 31u);
    }
}

static u32 rotl32(u32 value, u32 sh) {
    sh &= 31u;
    return sh ? (value << sh) | (value >> (32u - sh)) : value;
}

static void set_cr_field(CPUState* cpu, u8 field, u32 bits) {
    u32 shift = 4u * (7u - field);
    cpu->cr = (cpu->cr & ~(0xFu << shift)) | ((bits & 0xFu) << shift);
}

static void compare_s32(CPUState* cpu, u8 field, s32 a, s32 b) {
    u32 bits = a < b ? 8u : a > b ? 4u : 2u;
    set_cr_field(cpu, field, bits | ((cpu->xer >> 31) & 1u));
}

static void compare_u32(CPUState* cpu, u8 field, u32 a, u32 b) {
    u32 bits = a < b ? 8u : a > b ? 4u : 2u;
    set_cr_field(cpu, field, bits | ((cpu->xer >> 31) & 1u));
}

static void compare_f64(CPUState* cpu, u8 field, f64 a, f64 b) {
    u32 bits = a < b ? 8u : a > b ? 4u : a == b ? 2u : 1u;
    set_cr_field(cpu, field, bits);
}

static void set_cr0(CPUState* cpu, u32 value) {
    compare_s32(cpu, 0, (s32)value, 0);
}

static void set_cr1(CPUState* cpu) {
    cpu->cr = (cpu->cr & 0xF0FFFFFFu) | ((cpu->fpscr >> 4) & 0x0F000000u);
}

static void set_ca(CPUState* cpu, int carry) {
    cpu->xer = (cpu->xer & ~XER_CA) | (carry ? XER_CA : 0u);
}

static u32 add_ca(CPUState* cpu, u32 a, u32 b, u32 carry) {
    u64 result = (u64)a + (u64)b + carry;
    set_ca(cpu, (u32)(result >> 32));
    return (u32)result;
}

static u32 dform_ea(CPUState* cpu, const PPCInst* in, int update) {
    return (!update && in->rA == 0 ? 0u : cpu->gpr[in->rA]) +
           (u32)(s32)in->simm;
}

static u32 xform_ea(CPUState* cpu, const PPCInst* in, int update) {
    return (!update && in->rA == 0 ? 0u : cpu->gpr[in->rA]) +
           cpu->gpr[in->rB];
}

static f32 bits_f32(u32 bits) {
    f32 result;
    memcpy(&result, &bits, sizeof result);
    return result;
}

static u32 f32_bits(f32 value) {
    u32 result;
    memcpy(&result, &value, sizeof result);
    return result;
}

static f64 bits_f64(u64 bits) {
    f64 result;
    memcpy(&result, &bits, sizeof result);
    return result;
}

static u64 f64_bits(f64 value) {
    u64 result;
    memcpy(&result, &value, sizeof result);
    return result;
}

static int is_fp_op(PPCOpcode op) {
    return (op >= PPC_OP_LFS && op <= PPC_OP_PS_SEL) ||
           op == PPC_OP_STFIWX;
}

static int branch_condition(CPUState* cpu, u8 bo, u8 bi) {
    int ctr_ok = 1;
    int cr_ok = 1;
    if (!(bo & 0x04u)) {
        cpu->ctr--;
        ctr_ok = ((cpu->ctr != 0u) ? 1 : 0) ^ ((bo >> 1) & 1u);
    }
    if (!(bo & 0x10u)) {
        int cr_bit = (cpu->cr & (0x80000000u >> bi)) != 0;
        cr_ok = cr_bit == (int)((bo >> 3) & 1u);
    }
    return ctr_ok && cr_ok;
}

static void load_integer(CPUState* cpu, const PPCInst* in, u32 width,
                         int sign_extend, int indexed, int update) {
    u32 ea = indexed ? xform_ea(cpu, in, update) : dform_ea(cpu, in, update);
    u32 value;
    if (width == 32) value = mem_read32(cpu, ea, in->address);
    else if (width == 16) value = mem_read16(cpu, ea, in->address);
    else value = mem_read8(cpu, ea, in->address);
    if (sign_extend)
        value = width == 16 ? (u32)(s32)(s16)value : (u32)(s32)(s8)value;
    cpu->gpr[in->rD] = value;
    if (update) cpu->gpr[in->rA] = ea;
}

static void store_integer(CPUState* cpu, const PPCInst* in, u32 width,
                          int indexed, int update) {
    u32 ea = indexed ? xform_ea(cpu, in, update) : dform_ea(cpu, in, update);
    u32 value = cpu->gpr[in->rS];
    if (width == 32) mem_write32(cpu, ea, value, in->address);
    else if (width == 16) mem_write16(cpu, ea, (u16)value, in->address);
    else mem_write8(cpu, ea, (u8)value, in->address);
    if (update) cpu->gpr[in->rA] = ea;
}

static void load_float(CPUState* cpu, const PPCInst* in, int single,
                       int indexed, int update) {
    u32 ea = indexed ? xform_ea(cpu, in, update) : dform_ea(cpu, in, update);
    if (single) {
        f64 value = (f64)bits_f32(mem_read32(cpu, ea, in->address));
        cpu->fpr[in->rD] = value;
        cpu->ps1[in->rD] = value;
    } else {
        cpu->fpr[in->rD] = bits_f64(mem_read64(cpu, ea, in->address));
    }
    if (update) cpu->gpr[in->rA] = ea;
}

static void store_float(CPUState* cpu, const PPCInst* in, int single,
                        int indexed, int update) {
    u32 ea = indexed ? xform_ea(cpu, in, update) : dform_ea(cpu, in, update);
    if (single)
        mem_write32(cpu, ea, f32_bits((f32)cpu->fpr[in->rS]), in->address);
    else
        mem_write64(cpu, ea, f64_bits(cpu->fpr[in->rS]), in->address);
    if (update) cpu->gpr[in->rA] = ea;
}

static int execute_integer(CPUState* cpu, const PPCInst* in) {
    u32 a, b, r, ea;
    u64 wide;
    s64 swide;
    int overflow = 0;
    switch (in->op) {
    case PPC_OP_MULLI:
        cpu->gpr[in->rD] = (u32)((s64)(s32)cpu->gpr[in->rA] * in->simm);
        break;
    case PPC_OP_SUBFIC:
        cpu->gpr[in->rD] = add_ca(cpu, ~cpu->gpr[in->rA],
                                  (u32)(s32)in->simm, 1u);
        break;
    case PPC_OP_ADDI:
        cpu->gpr[in->rD] = (in->rA ? cpu->gpr[in->rA] : 0u) +
                           (u32)(s32)in->simm;
        break;
    case PPC_OP_ADDIS:
        cpu->gpr[in->rD] = (in->rA ? cpu->gpr[in->rA] : 0u) +
                           ((u32)(s32)in->simm << 16);
        break;
    case PPC_OP_ADDIC:
    case PPC_OP_ADDIC_DOT:
        cpu->gpr[in->rD] = add_ca(cpu, cpu->gpr[in->rA],
                                  (u32)(s32)in->simm, 0);
        break;
    case PPC_OP_CMPI:
        compare_s32(cpu, in->crfD, (s32)cpu->gpr[in->rA], in->simm);
        break;
    case PPC_OP_CMPLI:
        compare_u32(cpu, in->crfD, cpu->gpr[in->rA], in->uimm);
        break;
    case PPC_OP_CMP:
        compare_s32(cpu, in->crfD, (s32)cpu->gpr[in->rA],
                    (s32)cpu->gpr[in->rB]);
        break;
    case PPC_OP_CMPL:
        compare_u32(cpu, in->crfD, cpu->gpr[in->rA], cpu->gpr[in->rB]);
        break;
    case PPC_OP_ORI: cpu->gpr[in->rA] = cpu->gpr[in->rS] | in->uimm; break;
    case PPC_OP_ORIS: cpu->gpr[in->rA] = cpu->gpr[in->rS] | ((u32)in->uimm << 16); break;
    case PPC_OP_XORI: cpu->gpr[in->rA] = cpu->gpr[in->rS] ^ in->uimm; break;
    case PPC_OP_XORIS: cpu->gpr[in->rA] = cpu->gpr[in->rS] ^ ((u32)in->uimm << 16); break;
    case PPC_OP_ANDI: cpu->gpr[in->rA] = cpu->gpr[in->rS] & in->uimm; break;
    case PPC_OP_ANDIS: cpu->gpr[in->rA] = cpu->gpr[in->rS] & ((u32)in->uimm << 16); break;

    case PPC_OP_ADD: case PPC_OP_ADDO:
        a = cpu->gpr[in->rA]; b = cpu->gpr[in->rB]; r = a + b;
        cpu->gpr[in->rD] = r; overflow = ppc_add_overflowed(a, b, r); break;
    case PPC_OP_ADDC: case PPC_OP_ADDCO:
        a = cpu->gpr[in->rA]; b = cpu->gpr[in->rB];
        r = add_ca(cpu, a, b, 0); cpu->gpr[in->rD] = r;
        overflow = ppc_add_overflowed(a, b, r); break;
    case PPC_OP_ADDE: case PPC_OP_ADDEO: {
        u32 carry = (cpu->xer & XER_CA) != 0;
        a = cpu->gpr[in->rA]; b = cpu->gpr[in->rB];
        wide = (u64)a + b + carry; r = (u32)wide;
        set_ca(cpu, (u32)(wide >> 32)); cpu->gpr[in->rD] = r;
        overflow = ppc_add_overflowed(a, b + carry, r); break;
    }
    case PPC_OP_ADDME: case PPC_OP_ADDMEO: {
        u32 carry = (cpu->xer & XER_CA) != 0;
        a = cpu->gpr[in->rA]; r = add_ca(cpu, a, 0xFFFFFFFFu, carry);
        cpu->gpr[in->rD] = r; overflow = ppc_add_overflowed(a, 0xFFFFFFFFu + carry, r); break;
    }
    case PPC_OP_ADDZE: case PPC_OP_ADDZEO: {
        u32 carry = (cpu->xer & XER_CA) != 0;
        a = cpu->gpr[in->rA]; r = add_ca(cpu, a, 0, carry);
        cpu->gpr[in->rD] = r; overflow = ppc_add_overflowed(a, carry, r); break;
    }
    case PPC_OP_SUBF: case PPC_OP_SUBFO:
        a = cpu->gpr[in->rA]; b = cpu->gpr[in->rB]; r = b - a;
        cpu->gpr[in->rD] = r; overflow = ppc_add_overflowed(~a, b, r); break;
    case PPC_OP_SUBFC: case PPC_OP_SUBFCO:
        a = cpu->gpr[in->rA]; b = cpu->gpr[in->rB];
        r = add_ca(cpu, ~a, b, 1); cpu->gpr[in->rD] = r;
        overflow = ppc_add_overflowed(~a, b, r); break;
    case PPC_OP_SUBFE: case PPC_OP_SUBFEO: {
        u32 carry = (cpu->xer & XER_CA) != 0;
        a = cpu->gpr[in->rA]; b = cpu->gpr[in->rB];
        r = add_ca(cpu, ~a, b, carry); cpu->gpr[in->rD] = r;
        overflow = ppc_add_overflowed(~a, b + carry, r); break;
    }
    case PPC_OP_SUBFME: case PPC_OP_SUBFMEO: {
        u32 carry = (cpu->xer & XER_CA) != 0;
        a = cpu->gpr[in->rA]; r = add_ca(cpu, ~a, 0xFFFFFFFFu, carry);
        cpu->gpr[in->rD] = r; overflow = ppc_add_overflowed(~a, 0xFFFFFFFFu + carry, r); break;
    }
    case PPC_OP_SUBFZE: case PPC_OP_SUBFZEO: {
        u32 carry = (cpu->xer & XER_CA) != 0;
        a = cpu->gpr[in->rA]; r = add_ca(cpu, ~a, 0, carry);
        cpu->gpr[in->rD] = r; overflow = ppc_add_overflowed(~a, carry, r); break;
    }
    case PPC_OP_NEG: case PPC_OP_NEGO:
        a = cpu->gpr[in->rA]; cpu->gpr[in->rD] = 0u - a;
        overflow = a == 0x80000000u; break;
    case PPC_OP_MULLW: case PPC_OP_MULLWO:
        swide = (s64)(s32)cpu->gpr[in->rA] * (s64)(s32)cpu->gpr[in->rB];
        cpu->gpr[in->rD] = (u32)swide;
        overflow = swide != (s64)(s32)(u32)swide; break;
    case PPC_OP_MULHW:
        swide = (s64)(s32)cpu->gpr[in->rA] * (s64)(s32)cpu->gpr[in->rB];
        cpu->gpr[in->rD] = (u32)((u64)swide >> 32); break;
    case PPC_OP_MULHWU:
        wide = (u64)cpu->gpr[in->rA] * cpu->gpr[in->rB];
        cpu->gpr[in->rD] = (u32)(wide >> 32); break;
    case PPC_OP_DIVW: case PPC_OP_DIVWO:
        a = cpu->gpr[in->rA]; b = cpu->gpr[in->rB];
        overflow = b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu);
        cpu->gpr[in->rD] = overflow ? 0u : (u32)((s32)a / (s32)b); break;
    case PPC_OP_DIVWU: case PPC_OP_DIVWUO:
        a = cpu->gpr[in->rA]; b = cpu->gpr[in->rB]; overflow = b == 0;
        cpu->gpr[in->rD] = b ? a / b : 0u; break;

    case PPC_OP_AND: cpu->gpr[in->rA] = cpu->gpr[in->rS] & cpu->gpr[in->rB]; break;
    case PPC_OP_ANDC: cpu->gpr[in->rA] = cpu->gpr[in->rS] & ~cpu->gpr[in->rB]; break;
    case PPC_OP_OR: cpu->gpr[in->rA] = cpu->gpr[in->rS] | cpu->gpr[in->rB]; break;
    case PPC_OP_ORC: cpu->gpr[in->rA] = cpu->gpr[in->rS] | ~cpu->gpr[in->rB]; break;
    case PPC_OP_XOR: cpu->gpr[in->rA] = cpu->gpr[in->rS] ^ cpu->gpr[in->rB]; break;
    case PPC_OP_NAND: cpu->gpr[in->rA] = ~(cpu->gpr[in->rS] & cpu->gpr[in->rB]); break;
    case PPC_OP_NOR: cpu->gpr[in->rA] = ~(cpu->gpr[in->rS] | cpu->gpr[in->rB]); break;
    case PPC_OP_EQV: cpu->gpr[in->rA] = ~(cpu->gpr[in->rS] ^ cpu->gpr[in->rB]); break;
    case PPC_OP_CNTLZW:
#if defined(__GNUC__) || defined(__clang__)
        cpu->gpr[in->rA] = cpu->gpr[in->rS] ? (u32)__builtin_clz(cpu->gpr[in->rS]) : 32u;
#else
        r = cpu->gpr[in->rS]; a = 0; while (a < 32 && !(r & (0x80000000u >> a))) a++;
        cpu->gpr[in->rA] = a;
#endif
        break;
    case PPC_OP_EXTSB: cpu->gpr[in->rA] = (u32)(s32)(s8)cpu->gpr[in->rS]; break;
    case PPC_OP_EXTSH: cpu->gpr[in->rA] = (u32)(s32)(s16)cpu->gpr[in->rS]; break;
    case PPC_OP_SLW:
        b = cpu->gpr[in->rB] & 0x3Fu;
        cpu->gpr[in->rA] = b & 0x20u ? 0u : cpu->gpr[in->rS] << b; break;
    case PPC_OP_SRW:
        b = cpu->gpr[in->rB] & 0x3Fu;
        cpu->gpr[in->rA] = b & 0x20u ? 0u : cpu->gpr[in->rS] >> b; break;
    case PPC_OP_SRAW:
    case PPC_OP_SRAWI: {
        b = in->op == PPC_OP_SRAWI ? in->sh : (cpu->gpr[in->rB] & 0x3Fu);
        s32 value = (s32)cpu->gpr[in->rS];
        if (b & 0x20u) {
            cpu->gpr[in->rA] = value < 0 ? 0xFFFFFFFFu : 0u;
            set_ca(cpu, value < 0);
        } else {
            cpu->gpr[in->rA] = (u32)(value >> b);
            u32 lost = b ? cpu->gpr[in->rS] & ((1u << b) - 1u) : 0u;
            set_ca(cpu, value < 0 && lost != 0);
        }
        break;
    }
    case PPC_OP_RLWINM:
        cpu->gpr[in->rA] = rotl32(cpu->gpr[in->rS], in->sh) & mask32(in->mb, in->me); break;
    case PPC_OP_RLWNM:
        cpu->gpr[in->rA] = rotl32(cpu->gpr[in->rS], cpu->gpr[in->rB]) & mask32(in->mb, in->me); break;
    case PPC_OP_RLWIMI: {
        u32 mask = mask32(in->mb, in->me);
        cpu->gpr[in->rA] = (cpu->gpr[in->rA] & ~mask) |
                           (rotl32(cpu->gpr[in->rS], in->sh) & mask);
        break;
    }

    case PPC_OP_LWZ: load_integer(cpu, in, 32, 0, 0, 0); break;
    case PPC_OP_LWZU: load_integer(cpu, in, 32, 0, 0, 1); break;
    case PPC_OP_LBZ: load_integer(cpu, in, 8, 0, 0, 0); break;
    case PPC_OP_LBZU: load_integer(cpu, in, 8, 0, 0, 1); break;
    case PPC_OP_LHZ: load_integer(cpu, in, 16, 0, 0, 0); break;
    case PPC_OP_LHZU: load_integer(cpu, in, 16, 0, 0, 1); break;
    case PPC_OP_LHA: load_integer(cpu, in, 16, 1, 0, 0); break;
    case PPC_OP_LHAU: load_integer(cpu, in, 16, 1, 0, 1); break;
    case PPC_OP_LWZX: load_integer(cpu, in, 32, 0, 1, 0); break;
    case PPC_OP_LWZUX: load_integer(cpu, in, 32, 0, 1, 1); break;
    case PPC_OP_LBZX: load_integer(cpu, in, 8, 0, 1, 0); break;
    case PPC_OP_LBZUX: load_integer(cpu, in, 8, 0, 1, 1); break;
    case PPC_OP_LHZX: load_integer(cpu, in, 16, 0, 1, 0); break;
    case PPC_OP_LHZUX: load_integer(cpu, in, 16, 0, 1, 1); break;
    case PPC_OP_LHAX: load_integer(cpu, in, 16, 1, 1, 0); break;
    case PPC_OP_LHAUX: load_integer(cpu, in, 16, 1, 1, 1); break;
    case PPC_OP_STW: store_integer(cpu, in, 32, 0, 0); break;
    case PPC_OP_STWU: store_integer(cpu, in, 32, 0, 1); break;
    case PPC_OP_STB: store_integer(cpu, in, 8, 0, 0); break;
    case PPC_OP_STBU: store_integer(cpu, in, 8, 0, 1); break;
    case PPC_OP_STH: store_integer(cpu, in, 16, 0, 0); break;
    case PPC_OP_STHU: store_integer(cpu, in, 16, 0, 1); break;
    case PPC_OP_STWX: store_integer(cpu, in, 32, 1, 0); break;
    case PPC_OP_STWUX: store_integer(cpu, in, 32, 1, 1); break;
    case PPC_OP_STBX: store_integer(cpu, in, 8, 1, 0); break;
    case PPC_OP_STBUX: store_integer(cpu, in, 8, 1, 1); break;
    case PPC_OP_STHX: store_integer(cpu, in, 16, 1, 0); break;
    case PPC_OP_STHUX: store_integer(cpu, in, 16, 1, 1); break;
    case PPC_OP_LWBRX:
        cpu->gpr[in->rD] = __builtin_bswap32(mem_read32(cpu, xform_ea(cpu, in, 0), in->address)); break;
    case PPC_OP_LHBRX:
        cpu->gpr[in->rD] = __builtin_bswap16(mem_read16(cpu, xform_ea(cpu, in, 0), in->address)); break;
    case PPC_OP_STWBRX:
        mem_write32(cpu, xform_ea(cpu, in, 0), __builtin_bswap32(cpu->gpr[in->rS]), in->address); break;
    case PPC_OP_STHBRX:
        mem_write16(cpu, xform_ea(cpu, in, 0), __builtin_bswap16((u16)cpu->gpr[in->rS]), in->address); break;

    case PPC_OP_LMW:
        ea = dform_ea(cpu, in, 0);
        for (u32 reg = in->rD; reg < 32; reg++, ea += 4)
            cpu->gpr[reg] = mem_read32(cpu, ea, in->address);
        break;
    case PPC_OP_STMW:
        ea = dform_ea(cpu, in, 0);
        for (u32 reg = in->rS; reg < 32; reg++, ea += 4)
            mem_write32(cpu, ea, cpu->gpr[reg], in->address);
        break;
    case PPC_OP_LSWI: case PPC_OP_LSWX: {
        u32 count = in->op == PPC_OP_LSWI ? (in->nb ? in->nb : 32u) : (cpu->xer & XER_BC);
        ea = in->op == PPC_OP_LSWI ? (in->rA ? cpu->gpr[in->rA] : 0u) : xform_ea(cpu, in, 0);
        for (u32 n = 0; n < count; n++) {
            u32 reg = (in->rD + n / 4u) & 31u;
            if ((n & 3u) == 0) cpu->gpr[reg] = 0;
            cpu->gpr[reg] |= (u32)mem_read8(cpu, ea + n, in->address) << (24u - 8u * (n & 3u));
        }
        break;
    }
    case PPC_OP_STSWI: case PPC_OP_STSWX: {
        u32 count = in->op == PPC_OP_STSWI ? (in->nb ? in->nb : 32u) : (cpu->xer & XER_BC);
        ea = in->op == PPC_OP_STSWI ? (in->rA ? cpu->gpr[in->rA] : 0u) : xform_ea(cpu, in, 0);
        for (u32 n = 0; n < count; n++) {
            u32 reg = (in->rS + n / 4u) & 31u;
            mem_write8(cpu, ea + n, (u8)(cpu->gpr[reg] >> (24u - 8u * (n & 3u))), in->address);
        }
        break;
    }
    case PPC_OP_LWARX:
        ea = xform_ea(cpu, in, 0); cpu->gpr[in->rD] = mem_read32(cpu, ea, in->address);
        cpu->reserve_addr = ea; cpu->reserve_valid = true; break;
    case PPC_OP_STWCX: {
        ea = xform_ea(cpu, in, 0); int success = cpu->reserve_valid;
        cpu->reserve_valid = false;
        if (success) mem_write32(cpu, ea, cpu->gpr[in->rS], in->address);
        cpu->cr = (cpu->cr & 0x0FFFFFFFu) | ((success ? 2u : 0u) << 28) |
                  ((cpu->xer >> 3) & 0x10000000u);
        break;
    }

    case PPC_OP_DCBZ:
        ppc_dcbz(cpu, xform_ea(cpu, in, 0), in->address);
        break;
    case PPC_OP_DCBZ_L:
        ppc_dcbz_l(cpu, xform_ea(cpu, in, 0), in->address); break;
    case PPC_OP_ICBI:
        ppc_icbi(cpu, xform_ea(cpu, in, 0));
        break;
    case PPC_OP_DCBST: case PPC_OP_DCBF: case PPC_OP_DCBTST: case PPC_OP_DCBT:
    case PPC_OP_DCBI:
        break;

    case PPC_OP_B:
        if (in->lk) cpu->lr = in->address + 4u;
        cpu->pc = in->branch_target; return 2;
    case PPC_OP_BC:
        if (in->lk) cpu->lr = in->address + 4u;
        if (branch_condition(cpu, in->bo, in->bi)) {
            cpu->pc = in->branch_target; return 2;
        }
        break;
    case PPC_OP_BCLR: {
        u32 target = cpu->lr & ~3u;
        if (in->lk) cpu->lr = in->address + 4u;
        if (branch_condition(cpu, in->bo, in->bi)) {
            cpu->pc = target; return 2;
        }
        break;
    }
    case PPC_OP_BCCTR: {
        u32 target = cpu->ctr & ~3u;
        if (in->lk) cpu->lr = in->address + 4u;
        if (branch_condition(cpu, in->bo, in->bi)) {
            cpu->pc = target; return 2;
        }
        break;
    }
    case PPC_OP_TWI:
        if (ppc_trap_condition(in->to, cpu->gpr[in->rA], (u32)(s32)in->simm))
            ppc_program_exception(cpu, PPC_PROGRAM_TRAP, in->address);
        break;
    case PPC_OP_TW:
        if (ppc_trap_condition(in->to, cpu->gpr[in->rA], cpu->gpr[in->rB]))
            ppc_program_exception(cpu, PPC_PROGRAM_TRAP, in->address);
        break;
    case PPC_OP_SC: ppc_system_call_exception(cpu, in->address); return 2;
    case PPC_OP_RFI: ppc_rfi(cpu, in->address); return 2;

    case PPC_OP_CRAND: case PPC_OP_CRANDC: case PPC_OP_CREQV:
    case PPC_OP_CRNAND: case PPC_OP_CRNOR: case PPC_OP_CROR:
    case PPC_OP_CRORC: case PPC_OP_CRXOR: {
        u32 abit = (cpu->cr >> (31u - in->rA)) & 1u;
        u32 bbit = (cpu->cr >> (31u - in->rB)) & 1u;
        u32 result = 0;
        switch (in->op) {
        case PPC_OP_CRAND: result = abit & bbit; break;
        case PPC_OP_CRANDC: result = abit & ~bbit; break;
        case PPC_OP_CREQV: result = ~(abit ^ bbit); break;
        case PPC_OP_CRNAND: result = ~(abit & bbit); break;
        case PPC_OP_CRNOR: result = ~(abit | bbit); break;
        case PPC_OP_CROR: result = abit | bbit; break;
        case PPC_OP_CRORC: result = abit | ~bbit; break;
        default: result = abit ^ bbit; break;
        }
        u32 bit = 0x80000000u >> in->rD;
        cpu->cr = (cpu->cr & ~bit) | ((result & 1u) ? bit : 0u);
        break;
    }
    case PPC_OP_MCRF:
        set_cr_field(cpu, in->crfD, (cpu->cr >> (4u * (7u - in->crfS))) & 0xFu); break;
    case PPC_OP_MCRXR:
        set_cr_field(cpu, in->crfD, (cpu->xer >> 28) & 0xFu);
        cpu->xer &= ~0xE0000000u; break;
    case PPC_OP_MFCR: cpu->gpr[in->rD] = cpu->cr; break;
    case PPC_OP_MTCRF: {
        u32 mask = 0;
        for (u32 field = 0; field < 8; field++)
            if (in->crm & (0x80u >> field)) mask |= 0xFu << (4u * (7u - field));
        cpu->cr = (cpu->cr & ~mask) | (cpu->gpr[in->rS] & mask); break;
    }
    case PPC_OP_MFMSR: cpu->gpr[in->rD] = cpu->msr; break;
    case PPC_OP_MTMSR: cpu->msr = cpu->gpr[in->rS]; break;
    case PPC_OP_MFSR: cpu->gpr[in->rD] = cpu->sr[in->sr]; break;
    case PPC_OP_MFSRIN: cpu->gpr[in->rD] = cpu->sr[(cpu->gpr[in->rB] >> 28) & 15u]; break;
    case PPC_OP_MTSR: cpu->sr[in->sr] = cpu->gpr[in->rS]; break;
    case PPC_OP_MTSRIN: cpu->sr[(cpu->gpr[in->rB] >> 28) & 15u] = cpu->gpr[in->rS]; break;
    case PPC_OP_MFTB: cpu->gpr[in->rD] = ppc_mftb(cpu, in->spr, in->address); break;
    case PPC_OP_MFSPR: cpu->gpr[in->rD] = ppc_mfspr(cpu, in->spr, in->address); break;
    case PPC_OP_MTSPR: ppc_mtspr(cpu, in->spr, cpu->gpr[in->rS], in->address); break;
    case PPC_OP_TLBIE: ppc_tlbie(cpu, cpu->gpr[in->rB], in->address); break;
    case PPC_OP_SYNC: case PPC_OP_EIEIO: case PPC_OP_ISYNC: case PPC_OP_TLBSYNC:
        ppc_memory_fence(); break;
    case PPC_OP_ECIWX:
        cpu->gpr[in->rD] = ppc_eciwx(cpu, xform_ea(cpu, in, 0), in->address); break;
    case PPC_OP_ECOWX:
        ppc_ecowx(cpu, xform_ea(cpu, in, 0), cpu->gpr[in->rS], in->address); break;

    default:
        return 0;
    }

    if (in->oe)
        ppc_set_xer_ov(cpu, overflow != 0);
    return 1;
}

static int execute_float(CPUState* cpu, const PPCInst* in) {
    f64 a, b, c, x, y;
    u64 converted;
    u32 ea;
    switch (in->op) {
    case PPC_OP_LFS: load_float(cpu, in, 1, 0, 0); break;
    case PPC_OP_LFSU: load_float(cpu, in, 1, 0, 1); break;
    case PPC_OP_LFD: load_float(cpu, in, 0, 0, 0); break;
    case PPC_OP_LFDU: load_float(cpu, in, 0, 0, 1); break;
    case PPC_OP_LFSX: load_float(cpu, in, 1, 1, 0); break;
    case PPC_OP_LFSUX: load_float(cpu, in, 1, 1, 1); break;
    case PPC_OP_LFDX: load_float(cpu, in, 0, 1, 0); break;
    case PPC_OP_LFDUX: load_float(cpu, in, 0, 1, 1); break;
    case PPC_OP_STFS: store_float(cpu, in, 1, 0, 0); break;
    case PPC_OP_STFSU: store_float(cpu, in, 1, 0, 1); break;
    case PPC_OP_STFD: store_float(cpu, in, 0, 0, 0); break;
    case PPC_OP_STFDU: store_float(cpu, in, 0, 0, 1); break;
    case PPC_OP_STFSX: store_float(cpu, in, 1, 1, 0); break;
    case PPC_OP_STFSUX: store_float(cpu, in, 1, 1, 1); break;
    case PPC_OP_STFDX: store_float(cpu, in, 0, 1, 0); break;
    case PPC_OP_STFDUX: store_float(cpu, in, 0, 1, 1); break;
    case PPC_OP_STFIWX:
        ea = xform_ea(cpu, in, 0);
        mem_write32(cpu, ea, (u32)f64_bits(cpu->fpr[in->rS]), in->address); break;

    case PPC_OP_PSQ_L: case PPC_OP_PSQ_LU: case PPC_OP_PSQ_LX: case PPC_OP_PSQ_LUX:
        ea = (in->op == PPC_OP_PSQ_LX || in->op == PPC_OP_PSQ_LUX) ?
             xform_ea(cpu, in, in->op == PPC_OP_PSQ_LUX) :
             dform_ea(cpu, in, in->op == PPC_OP_PSQ_LU);
        ppc_psq_load(cpu, in->rD, ea, in->w, in->i,
                     in->op == PPC_OP_PSQ_LX || in->op == PPC_OP_PSQ_LUX,
                     in->address);
        if (in->op == PPC_OP_PSQ_LU || in->op == PPC_OP_PSQ_LUX) cpu->gpr[in->rA] = ea;
        break;
    case PPC_OP_PSQ_ST: case PPC_OP_PSQ_STU: case PPC_OP_PSQ_STX: case PPC_OP_PSQ_STUX:
        ea = (in->op == PPC_OP_PSQ_STX || in->op == PPC_OP_PSQ_STUX) ?
             xform_ea(cpu, in, in->op == PPC_OP_PSQ_STUX) :
             dform_ea(cpu, in, in->op == PPC_OP_PSQ_STU);
        ppc_psq_store(cpu, in->rS, ea, in->w, in->i,
                      in->op == PPC_OP_PSQ_STX || in->op == PPC_OP_PSQ_STUX,
                      in->address);
        if (in->op == PPC_OP_PSQ_STU || in->op == PPC_OP_PSQ_STUX) cpu->gpr[in->rA] = ea;
        break;

    case PPC_OP_FADDS: cpu->fpr[in->rD] = (f64)(f32)(cpu->fpr[in->rA] + cpu->fpr[in->rB]); break;
    case PPC_OP_FSUBS: cpu->fpr[in->rD] = (f64)(f32)(cpu->fpr[in->rA] - cpu->fpr[in->rB]); break;
    case PPC_OP_FMULS: cpu->fpr[in->rD] = (f64)(f32)(cpu->fpr[in->rA] * cpu->fpr[in->rC]); break;
    case PPC_OP_FDIVS: cpu->fpr[in->rD] = (f64)(f32)(cpu->fpr[in->rA] / cpu->fpr[in->rB]); break;
    case PPC_OP_FADD: cpu->fpr[in->rD] = cpu->fpr[in->rA] + cpu->fpr[in->rB]; break;
    case PPC_OP_FSUB: cpu->fpr[in->rD] = cpu->fpr[in->rA] - cpu->fpr[in->rB]; break;
    case PPC_OP_FMUL: cpu->fpr[in->rD] = cpu->fpr[in->rA] * cpu->fpr[in->rC]; break;
    case PPC_OP_FDIV: cpu->fpr[in->rD] = cpu->fpr[in->rA] / cpu->fpr[in->rB]; break;
    case PPC_OP_FRES:
        if (!ppc_fres(cpu, cpu->fpr[in->rB], &x)) return 1;
        cpu->fpr[in->rD] = x; break;
    case PPC_OP_FRSQRTE:
        if (!ppc_frsqrte(cpu, cpu->fpr[in->rB], &x)) return 1;
        cpu->fpr[in->rD] = x; break;
    case PPC_OP_FMADDS: case PPC_OP_FMSUBS: case PPC_OP_FNMADDS: case PPC_OP_FNMSUBS:
        if (ppc_fma(cpu, cpu->fpr[in->rA], cpu->fpr[in->rC], cpu->fpr[in->rB],
                    true, in->op == PPC_OP_FMSUBS || in->op == PPC_OP_FNMSUBS,
                    in->op == PPC_OP_FNMADDS || in->op == PPC_OP_FNMSUBS, &x))
            cpu->fpr[in->rD] = x;
        break;
    case PPC_OP_FMADD: case PPC_OP_FMSUB: case PPC_OP_FNMADD: case PPC_OP_FNMSUB:
        if (ppc_fma(cpu, cpu->fpr[in->rA], cpu->fpr[in->rC], cpu->fpr[in->rB],
                    false, in->op == PPC_OP_FMSUB || in->op == PPC_OP_FNMSUB,
                    in->op == PPC_OP_FNMADD || in->op == PPC_OP_FNMSUB, &x))
            cpu->fpr[in->rD] = x;
        break;
    case PPC_OP_FCTIW: case PPC_OP_FCTIWZ:
        if (ppc_fctiw(cpu, cpu->fpr[in->rB], in->op == PPC_OP_FCTIWZ, &converted))
            cpu->fpr[in->rD] = bits_f64(converted);
        break;
    case PPC_OP_FMR: cpu->fpr[in->rD] = cpu->fpr[in->rB]; break;
    case PPC_OP_FNEG: cpu->fpr[in->rD] = -cpu->fpr[in->rB]; break;
    case PPC_OP_FABS: cpu->fpr[in->rD] = fabs(cpu->fpr[in->rB]); break;
    case PPC_OP_FNABS: cpu->fpr[in->rD] = -fabs(cpu->fpr[in->rB]); break;
    case PPC_OP_FRSP: cpu->fpr[in->rD] = (f64)(f32)cpu->fpr[in->rB]; break;
    case PPC_OP_FSEL:
        cpu->fpr[in->rD] = cpu->fpr[in->rA] >= 0.0 ? cpu->fpr[in->rC] : cpu->fpr[in->rB]; break;
    case PPC_OP_FCMPU: case PPC_OP_FCMPO:
        compare_f64(cpu, in->crfD, cpu->fpr[in->rA], cpu->fpr[in->rB]); break;
    case PPC_OP_MTFSB0: cpu->fpscr &= ~(0x80000000u >> in->rD); ppc_fpscr_updated(cpu); break;
    case PPC_OP_MTFSB1: cpu->fpscr |=  (0x80000000u >> in->rD); ppc_fpscr_updated(cpu); break;
    case PPC_OP_MCRFS: {
        u32 shift = 4u * (7u - in->crfS);
        set_cr_field(cpu, in->crfD, (cpu->fpscr >> shift) & 0xFu);
        cpu->fpscr &= ~(0xFu << shift); ppc_fpscr_updated(cpu); break;
    }
    case PPC_OP_MFFS: cpu->fpr[in->rD] = bits_f64((u64)cpu->fpscr); break;
    case PPC_OP_MTFSFI: {
        u32 shift = 4u * (7u - in->crfD);
        cpu->fpscr = (cpu->fpscr & ~(0xFu << shift)) | ((u32)in->imm << shift);
        ppc_fpscr_updated(cpu); break;
    }
    case PPC_OP_MTFSF: {
        u32 source = (u32)f64_bits(cpu->fpr[in->rB]);
        u32 mask = 0;
        for (u32 field = 0; field < 8; field++)
            if (in->fm & (0x80u >> field)) mask |= 0xFu << (4u * (7u - field));
        cpu->fpscr = (cpu->fpscr & ~mask) | (source & mask); ppc_fpscr_updated(cpu); break;
    }

    case PPC_OP_PS_ADD:
        cpu->fpr[in->rD] = (f64)(f32)(cpu->fpr[in->rA] + cpu->fpr[in->rB]);
        cpu->ps1[in->rD] = (f64)(f32)(cpu->ps1[in->rA] + cpu->ps1[in->rB]); break;
    case PPC_OP_PS_SUB:
        cpu->fpr[in->rD] = (f64)(f32)(cpu->fpr[in->rA] - cpu->fpr[in->rB]);
        cpu->ps1[in->rD] = (f64)(f32)(cpu->ps1[in->rA] - cpu->ps1[in->rB]); break;
    case PPC_OP_PS_MUL:
        cpu->fpr[in->rD] = (f64)(f32)(cpu->fpr[in->rA] * cpu->fpr[in->rC]);
        cpu->ps1[in->rD] = (f64)(f32)(cpu->ps1[in->rA] * cpu->ps1[in->rC]); break;
    case PPC_OP_PS_DIV:
        cpu->fpr[in->rD] = (f64)(f32)(cpu->fpr[in->rA] / cpu->fpr[in->rB]);
        cpu->ps1[in->rD] = (f64)(f32)(cpu->ps1[in->rA] / cpu->ps1[in->rB]); break;
    case PPC_OP_PS_RES:
        ppc_ps_res(cpu, cpu->fpr[in->rB], cpu->ps1[in->rB], &x, &y);
        cpu->fpr[in->rD] = x; cpu->ps1[in->rD] = y; break;
    case PPC_OP_PS_RSQRTE:
        ppc_ps_rsqrte(cpu, cpu->fpr[in->rB], cpu->ps1[in->rB], &x, &y);
        cpu->fpr[in->rD] = x; cpu->ps1[in->rD] = y; break;
    case PPC_OP_PS_MADD: case PPC_OP_PS_MSUB: case PPC_OP_PS_NMADD: case PPC_OP_PS_NMSUB: {
        int sub = in->op == PPC_OP_PS_MSUB || in->op == PPC_OP_PS_NMSUB;
        int neg = in->op == PPC_OP_PS_NMADD || in->op == PPC_OP_PS_NMSUB;
        ppc_fma(cpu, cpu->fpr[in->rA], cpu->fpr[in->rC], cpu->fpr[in->rB], true, sub, neg, &x);
        ppc_fma(cpu, cpu->ps1[in->rA], cpu->ps1[in->rC], cpu->ps1[in->rB], true, sub, neg, &y);
        cpu->fpr[in->rD] = x; cpu->ps1[in->rD] = y; break;
    }
    case PPC_OP_PS_NEG:
        cpu->fpr[in->rD] = -cpu->fpr[in->rB]; cpu->ps1[in->rD] = -cpu->ps1[in->rB]; break;
    case PPC_OP_PS_ABS:
        cpu->fpr[in->rD] = fabs(cpu->fpr[in->rB]); cpu->ps1[in->rD] = fabs(cpu->ps1[in->rB]); break;
    case PPC_OP_PS_NABS:
        cpu->fpr[in->rD] = -fabs(cpu->fpr[in->rB]); cpu->ps1[in->rD] = -fabs(cpu->ps1[in->rB]); break;
    case PPC_OP_PS_MR:
        cpu->fpr[in->rD] = cpu->fpr[in->rB]; cpu->ps1[in->rD] = cpu->ps1[in->rB]; break;
    case PPC_OP_PS_SUM0:
        cpu->fpr[in->rD] = (f64)(f32)(cpu->fpr[in->rA] + cpu->ps1[in->rB]);
        cpu->ps1[in->rD] = cpu->ps1[in->rC]; break;
    case PPC_OP_PS_SUM1:
        cpu->fpr[in->rD] = cpu->fpr[in->rC];
        cpu->ps1[in->rD] = (f64)(f32)(cpu->ps1[in->rA] + cpu->fpr[in->rB]); break;
    case PPC_OP_PS_MULS0:
        cpu->fpr[in->rD] = (f64)(f32)(cpu->fpr[in->rA] * cpu->fpr[in->rC]);
        cpu->ps1[in->rD] = (f64)(f32)(cpu->ps1[in->rA] * cpu->fpr[in->rC]); break;
    case PPC_OP_PS_MULS1:
        cpu->fpr[in->rD] = (f64)(f32)(cpu->fpr[in->rA] * cpu->ps1[in->rC]);
        cpu->ps1[in->rD] = (f64)(f32)(cpu->ps1[in->rA] * cpu->ps1[in->rC]); break;
    case PPC_OP_PS_MADDS0:
        ppc_fma(cpu, cpu->fpr[in->rA], cpu->fpr[in->rC], cpu->fpr[in->rB], true, false, false, &x);
        ppc_fma(cpu, cpu->ps1[in->rA], cpu->fpr[in->rC], cpu->ps1[in->rB], true, false, false, &y);
        cpu->fpr[in->rD] = x; cpu->ps1[in->rD] = y; break;
    case PPC_OP_PS_MADDS1:
        ppc_fma(cpu, cpu->fpr[in->rA], cpu->ps1[in->rC], cpu->fpr[in->rB], true, false, false, &x);
        ppc_fma(cpu, cpu->ps1[in->rA], cpu->ps1[in->rC], cpu->ps1[in->rB], true, false, false, &y);
        cpu->fpr[in->rD] = x; cpu->ps1[in->rD] = y; break;
    case PPC_OP_PS_MERGE00:
        a = cpu->fpr[in->rA]; b = cpu->fpr[in->rB]; cpu->fpr[in->rD] = a; cpu->ps1[in->rD] = b; break;
    case PPC_OP_PS_MERGE01:
        a = cpu->fpr[in->rA]; b = cpu->ps1[in->rB]; cpu->fpr[in->rD] = a; cpu->ps1[in->rD] = b; break;
    case PPC_OP_PS_MERGE10:
        a = cpu->ps1[in->rA]; b = cpu->fpr[in->rB]; cpu->fpr[in->rD] = a; cpu->ps1[in->rD] = b; break;
    case PPC_OP_PS_MERGE11:
        a = cpu->ps1[in->rA]; b = cpu->ps1[in->rB]; cpu->fpr[in->rD] = a; cpu->ps1[in->rD] = b; break;
    case PPC_OP_PS_CMPU0: case PPC_OP_PS_CMPO0:
        compare_f64(cpu, in->crfD, cpu->fpr[in->rA], cpu->fpr[in->rB]); break;
    case PPC_OP_PS_CMPU1: case PPC_OP_PS_CMPO1:
        compare_f64(cpu, in->crfD, cpu->ps1[in->rA], cpu->ps1[in->rB]); break;
    case PPC_OP_PS_SEL:
        cpu->fpr[in->rD] = (f32)cpu->fpr[in->rA] >= 0.0f ? cpu->fpr[in->rC] : cpu->fpr[in->rB];
        cpu->ps1[in->rD] = (f32)cpu->ps1[in->rA] >= 0.0f ? cpu->ps1[in->rC] : cpu->ps1[in->rB];
        break;
    default:
        return 0;
    }
    return 1;
}

static u32 integer_record_value(const CPUState* cpu, const PPCInst* in) {
    switch (in->op) {
    case PPC_OP_ORI: case PPC_OP_ORIS:
    case PPC_OP_XORI: case PPC_OP_XORIS:
    case PPC_OP_ANDI: case PPC_OP_ANDIS:
    case PPC_OP_AND: case PPC_OP_ANDC:
    case PPC_OP_OR: case PPC_OP_ORC:
    case PPC_OP_XOR: case PPC_OP_NAND:
    case PPC_OP_NOR: case PPC_OP_EQV:
    case PPC_OP_CNTLZW: case PPC_OP_EXTSB: case PPC_OP_EXTSH:
    case PPC_OP_SLW: case PPC_OP_SRW:
    case PPC_OP_SRAW: case PPC_OP_SRAWI:
    case PPC_OP_RLWINM: case PPC_OP_RLWNM: case PPC_OP_RLWIMI:
        return cpu->gpr[in->rA];
    default:
        return cpu->gpr[in->rD];
    }
}

/* --- interpreter-side batching of counted cache-maintenance loops ---
 *
 * BS1's post-DMA I/D-cache flush is the classic 3-instruction PowerPC idiom:
 *     loop:  <cache-op> 0,rX     ; icbi/dcbi/dcbf/dcbst/dcbtst/dcbt
 *            addi       rX,rX,32 ; advance one cache line
 *            bdnz       loop     ; BO=0x10 (ignore CR), branch back -8
 * Content validation correctly routes this to the interpreter (the AOT
 * snapshot has stale bytes at a DYNAMICALLY-WRITTEN address), but that means
 * paying dispatch.c's full per-block round trip (device ticks, ring log,
 * debug-server pump gate, ...) three times per iteration -- 622,592 times
 * for BS1's icbi cluster alone (ctr=0x8000, 19 invocations), and 121,487
 * times for its dcbi cluster. This recognizer closes the whole loop (or as
 * much of it as the cycle quantum allows) in one interpreter call: identical
 * final GPR/CTR, identical cycle charge (per-iteration cost taken straight
 * from dr_ppc_num_cycles(), summed exactly -- never re-derived or estimated),
 * and identical cache-invalidation side effects. gcn_native_code_invalidate
 * and gcn_title_module_icbi already expose a RANGE api that is a monotonic
 * set-union over pages/chunks (native_code.c's bitmap only counts a page
 * once no matter how many overlapping calls set it; aot_module.c's
 * gcn_aot_module_invalidate unions overlapping chunks the same way) -- so
 * one call over [start, start+32*K) invalidates exactly the set N per-line
 * calls would have.
 *
 * Recognized ONLY on the DECODED INSTRUCTION SHAPE (cache-op class + a
 * self-incrementing addi-by-32 on the cache-op's index register + a
 * bdnz-always branch back to the cache-op), never on PC -- any future title
 * that hits the same idiom at a different address is covered automatically.
 *
 * Exactness / safety gates (fall through to the ordinary one-instruction
 * path -- byte-identical to before this existed -- the instant any of these
 * doesn't hold):
 *   - gcn_interpreter_set_cache_loop_batch_enabled(false): dispatch.c calls
 *     this with GCN_COSIM's enabled state each block (cosim grants exactly
 *     one retired instruction per budget and would desync if a whole loop
 *     retired at once). interpreter.c has no debug_server.c dependency of
 *     its own -- gcn_runtime_core stays link-complete without the TCP/device
 *     sources gcn_boot adds -- so the caller pushes the decision down instead
 *     of this file pulling debug_server.c in;
 *   - never while MSR[EE] is set: both external (pi.c) and decrementer
 *     (cpu_glue.c) delivery gate on MSR[EE], so with EE clear neither check
 *     could have fired at ANY of the per-block boundaries this batch skips,
 *     batched or not -- there is nothing to miss;
 *   - never past cpu->cycle_deadline: the same quantum discipline as the
 *     generated-code taken-backward-branch peephole (dispatch.c's deadline
 *     yield). A real deadline expiry still yields back to dispatch.c's
 *     per-block device tick / interrupt check exactly where the unbatched
 *     path would have -- we just don't pay 3*(iterations before the
 *     deadline) separate round trips to discover that.
 */
static bool s_cache_loop_batch_enabled = true;

/* dispatch.c calls this once per block with !gcn_debug_server_cosim_enabled()
 * (production); tests call it directly to run the same synthetic loop both
 * batched and unbatched and diff the resulting GPR/CTR/cycles/invalidation
 * state for exactness. Defaults to enabled. */
void gcn_interpreter_set_cache_loop_batch_enabled(bool enabled) {
    s_cache_loop_batch_enabled = enabled;
}

static bool try_batch_cache_loop(CPUState* cpu, const PPCInst* in, u32 cia) {
    if (!s_cache_loop_batch_enabled)
        return false;
    switch (in->op) {
    case PPC_OP_ICBI: case PPC_OP_DCBI: case PPC_OP_DCBF:
    case PPC_OP_DCBST: case PPC_OP_DCBTST: case PPC_OP_DCBT:
        break;
    default:
        return false;
    }
    if (cpu->exception)
        return false;
    if (cpu->msr & 0x00008000u)          /* MSR[EE] (PPC bit 16) */
        return false;

    const u32 raw2 = mem_read32(cpu, cia + 4u, cia + 4u);
    PPCInst in2 = ppc_decode(raw2, cia + 4u);
    if (in2.op != PPC_OP_ADDI)
        return false;
    if (in2.rD != in2.rA || in2.rD != in->rB)   /* addi rX,rX,32; rX == cache-op's index reg */
        return false;
    if ((s32)in2.simm != 32)
        return false;
    if (in->rA != 0u && in->rA == in->rB)       /* base register must stay constant */
        return false;

    const u32 raw3 = mem_read32(cpu, cia + 8u, cia + 8u);
    PPCInst in3 = ppc_decode(raw3, cia + 8u);
    if (in3.op != PPC_OP_BC)
        return false;
    if (in3.bo != 0x10u || in3.lk)              /* bdnz, ignore CR, always, no link */
        return false;
    if (in3.branch_target != cia)               /* must close back onto the cache-op */
        return false;

    const u32 idx_reg = in->rB;
    const u32 ctr = cpu->ctr;
    const u64 iters_to_exit = ctr == 0u ? 0x100000000ULL : (u64)ctr;

    const u64 per_iter = (u64)dr_ppc_num_cycles(in->op) +
                          (u64)dr_ppc_num_cycles(PPC_OP_ADDI) +
                          (u64)dr_ppc_num_cycles(PPC_OP_BC);
    const u64 budget_cycles = cpu->cycle_deadline > cpu->cycles ?
                               cpu->cycle_deadline - cpu->cycles : 0u;
    const u64 budget_iters = budget_cycles / per_iter;
    const u64 k = iters_to_exit < budget_iters ? iters_to_exit : budget_iters;
    if (k == 0u)
        return false;   /* not even one full iteration fits the remaining
                          * quantum -- let the ordinary single-instruction
                          * path run this one, identical to unbatched. */

    if (in->op == PPC_OP_ICBI) {
        const u32 base_ea = in->rA ? cpu->gpr[in->rA] : 0u;
        const u32 start_addr = base_ea + cpu->gpr[idx_reg];
        const u32 range = (u32)(k * 32ull);
        gcn_native_code_invalidate(start_addr & ~31u, range);
        gcn_title_module_icbi(start_addr & ~31u, range);
        gcn_overlay_icbi(start_addr & ~31u, range);
    }
    /* dcbi/dcbf/dcbst/dcbtst/dcbt are architectural hints with no modeled
     * side effect in this interpreter (see the no-op case in
     * execute_integer's ICBI/DCB* switch) -- nothing else to replay. */

    cpu->gpr[idx_reg] += (u32)(k * 32ull);
    cpu->ctr = (u32)(ctr - (u32)k);
    cpu->cycles += per_iter * k;
    cpu->pc = (k == iters_to_exit) ? cia + 12u /* branch falls through */
                                    : cia;      /* deadline hit mid-loop */

    /* One retired *step*, not 3*k instructions: the batched iterations never
     * call gcn_interpreter_step, so this total legitimately reads lower than
     * an unbatched run over the same guest execution -- expected, not a bug
     * (see the M2 census comparison in the task notes). */
    s_instruction_count++;
    note_edge(cia, cpu->pc, cpu->cycles);
    return true;
}

int gcn_interpreter_step(CPUState* cpu) {
    const u32 cia = cpu->pc;
    const u32 raw = mem_read32(cpu, cia, cia);
    PPCInst in = ppc_decode(raw, cia);
    if (in.op == PPC_OP_UNKNOWN) {
        fprintf(stderr, "[interpreter] UNSUPPORTED pc=%08X raw=%08X cycles=%llu\n",
                cia, raw, (unsigned long long)cpu->cycles);
        cpu->halted = true;
        cpu->halt_reason = GCN_HALT_INTERPRETER_UNSUPPORTED;
        return 0;
    }
    if (try_batch_cache_loop(cpu, &in, cia))
        return 1;
    if (is_fp_op(in.op) && !(cpu->msr & MSR_FP)) {
        ppc_fp_unavailable(cpu, cia);
        cpu->cycles += dr_ppc_num_cycles(in.op);
        s_instruction_count++;
        note_edge(cia, cpu->pc, cpu->cycles);
        return 1;
    }

    int result = is_fp_op(in.op) ? execute_float(cpu, &in)
                                 : execute_integer(cpu, &in);
    if (!result) {
        fprintf(stderr,
                "[interpreter] DECODED-BUT-UNIMPLEMENTED pc=%08X raw=%08X op=%s cycles=%llu\n",
                cia, raw, ppc_op_name(in.op), (unsigned long long)cpu->cycles);
        cpu->halted = true;
        cpu->halt_reason = GCN_HALT_INTERPRETER_UNSUPPORTED;
        return 0;
    }

    cpu->cycles += dr_ppc_num_cycles(in.op);
    s_instruction_count++;
    if (result != 2 && !cpu->exception)
        cpu->pc = cia + 4u;
    if (in.rc && in.op != PPC_OP_STWCX) {
        if (is_fp_op(in.op)) set_cr1(cpu);
        else set_cr0(cpu, integer_record_value(cpu, &in));
    }
    note_edge(cia, cpu->pc, cpu->cycles);
    return 1;
}

void gcn_interpreter_shutdown(void) {
    if (s_journal) {
        fprintf(s_journal,
                "{\"version\":1,\"event\":\"summary\",\"instructions\":%llu,"
                "\"unique_misses\":%llu,\"unique_edges\":%llu}\n",
                (unsigned long long)s_instruction_count,
                (unsigned long long)s_unique_miss_count,
                (unsigned long long)s_edge_count);
        fclose(s_journal);
        s_journal = NULL;
    }
    if (s_instruction_count || s_unique_miss_count) {
        fprintf(stderr,
                "[interpreter] summary instructions=%llu unique-misses=%llu unique-edges=%llu\n",
                (unsigned long long)s_instruction_count,
                (unsigned long long)s_unique_miss_count,
                (unsigned long long)s_edge_count);
    }
}

u64 gcn_interpreter_instruction_count(void) { return s_instruction_count; }
u64 gcn_interpreter_unique_miss_count(void) { return s_unique_miss_count; }
