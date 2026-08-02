/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Always-on diagnostic ring buffers (see include/debug/rings.h). Single-writer
 * (main dispatch thread); the TCP server reads them from the same thread during
 * its pump, so no locking. Records are fixed-size structs in fixed-cap arrays;
 * a monotonic write count gives eviction (index = count & (cap-1)) and lets a
 * dump walk the live window oldest->newest.
 */
#include "debug/rings.h"

#include <stdio.h>
#include <stdlib.h>      /* getenv/strtoul — GCN_WATCH parse (gcn_ring_watch_init) */
#include <string.h>

typedef struct {
    u64 seq;
    u64 block;
    u32 pc;
    u32 addr;
    u32 value;
    u8  size;
    u8  rw;
    u8  mapped;
    u8  _pad;
} MmioEntry;

typedef struct {
    u64 seq;
    u32 pc;
    u32 _pad;
} BlockEntry;

typedef struct {
    u64 seq;
    u64 block;
    u32 pc;
    u32 detail;
    u32 aux;
    u16 kind;
    u16 _pad;
} EventEntry;

typedef struct {
    u64 seq;
    u64 block;
    u32 pc;
    u32 wptr;
    u8  data[32];
} FifoEntry;

typedef struct {
    u64 seq;
    u64 block;
    u32 pc;
    u32 address;
    u32 length;
    u32 data;
    u8  channel;
    u8  cs;
    u8  command;
    u8  rw;
    u8  dma;
    u8  _pad[3];
} MemcardEntry;

static MmioEntry    s_mmio[GCN_MMIO_RING_CAP];
static BlockEntry   s_block[GCN_BLOCK_RING_CAP];
static EventEntry   s_event[GCN_EVENT_RING_CAP];
static FifoEntry    s_fifo[GCN_FIFO_RING_CAP];
static MemcardEntry s_memcard[GCN_MEMCARD_RING_CAP];

static u64 s_mmio_count;   /* total ever recorded (also the next write index)   */
static u64 s_block_count;
static u64 s_event_count;
static u64 s_fifo_count;
static u64 s_memcard_count;
static u64 s_block_index;  /* monotonic retired-block counter (timeline stamp)  */

/* One bit per aligned word. MEM1/MEM2 code is observed through the cached
 * 0x80000000 alias in ordinary execution (24 MiB => 768 KiB); reset/BS1 code
 * uses the 2 MiB high-ROM window (64 KiB). The rolling PC ring answers "when";
 * this non-evicting bitmap answers "ever" without an arm-before-run race. */
#define GCN_RAM_EXEC_BASE       0x80000000u
#define GCN_RAM_EXEC_BYTES      0x01800000u
#define GCN_RAM_EXEC_WORDS      (GCN_RAM_EXEC_BYTES / 4u)
#define GCN_RAM_EXEC_COV_BYTES  (GCN_RAM_EXEC_WORDS / 8u)
#define GCN_IPL_EXEC_BASE       0xFFE00000u
#define GCN_IPL_EXEC_BYTES      0x00200000u
#define GCN_IPL_EXEC_WORDS      (GCN_IPL_EXEC_BYTES / 4u)
#define GCN_IPL_EXEC_COV_BYTES  (GCN_IPL_EXEC_WORDS / 8u)
static u8 s_ram_pc_coverage[GCN_RAM_EXEC_COV_BYTES];
static u8 s_ipl_pc_coverage[GCN_IPL_EXEC_COV_BYTES];

void gcn_rings_init(void) {
    memset(s_mmio, 0, sizeof s_mmio);
    memset(s_block, 0, sizeof s_block);
    memset(s_event, 0, sizeof s_event);
    memset(s_fifo, 0, sizeof s_fifo);
    memset(s_memcard, 0, sizeof s_memcard);
    memset(s_ram_pc_coverage, 0, sizeof s_ram_pc_coverage);
    memset(s_ipl_pc_coverage, 0, sizeof s_ipl_pc_coverage);
    s_mmio_count = s_block_count = s_event_count = s_fifo_count = 0;
    s_memcard_count = 0;
    s_block_index = 0;
}

