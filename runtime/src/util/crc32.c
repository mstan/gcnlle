/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gcnrecomp runtime — pinned CRC32 (see include/util/crc32.h for provenance).
 */
#include "util/crc32.h"

u32 gcn_crc32(const u8* data, u32 len) {
    static u32 table[256];
    static int table_ready = 0;
    if (!table_ready) {
        for (u32 i = 0; i < 256u; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        table_ready = 1;
    }

    u32 crc = 0xFFFFFFFFu;
    for (u32 i = 0; i < len; i++)
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
