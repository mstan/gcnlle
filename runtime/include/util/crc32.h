/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gcnrecomp runtime — pinned CRC32 (zlib/binascii-compatible).
 *
 * Standard reflected CRC32 (poly 0xEDB88320, init/final 0xFFFFFFFF) — the same
 * convention docs/DYNAMIC-CODE.md §3 requires ("two hashers must be bit-
 * identical" between an offline tool and the runtime): this matches Python's
 * `zlib.crc32`/`binascii.crc32` and zlib's `crc32()` exactly, so an offline
 * verification script and this runtime always agree on the same bytes.
 *
 * First consumer: the M1 BS1->BS2 jump-time integrity check (boot.c, "did our
 * own real EXI DMA through the transparent descrambler reproduce the offline
 * descramble exactly" — docs/M1_PLAN.md §8). General-purpose otherwise (the
 * DYNAMIC-CODE.md Layer-B/A content-hash validity gate will want the same
 * function later).
 */
#ifndef GCN_UTIL_CRC32_H
#define GCN_UTIL_CRC32_H

#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

u32 gcn_crc32(const u8* data, u32 len);

#ifdef __cplusplus
}
#endif

#endif /* GCN_UTIL_CRC32_H */