void gcn_ring_mmio(u32 pc, u32 addr, u32 value, u8 size, u8 rw, u8 mapped) {
    MmioEntry* e = &s_mmio[s_mmio_count & (GCN_MMIO_RING_CAP - 1)];
    e->seq = s_mmio_count++;
    e->block = s_block_index;
    e->pc = pc; e->addr = addr; e->value = value;
    e->size = size; e->rw = rw; e->mapped = mapped; e->_pad = 0;
}

void gcn_ring_block(u32 pc) {
    BlockEntry* e = &s_block[s_block_count & (GCN_BLOCK_RING_CAP - 1)];
    e->seq = s_block_count++;
    e->pc = pc; e->_pad = 0;
    if ((pc & 3u) == 0u) {
        if (pc - GCN_RAM_EXEC_BASE < GCN_RAM_EXEC_BYTES) {
            u32 word = (pc - GCN_RAM_EXEC_BASE) >> 2;
            s_ram_pc_coverage[word >> 3] |= (u8)(1u << (word & 7u));
        } else if (pc - GCN_IPL_EXEC_BASE < GCN_IPL_EXEC_BYTES) {
            u32 word = (pc - GCN_IPL_EXEC_BASE) >> 2;
            s_ipl_pc_coverage[word >> 3] |= (u8)(1u << (word & 7u));
        }
    }
    s_block_index++;
}

void gcn_ring_event(u16 kind, u32 detail, u32 aux, u32 pc) {
    EventEntry* e = &s_event[s_event_count & (GCN_EVENT_RING_CAP - 1)];
    e->seq = s_event_count++;
    e->block = s_block_index;
    e->pc = pc; e->detail = detail; e->aux = aux;
    e->kind = kind; e->_pad = 0;
}

void gcn_ring_fifo(u32 pc, u32 wptr, const u8* data32) {
    FifoEntry* e = &s_fifo[s_fifo_count & (GCN_FIFO_RING_CAP - 1)];
    e->seq = s_fifo_count++;
    e->block = s_block_index;
    e->pc = pc; e->wptr = wptr;
    memcpy(e->data, data32, 32);
}

void gcn_ring_memcard(u32 pc, u8 channel, u8 cs, u8 command, u8 rw, u8 dma,
                      u32 address, u32 length, u32 data) {
    MemcardEntry* e = &s_memcard[s_memcard_count & (GCN_MEMCARD_RING_CAP - 1)];
    e->seq = s_memcard_count++;
    e->block = s_block_index;
    e->pc = pc; e->address = address; e->length = length; e->data = data;
    e->channel = channel; e->cs = cs; e->command = command; e->rw = rw; e->dma = dma;
}

u64 gcn_ring_block_index(void) { return s_block_index; }

int gcn_ring_pc_seen(u32 pc) {
    u32 word;
    if ((pc & 3u) != 0u) return 0;
    if (pc - GCN_RAM_EXEC_BASE < GCN_RAM_EXEC_BYTES) {
        word = (pc - GCN_RAM_EXEC_BASE) >> 2;
        return (s_ram_pc_coverage[word >> 3] >> (word & 7u)) & 1u;
    }
    if (pc - GCN_IPL_EXEC_BASE < GCN_IPL_EXEC_BYTES) {
        word = (pc - GCN_IPL_EXEC_BASE) >> 2;
        return (s_ipl_pc_coverage[word >> 3] >> (word & 7u)) & 1u;
    }
    return 0;
}

/* [gx-fifoprov] paired-single load/store value ring. The corrupt FIFO matrix
 * payloads are pushed by the IPL's psq_l(RAM)->psq_st(gather pipe) upload
 * loop with one LANE of some pairs scaled by exactly 2^12 — this ring holds
 * the bit patterns each psq instruction actually loaded/stored, so a dump at
 * the corrupt command's decode names whether the garbage already came out of
 * RAM (upstream guest computation corrupt) or appeared between the load and
 * the store (CPU state corruption — interrupt save/restore suspect). */
typedef struct {
    u64 seq;
    u32 cia;      /* guest pc of the psq instruction */
    u32 ea;       /* effective address (ps0 lane) */
    u32 ps0, ps1; /* f32 bit patterns moved (post-load / as-stored) */
    u8  is_store;
    u8  _pad[3];
} PsqEntry;
#define GCN_PSQ_RING_CAP (1u << 15)
static PsqEntry s_psq[GCN_PSQ_RING_CAP];
static u64 s_psq_count;

