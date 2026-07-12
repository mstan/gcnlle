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

void gcn_rings_init(void) {
    memset(s_mmio, 0, sizeof s_mmio);
    memset(s_block, 0, sizeof s_block);
    memset(s_event, 0, sizeof s_event);
    memset(s_fifo, 0, sizeof s_fifo);
    memset(s_memcard, 0, sizeof s_memcard);
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
