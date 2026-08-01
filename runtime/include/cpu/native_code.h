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

#endif