void gcn_ring_psq(u32 cia, u32 ea, u32 ps0, u32 ps1, u8 is_store) {
    PsqEntry* e = &s_psq[s_psq_count & (GCN_PSQ_RING_CAP - 1)];
    e->seq = s_psq_count++;
    e->cia = cia; e->ea = ea; e->ps0 = ps0; e->ps1 = ps1;
    e->is_store = is_store;
}

void gcn_ring_psq_dump_stderr(int max_entries) {
    u64 avail = s_psq_count < GCN_PSQ_RING_CAP ? s_psq_count : GCN_PSQ_RING_CAP;
    if ((u64)max_entries < avail) avail = (u64)max_entries;
    u64 start = s_psq_count - avail;
    fprintf(stderr, "[gx-fifoprov] psq ring (%llu total, showing #%llu..#%llu):\n",
            (unsigned long long)s_psq_count, (unsigned long long)start,
            (unsigned long long)(s_psq_count ? s_psq_count - 1u : 0u));
    for (u64 i = start; i < s_psq_count; i++) {
        const PsqEntry* e = &s_psq[i & (GCN_PSQ_RING_CAP - 1)];
        f32 v0, v1;
        memcpy(&v0, &e->ps0, 4);
        memcpy(&v1, &e->ps1, 4);
        fprintf(stderr, "  #%llu %s pc=%08X ea=%08X ps0=%08X(%.6g) ps1=%08X(%.6g)\n",
                (unsigned long long)e->seq, e->is_store ? "ST" : "LD",
                e->cia, e->ea, e->ps0, (double)v0, e->ps1, (double)v1);
    }
    fflush(stderr);
}

/* [gx-fifoprov] guest-RAM write watch (env-gated, zero-cost when unset).
 * GCN_WATCH=<lo_hex>:<hi_hex> (any address alias; normalized to physical)
 * makes every recompiled/glue store into [lo,hi) print pc/ea/value. Built to
 * name the routine that writes the corrupt instance matrices (the producer
 * uses stfs, which the psq ring cannot see). */
u32 gcn_watch_lo = 0, gcn_watch_len = 0;

void gcn_ring_watch_init(void) {
    const char* e = getenv("GCN_WATCH");
    if (!e) return;
    unsigned long lo = strtoul(e, NULL, 16);
    const char* colon = strchr(e, ':');
    unsigned long hi = colon ? strtoul(colon + 1, NULL, 16) : lo + 4u;
    gcn_watch_lo = (u32)lo & 0x1FFFFFFFu;
    gcn_watch_len = (u32)((hi & 0x1FFFFFFFu) - gcn_watch_lo);
    fprintf(stderr, "[gcn-watch] armed: phys %08X..%08X\n",
            gcn_watch_lo, gcn_watch_lo + gcn_watch_len);
}

/* Watch hits go into an always-on ring (dumped on demand by the corrupt-
 * upload trigger); only the first few print live as a liveness check. */
typedef struct { u64 block; u32 pc, ea; u64 value; u8 size; u8 _pad[7]; } WatchEntry;
#define GCN_WATCH_RING_CAP (1u << 12)
static WatchEntry s_watch[GCN_WATCH_RING_CAP];
static u64 s_watch_count;

void gcn_ring_watch_hit(u32 ea, u64 value, u32 size, u32 cia) {
    WatchEntry* e = &s_watch[s_watch_count & (GCN_WATCH_RING_CAP - 1)];
    e->block = s_block_index;
    e->pc = cia; e->ea = ea; e->value = value; e->size = (u8)size;
    s_watch_count++;
    if (s_watch_count <= 12u) {
        f32 vf;
        u32 v32 = (u32)value;
        memcpy(&vf, &v32, 4);
        fprintf(stderr, "[gcn-watch] #%llu blk=%llu pc=%08X ea=%08X size=%u "
                        "val=%0*llX(%.6g)\n",
                (unsigned long long)s_watch_count,
                (unsigned long long)s_block_index, cia, ea, size,
                (int)(size * 2u), (unsigned long long)value, (double)vf);
        fflush(stderr);
    }
}

