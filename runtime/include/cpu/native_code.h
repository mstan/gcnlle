#ifndef GCN_NATIVE_CODE_H
#define GCN_NATIVE_CODE_H

#include "cpu/cpu.h"

/*
 * Page-granular validity fence for statically recompiled MEM1 code.
 *
 * A generated candidate is eligible only while its backing page still holds
 * the bytes it was compiled from. Disc DMA and interpreted stores invalidate
 * pages before control can reach newly loaded code. Invalid pages fall through
 * to the interpreter/capture path until a content-identified AOT module is
 * installed for them.
 */
void gcn_native_code_reset(void);
void gcn_native_code_invalidate(u32 address, u32 size);
bool gcn_native_code_is_invalid(u32 pc);
u32 gcn_native_code_invalid_page_count(void);

/* Content-dirtiness WITHOUT icache invalidation: the page's bytes changed,
 * but architecturally the instruction cache still holds the old line (dcbz's
 * zeroing store, gather-pipe redirect writes, GX XFB/EFB->RAM copies, any
 * device write the guest must icbi before executing). Feeds only the
 * native-miss page-CRC memo; never touches the native-dispatch fence.
 * gcn_native_code_invalidate implies this. */
void gcn_native_code_content_dirty(u32 address, u32 size);

/* Read-and-clear content-staleness bit for pc's MEM1 page: true if any
 * gcn_native_code_invalidate or gcn_native_code_content_dirty call (guest
 * store, DMA, icbi, dcbz, device RAM write) covered the page since the last
 * take. Backs interpreter.c's native-miss page-CRC memo; pages outside MEM1
 * always report stale. */
bool gcn_native_code_page_content_stale_take(u32 pc);

#endif