void gcn_ring_watch_hit_span(u32 ea, u32 len, u32 tag) {
    WatchEntry* e = &s_watch[s_watch_count & (GCN_WATCH_RING_CAP - 1)];
    e->block = s_block_index;
    e->pc = tag; e->ea = ea; e->value = len; e->size = 0;   /* size 0 = span */
    s_watch_count++;
    fprintf(stderr, "[gcn-watch] SPAN #%llu blk=%llu tag=%08X ea=%08X len=%u\n",
            (unsigned long long)s_watch_count,
            (unsigned long long)s_block_index, tag, ea, len);
    fflush(stderr);
}

void gcn_ring_watch_dump_stderr(int max_entries) {
    u64 avail = s_watch_count < GCN_WATCH_RING_CAP ? s_watch_count : GCN_WATCH_RING_CAP;
    if ((u64)max_entries < avail) avail = (u64)max_entries;
    u64 start = s_watch_count - avail;
    fprintf(stderr, "[gcn-watch] ring (%llu total, cur block %llu, showing "
                    "#%llu..#%llu):\n",
            (unsigned long long)s_watch_count, (unsigned long long)s_block_index,
            (unsigned long long)start,
            (unsigned long long)(s_watch_count ? s_watch_count - 1u : 0u));
    for (u64 i = start; i < s_watch_count; i++) {
        const WatchEntry* e = &s_watch[i & (GCN_WATCH_RING_CAP - 1)];
        f32 vf;
        u32 v32 = (u32)e->value;
        memcpy(&vf, &v32, 4);
        fprintf(stderr, "  #%llu blk=%llu pc=%08X ea=%08X size=%u val=%0*llX(%.6g)\n",
                (unsigned long long)i, (unsigned long long)e->block, e->pc,
                e->ea, e->size, (int)(e->size * 2u),
                (unsigned long long)e->value, (double)vf);
    }
    fflush(stderr);
}

void gcn_ring_psq_dump_pc_range(u32 pc_lo, u32 pc_hi, int max_entries) {
    u64 avail = s_psq_count < GCN_PSQ_RING_CAP ? s_psq_count : GCN_PSQ_RING_CAP;
    /* newest-first scan to find the window, then print oldest->newest */
    u64 idx[256];
    int n = 0;
    int cap = max_entries < 256 ? max_entries : 256;
    for (u64 i = 0; i < avail && n < cap; i++) {
        u64 j = s_psq_count - 1u - i;
        const PsqEntry* e = &s_psq[j & (GCN_PSQ_RING_CAP - 1)];
        if (e->cia >= pc_lo && e->cia < pc_hi) idx[n++] = j;
    }
    fprintf(stderr, "[gx-fifoprov] psq ops in pc [%08X,%08X): newest %d shown:\n",
            pc_lo, pc_hi, n);
    for (int k = n - 1; k >= 0; k--) {
        const PsqEntry* e = &s_psq[idx[k] & (GCN_PSQ_RING_CAP - 1)];
        f32 v0, v1;
        memcpy(&v0, &e->ps0, 4);
        memcpy(&v1, &e->ps1, 4);
        fprintf(stderr, "  #%llu %s pc=%08X ea=%08X ps0=%08X(%.6g) ps1=%08X(%.6g)\n",
                (unsigned long long)e->seq, e->is_store ? "ST" : "LD",
                e->cia, e->ea, e->ps0, (double)v0, e->ps1, (double)v1);
    }
    fflush(stderr);
}

void gcn_ring_psq_dump_around_watched_store(int radius) {
    if (gcn_watch_len == 0u) return;
    u64 avail = s_psq_count < GCN_PSQ_RING_CAP ? s_psq_count : GCN_PSQ_RING_CAP;
    u64 found = (u64)-1;
    for (u64 i = 0; i < avail; i++) {
        u64 j = s_psq_count - 1u - i;
        const PsqEntry* e = &s_psq[j & (GCN_PSQ_RING_CAP - 1)];
        if (e->is_store &&
            (u32)((e->ea & 0x1FFFFFFFu) - gcn_watch_lo) < gcn_watch_len) {
            found = j;
            break;
        }
    }
    if (found == (u64)-1) {
        fprintf(stderr, "[gx-fifoprov] no psq store into watch range resident\n");
        return;
    }
    u64 lo = found > (u64)radius ? found - (u64)radius : 0u;
    u64 hi = found + (u64)radius + 1u;
    if (hi > s_psq_count) hi = s_psq_count;
    if (s_psq_count - lo > avail) lo = s_psq_count - avail;
    fprintf(stderr, "[gx-fifoprov] psq ops around newest watched store "
                    "(#%llu, ea in %08X+%X):\n",
            (unsigned long long)found, gcn_watch_lo, gcn_watch_len);
    for (u64 i = lo; i < hi; i++) {
        const PsqEntry* e = &s_psq[i & (GCN_PSQ_RING_CAP - 1)];
        f32 v0, v1;
        memcpy(&v0, &e->ps0, 4);
        memcpy(&v1, &e->ps1, 4);
        fprintf(stderr, "  #%llu %s pc=%08X ea=%08X ps0=%08X(%.6g) ps1=%08X(%.6g)%s\n",
                (unsigned long long)e->seq, e->is_store ? "ST" : "LD",
                e->cia, e->ea, e->ps0, (double)v0, e->ps1, (double)v1,
                i == found ? "   <-- watched" : "");
    }
    fflush(stderr);
}

void gcn_ring_psq_value_trace(const u32* words, int nwords, int max_print) {
    u64 avail = s_psq_count < GCN_PSQ_RING_CAP ? s_psq_count : GCN_PSQ_RING_CAP;
    u64 start = s_psq_count - avail;
    int printed = 0;
    fprintf(stderr, "[gx-fifoprov] psq value trace (%d target words over %llu "
                    "ring entries):\n", nwords, (unsigned long long)avail);
    for (u64 i = start; i < s_psq_count && printed < max_print; i++) {
        const PsqEntry* e = &s_psq[i & (GCN_PSQ_RING_CAP - 1)];
        int hit = 0;
        for (int wi = 0; wi < nwords && !hit; wi++) {
            u32 t = words[wi];
            if (t == 0u || (t & 0x7FFFFFFFu) == 0u) continue;   /* skip zeros */
            if (e->ps0 == t || e->ps1 == t) hit = 1;
        }
        if (!hit) continue;
        f32 v0, v1;
        memcpy(&v0, &e->ps0, 4);
        memcpy(&v1, &e->ps1, 4);
        fprintf(stderr, "  #%llu %s pc=%08X ea=%08X ps0=%08X(%.6g) ps1=%08X(%.6g)\n",
                (unsigned long long)e->seq, e->is_store ? "ST" : "LD",
                e->cia, e->ea, e->ps0, (double)v0, e->ps1, (double)v1);
        printed++;
    }
    if (printed >= max_print)
        fprintf(stderr, "  (value-trace print cap %d reached)\n", max_print);
    fflush(stderr);
}

int gcn_ring_fifo_find(u32 phys, u64* seq, u32* pc, u64* block, u8 out[32]) {
    u64 avail = s_fifo_count < GCN_FIFO_RING_CAP ? s_fifo_count : GCN_FIFO_RING_CAP;
    for (u64 i = 0; i < avail; i++) {
        const FifoEntry* e = &s_fifo[(s_fifo_count - 1u - i) & (GCN_FIFO_RING_CAP - 1)];
        if ((e->wptr & 0x1FFFFFFFu) == phys) {
            if (seq)   *seq = e->seq;
            if (pc)    *pc = e->pc;
            if (block) *block = e->block;
            if (out)   memcpy(out, e->data, 32);
            return 1;
        }
    }
    return 0;
}

void gcn_ring_event_dump_stderr(int max_entries) {
    u64 avail = s_event_count < GCN_EVENT_RING_CAP ? s_event_count : GCN_EVENT_RING_CAP;
    if ((u64)max_entries < avail) avail = (u64)max_entries;
    u64 start = s_event_count - avail;
    fprintf(stderr, "[gx-fifoprov] event ring (%llu total, cur block %llu, "
                    "showing #%llu..#%llu):\n",
            (unsigned long long)s_event_count, (unsigned long long)s_block_index,
            (unsigned long long)start,
            (unsigned long long)(s_event_count ? s_event_count - 1u : 0u));
    for (u64 i = start; i < s_event_count; i++) {
        const EventEntry* e = &s_event[i & (GCN_EVENT_RING_CAP - 1)];
        fprintf(stderr, "  #%llu blk=%llu kind=%u detail=0x%08X aux=0x%08X pc=%08X\n",
                (unsigned long long)e->seq, (unsigned long long)e->block,
                e->kind, e->detail, e->aux, e->pc);
    }
    fflush(stderr);
}

/* --- JSON emit helpers --- *
 * Walk the live window (oldest of the last min(count,cap,max) .. newest). Each
 * appends objects to `out`, stopping before cap to never overflow the buffer. */

static int emit_prefix(char* out, int cap, const char* name, u64 total) {
    return snprintf(out, (size_t)cap,
                    "{\"ok\":true,\"ring\":\"%s\",\"total\":%llu,\"entries\":[",
                    name, (unsigned long long)total);
}

int gcn_ring_mmio_json(char* out, int cap, int max_entries,
                       u32 addr_filter, int have_filter, int rw_filter) {
    int n = (int)emit_prefix(out, cap, "mmio", s_mmio_count);
    if (n < 0 || n >= cap) return n;

    u64 avail = s_mmio_count < GCN_MMIO_RING_CAP ? s_mmio_count : GCN_MMIO_RING_CAP;
    /* Filtering is applied over the FULL live window before the emit cap, so a
     * filtered query returns the newest matches, not the oldest of the window. */
    u64 start = s_mmio_count - avail;   /* oldest live seq */
    int emitted = 0, first = 1;
    /* Collect newest-first into a temporary index list, then emit oldest->newest. */
    u64 pick[4096];
    int npick = 0;
    if (max_entries > 4096) max_entries = 4096;
    for (u64 s = s_mmio_count; s > start && npick < max_entries; ) {
        s--;
        MmioEntry* e = &s_mmio[s & (GCN_MMIO_RING_CAP - 1)];
        if (have_filter && e->addr != addr_filter) continue;
        if (rw_filter >= 0 && e->rw != (u8)rw_filter) continue;
        pick[npick++] = s;
    }
    for (int i = npick - 1; i >= 0; i--) {
        MmioEntry* e = &s_mmio[pick[i] & (GCN_MMIO_RING_CAP - 1)];
        int w = snprintf(out + n, (size_t)(cap - n),
            "%s{\"seq\":%llu,\"block\":%llu,\"pc\":%u,\"addr\":%u,\"val\":%u,"
            "\"size\":%u,\"rw\":%u,\"mapped\":%u}",
            first ? "" : ",",
            (unsigned long long)e->seq, (unsigned long long)e->block,
            e->pc, e->addr, e->value, e->size, e->rw, e->mapped);
        if (w < 0 || n + w >= cap - 4) break;
        n += w; first = 0; emitted++;
    }
    n += snprintf(out + n, (size_t)(cap - n), "],\"count\":%d}\n", emitted);
    return n;
}

int gcn_ring_block_json(char* out, int cap, int max_entries) {
    int n = (int)emit_prefix(out, cap, "block", s_block_count);
    if (n < 0 || n >= cap) return n;
    u64 avail = s_block_count < GCN_BLOCK_RING_CAP ? s_block_count : GCN_BLOCK_RING_CAP;
    if ((u64)max_entries < avail) avail = (u64)max_entries;
    u64 start = s_block_count - avail;
    int emitted = 0, first = 1;
    for (u64 s = start; s < s_block_count; s++) {
        BlockEntry* e = &s_block[s & (GCN_BLOCK_RING_CAP - 1)];
        int w = snprintf(out + n, (size_t)(cap - n),
            "%s{\"seq\":%llu,\"pc\":%u}", first ? "" : ",",
            (unsigned long long)e->seq, e->pc);
        if (w < 0 || n + w >= cap - 4) break;
        n += w; first = 0; emitted++;
    }
    n += snprintf(out + n, (size_t)(cap - n), "],\"count\":%d}\n", emitted);
    return n;
}

int gcn_ring_fifo_json(char* out, int cap, int max_entries) {
    static const char hexd[] = "0123456789abcdef";
    int n = (int)emit_prefix(out, cap, "fifo", s_fifo_count);
    if (n < 0 || n >= cap) return n;
    u64 avail = s_fifo_count < GCN_FIFO_RING_CAP ? s_fifo_count : GCN_FIFO_RING_CAP;
    if ((u64)max_entries < avail) avail = (u64)max_entries;
    u64 start = s_fifo_count - avail;
    int emitted = 0, first = 1;
    for (u64 s = start; s < s_fifo_count; s++) {
        FifoEntry* e = &s_fifo[s & (GCN_FIFO_RING_CAP - 1)];
        int w = snprintf(out + n, (size_t)(cap - n),
            "%s{\"seq\":%llu,\"block\":%llu,\"pc\":%u,\"wptr\":%u,\"data\":\"",
            first ? "" : ",", (unsigned long long)e->seq,
            (unsigned long long)e->block, e->pc, e->wptr);
        if (w < 0 || n + w >= cap - 80) break;   /* leave room for 64 hex + closer */
        n += w;
        for (int i = 0; i < 32; i++) {
            out[n++] = hexd[e->data[i] >> 4];
            out[n++] = hexd[e->data[i] & 0xF];
        }
        n += snprintf(out + n, (size_t)(cap - n), "\"}");
        first = 0; emitted++;
    }
    n += snprintf(out + n, (size_t)(cap - n), "],\"count\":%d}\n", emitted);
    return n;
}

int gcn_ring_memcard_json(char* out, int cap, int max_entries) {
    int n = (int)emit_prefix(out, cap, "memcard", s_memcard_count);
    if (n < 0 || n >= cap) return n;
    u64 avail = s_memcard_count < GCN_MEMCARD_RING_CAP ? s_memcard_count : GCN_MEMCARD_RING_CAP;
    if ((u64)max_entries < avail) avail = (u64)max_entries;
    u64 start = s_memcard_count - avail;
    int emitted = 0, first = 1;
    for (u64 s = start; s < s_memcard_count; s++) {
        MemcardEntry* e = &s_memcard[s & (GCN_MEMCARD_RING_CAP - 1)];
        int w = snprintf(out + n, (size_t)(cap - n),
            "%s{\"seq\":%llu,\"block\":%llu,\"pc\":%u,\"channel\":%u,\"cs\":%u,"
            "\"command\":%u,\"rw\":%u,\"dma\":%u,\"address\":%u,\"length\":%u,\"data\":%u}",
            first ? "" : ",", (unsigned long long)e->seq, (unsigned long long)e->block,
            e->pc, e->channel, e->cs, e->command, e->rw, e->dma,
            e->address, e->length, e->data);
        if (w < 0 || n + w >= cap - 4) break;
        n += w; first = 0; emitted++;
    }
    n += snprintf(out + n, (size_t)(cap - n), "],\"count\":%d}\n", emitted);
    return n;
}

int gcn_ring_event_json(char* out, int cap, int max_entries) {
    int n = (int)emit_prefix(out, cap, "event", s_event_count);
    if (n < 0 || n >= cap) return n;
    u64 avail = s_event_count < GCN_EVENT_RING_CAP ? s_event_count : GCN_EVENT_RING_CAP;
    if ((u64)max_entries < avail) avail = (u64)max_entries;
    u64 start = s_event_count - avail;
    int emitted = 0, first = 1;
    for (u64 s = start; s < s_event_count; s++) {
        EventEntry* e = &s_event[s & (GCN_EVENT_RING_CAP - 1)];
        int w = snprintf(out + n, (size_t)(cap - n),
            "%s{\"seq\":%llu,\"block\":%llu,\"kind\":%u,\"detail\":%u,\"aux\":%u,\"pc\":%u}",
            first ? "" : ",", (unsigned long long)e->seq,
            (unsigned long long)e->block, e->kind, e->detail, e->aux, e->pc);
        if (w < 0 || n + w >= cap - 4) break;
        n += w; first = 0; emitted++;
    }
    n += snprintf(out + n, (size_t)(cap - n), "],\"count\":%d}\n", emitted);
    return n;
}
