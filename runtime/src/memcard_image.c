/* SPDX-License-Identifier: GPL-3.0-only
 *
 * gcnrecomp — GameCube memory-card image module (format/check/list/import).
 *
 * Provenance: every offset/algorithm here is transcribed from the Dolphin
 * oracle (GPL-2.0-or-later), file
 *   oracle/dolphin/Source/Core/Core/HW/GCMemcard/GCMemcard.cpp
 *   oracle/dolphin/Source/Core/Core/HW/GCMemcard/GCMemcard.h
 *   oracle/dolphin/Source/Core/Core/HW/GCMemcard/GCMemcardUtils.cpp
 * — see memcard_image.h's top-of-file comment for the exact line citations,
 * and F:/Projects/gcnrecomp/_work/M4_SPEC_FORMAT.md for the derived spec.
 *
 * This module reads/writes all multi-byte card fields as explicit big-endian
 * bytes directly (never via host-native struct layout), so it is correct on
 * both little- and big-endian build hosts without any of Dolphin's
 * BigEndianValue<T> wrapper machinery.
 */
#include "memcard/memcard_image.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ---- Byte-offset layout (private to this translation unit) ------------ */

/* Header, block 0 (GCMemcard.h:160-214) */
#define H_SERIAL           0x0000u /* 12 bytes */
#define H_FORMAT_TIME      0x000Cu /* 8 bytes, BE u64 */
#define H_SRAM_BIAS        0x0014u /* 4 bytes, BE u32 (recommendation; see spec ambiguity note) */
#define H_SRAM_LANGUAGE    0x0018u /* 4 bytes, BE u32 */
#define H_DTV_STATUS       0x001Cu /* 4 bytes, BE u32 (always 0) */
#define H_DEVICE_ID        0x0020u /* 2 bytes, BE u16 */
#define H_SIZE_MB          0x0022u /* 2 bytes, BE u16 */
#define H_ENCODING         0x0024u /* 2 bytes, BE u16 */
#define H_UNUSED_1         0x0026u /* 468 bytes, must be 0xFF */
#define H_UNUSED_1_LEN     468u
#define H_UPDATE_COUNTER   0x01FAu /* 2 bytes */
#define H_CHECKSUM         0x01FCu /* 2 bytes */
#define H_CHECKSUM_INV     0x01FEu /* 2 bytes */
#define H_UNUSED_2         0x0200u /* 7680 bytes, must be 0xFF */
#define H_UNUSED_2_LEN     7680u
#define H_CHECKSUM_AREA    0x01FCu /* sum over [0, H_CHECKSUM_AREA) */

/* Directory, blocks 1 & 2 (GCMemcard.h:316-332) */
#define D_ENTRIES          0x0000u /* 127 * 0x40 = 0x1FC0 bytes */
#define D_PADDING          0x1FC0u /* 0x3A bytes, must be 0xFF */
#define D_PADDING_LEN      0x3Au
#define D_UPDATE_COUNTER   0x1FFAu /* 2 bytes, signed */
#define D_CHECKSUM         0x1FFCu /* 2 bytes */
#define D_CHECKSUM_INV     0x1FFEu /* 2 bytes */
#define D_CHECKSUM_AREA    0x1FFCu /* sum over [0, D_CHECKSUM_AREA) */

/* DEntry, 0x40 bytes, offsets relative to entry start (GCMemcard.h:239-312) */
#define DE_GAMECODE        0x00u /* 4 bytes */
#define DE_MAKERCODE       0x04u /* 2 bytes */
#define DE_UNUSED_1        0x06u /* 1 byte */
#define DE_BANNER_ICON     0x07u /* 1 byte */
#define DE_FILENAME        0x08u /* 32 bytes */
#define DE_MOD_TIME        0x28u /* 4 bytes, BE u32 */
#define DE_IMAGE_OFFSET    0x2Cu /* 4 bytes, BE u32 */
#define DE_ICON_FORMAT     0x30u /* 2 bytes, BE u16 */
#define DE_ANIM_SPEED      0x32u /* 2 bytes, BE u16 */
#define DE_FILE_PERM       0x34u /* 1 byte */
#define DE_COPY_COUNTER    0x35u /* 1 byte */
#define DE_FIRST_BLOCK     0x36u /* 2 bytes, BE u16 */
#define DE_BLOCK_COUNT     0x38u /* 2 bytes, BE u16 */
#define DE_UNUSED_2        0x3Au /* 2 bytes */
#define DE_COMMENTS_ADDR   0x3Cu /* 4 bytes, BE u32 */

/* BlockAlloc (BAT), blocks 3 & 4 (GCMemcard.h:350-368) */
#define B_CHECKSUM         0x0000u /* 2 bytes */
#define B_CHECKSUM_INV     0x0002u /* 2 bytes */
#define B_UPDATE_COUNTER   0x0004u /* 2 bytes, signed */
#define B_FREE_BLOCKS      0x0006u /* 2 bytes, BE u16 */
#define B_LAST_ALLOC       0x0008u /* 2 bytes, BE u16 */
#define B_MAP              0x000Au /* BAT_SIZE * 2 = 8182 bytes, BE u16 each */
#define B_CHECKSUM_AREA_START 0x0004u /* sum over [B_CHECKSUM_AREA_START, BLOCK_SIZE) */

/* GCS/SAV/GCI save-container layout (GCMemcardUtils.cpp:19-26) */
#define GCI_HEADER_SIZE    GCN_MC_DENTRY_SIZE     /* 0x40 */
#define GCS_HEADER_SIZE    0x150u
#define GCS_DENTRY_OFFSET  0x110u
#define SAV_HEADER_SIZE    0xC0u
#define SAV_DENTRY_OFFSET  0x80u

/* ---- Big-endian raw accessors ------------------------------------------ */

static unsigned rd_be16(const unsigned char* p)
{
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}

static void wr_be16(unsigned char* p, unsigned v)
{
    p[0] = (unsigned char)((v >> 8) & 0xFFu);
    p[1] = (unsigned char)(v & 0xFFu);
}

static unsigned long rd_be32(const unsigned char* p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

static void wr_be32(unsigned char* p, unsigned long v)
{
    p[0] = (unsigned char)((v >> 24) & 0xFFu);
    p[1] = (unsigned char)((v >> 16) & 0xFFu);
    p[2] = (unsigned char)((v >> 8) & 0xFFu);
    p[3] = (unsigned char)(v & 0xFFu);
}

static void wr_be64(unsigned char* p, unsigned long long v)
{
    int i;
    for (i = 0; i < 8; ++i)
        p[i] = (unsigned char)((v >> (8 * (7 - i))) & 0xFFu);
}

/* ---- Checksum algorithm (GCMemcard.cpp:326-348, CalculateMemcardChecksums)
 *
 * Dolphin's implementation reads each big-endian 16-bit word, sums it, sums
 * its bitwise complement into a second accumulator, then (on their
 * little-endian build host) swaps both accumulators once more purely so the
 * result lands correctly in a plain (non-byte-order-wrapped) u16 field when
 * raw-copied to disk. Since we write big-endian bytes explicitly ourselves
 * (see wr_be16 above), that extra swap has no separate effect for us: the
 * "csum"/"inv_csum" values below already are the correct numbers to store,
 * big-endian, at the checksum offsets. (Cross-checked against the spec's
 * worked example: the blank Directory's closed-form checksum 0xF003 / 0
 * falls out of this exact loop — see gcn_mc_image_format()'s Directory step
 * and test_memcard_image.c's assertion on it.) */
static void calc_checksums(const unsigned char* data, size_t size,
                            unsigned* csum_out, unsigned* inv_csum_out)
{
    unsigned csum = 0;
    unsigned inv_csum = 0;
    size_t i;

    for (i = 0; i < size; i += 2) {
        unsigned d = rd_be16(&data[i]);
        csum = (csum + d) & 0xFFFFu;
        inv_csum = (inv_csum + (d ^ 0xFFFFu)) & 0xFFFFu;
    }
    if (csum == 0xFFFFu)
        csum = 0u;
    if (inv_csum == 0xFFFFu)
        inv_csum = 0u;

    *csum_out = csum;
    *inv_csum_out = inv_csum;
}

/* ---- Size helpers ------------------------------------------------------- */

static const unsigned VALID_MBITS[6] = {
    GCN_MC_MBIT_59, GCN_MC_MBIT_123, GCN_MC_MBIT_251,
    GCN_MC_MBIT_507, GCN_MC_MBIT_1019, GCN_MC_MBIT_2043
};

unsigned gcn_mc_image_bytes_for_mbits(unsigned size_mbits)
{
    return size_mbits * GCN_MC_BLOCKS_PER_MBIT * GCN_MC_BLOCK_SIZE;
}

int gcn_mc_image_valid_size(unsigned size_bytes)
{
    int i;
    for (i = 0; i < 6; ++i) {
        if (gcn_mc_image_bytes_for_mbits(VALID_MBITS[i]) == size_bytes)
            return 1;
    }
    return 0;
}

static unsigned mbits_for_bytes(unsigned size_bytes)
{
    int i;
    for (i = 0; i < 6; ++i) {
        if (gcn_mc_image_bytes_for_mbits(VALID_MBITS[i]) == size_bytes)
            return VALID_MBITS[i];
    }
    return 0;
}

static int all_ff(const unsigned char* p, size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i) {
        if (p[i] != 0xFFu)
            return 0;
    }
    return 1;
}

/* ---- err helper ---------------------------------------------------------*/

static void set_err(char* err, int errcap, const char* msg)
{
    if (!err || errcap <= 0)
        return;
#if defined(_MSC_VER)
    _snprintf_s(err, (size_t)errcap, _TRUNCATE, "%s", msg);
    err[errcap - 1] = '\0';
#else
    snprintf(err, (size_t)errcap, "%s", msg);
#endif
}

/* ==========================================================================
 * FORMAT
 * ==========================================================================
 */

int gcn_mc_image_format(unsigned char* buf, unsigned size_bytes,
                         const unsigned char flash_id[12], unsigned rtc_bias,
                         unsigned sram_language, unsigned long long format_time)
{
    unsigned size_mbits;
    unsigned free_blocks;
    unsigned csum, icsum;
    unsigned long long rand_v;
    int i;
    unsigned char dirblock[GCN_MC_BLOCK_SIZE];
    unsigned char batblock[GCN_MC_BLOCK_SIZE];
    unsigned char* hdr;

    if (!buf || !flash_id)
        return 0;
    if (!gcn_mc_image_valid_size(size_bytes))
        return 0;
    size_mbits = mbits_for_bytes(size_bytes);

    /* Step 2: fill everything 0xFF first (GCMemcardRaw.cpp:73 equivalent). */
    memset(buf, 0xFF, (size_t)size_bytes);

    /* Step 3: Header (block 0), per InitializeHeaderData (GCMemcard.cpp:1198-1221). */
    hdr = buf + 0;

    rand_v = format_time;
    for (i = 0; i < 12; ++i) {
        rand_v = ((rand_v * 0x41c64e6dULL) + 0x3039ULL) >> 16;
        hdr[H_SERIAL + i] = (unsigned char)(flash_id[i] + (unsigned)rand_v);
        rand_v = ((rand_v * 0x41c64e6dULL) + 0x3039ULL) >> 16;
        rand_v &= 0x7fffULL;
    }
    wr_be64(hdr + H_FORMAT_TIME, format_time);
    /* m_sram_bias / m_dtv_status: Dolphin's C++ types are plain (host-native)
     * u32, an artifact of running on little-endian hosts (see spec §2's
     * ambiguity note). We write both big-endian for consistency with every
     * other field and with real (big-endian PPC) hardware, per the spec's
     * explicit recommendation; CheckForErrors() never inspects either. */
    wr_be32(hdr + H_SRAM_BIAS, rtc_bias);
    wr_be32(hdr + H_SRAM_LANGUAGE, sram_language);
    wr_be32(hdr + H_DTV_STATUS, 0u);
    wr_be16(hdr + H_DEVICE_ID, 0u);       /* slot A */
    wr_be16(hdr + H_SIZE_MB, size_mbits);
    wr_be16(hdr + H_ENCODING, 0u);        /* 0 = Windows-1252 */
    /* H_UNUSED_1 / H_UPDATE_COUNTER already 0xFF from the whole-buffer fill. */
    calc_checksums(hdr + 0, H_CHECKSUM_AREA, &csum, &icsum);
    wr_be16(hdr + H_CHECKSUM, csum);
    wr_be16(hdr + H_CHECKSUM_INV, icsum);
    /* H_UNUSED_2 already 0xFF. */

    /* Step 4: Directory (blocks 1 & 2), per Directory() (GCMemcard.cpp:1316-1322).
     * Hardcoded closed-form checksum 0xF003 / 0 — see calc_checksums() comment
     * and the spec's worked derivation (§6 step 4). */
    memset(dirblock, 0xFF, sizeof dirblock);
    wr_be16(dirblock + D_UPDATE_COUNTER, 0u);
    wr_be16(dirblock + D_CHECKSUM, 0xF003u);
    wr_be16(dirblock + D_CHECKSUM_INV, 0u);
    memcpy(buf + (size_t)GCN_MC_BLOCK_SIZE * 1, dirblock, GCN_MC_BLOCK_SIZE);
    memcpy(buf + (size_t)GCN_MC_BLOCK_SIZE * 2, dirblock, GCN_MC_BLOCK_SIZE);

    /* Step 5: BAT (blocks 3 & 4), per BlockAlloc(size_mbits) (GCMemcard.cpp:549-555). */
    memset(batblock, 0x00, sizeof batblock); /* note: zero-fill, not 0xFF */
    free_blocks = size_mbits * GCN_MC_BLOCKS_PER_MBIT - GCN_MC_FST_BLOCKS;
    wr_be16(batblock + B_FREE_BLOCKS, free_blocks);
    wr_be16(batblock + B_LAST_ALLOC, 4u);
    /* B_UPDATE_COUNTER stays 0 from the zero-fill; B_MAP stays all-zero (free). */
    calc_checksums(batblock + B_CHECKSUM_AREA_START, GCN_MC_BLOCK_SIZE - B_CHECKSUM_AREA_START,
                   &csum, &icsum);
    wr_be16(batblock + B_CHECKSUM, csum);
    wr_be16(batblock + B_CHECKSUM_INV, icsum);
    memcpy(buf + (size_t)GCN_MC_BLOCK_SIZE * 3, batblock, GCN_MC_BLOCK_SIZE);
    memcpy(buf + (size_t)GCN_MC_BLOCK_SIZE * 4, batblock, GCN_MC_BLOCK_SIZE);

    /* Step 6: user data blocks (5..N-1) are already 0xFF from the initial fill. */
    return 1;
}

/* ==========================================================================
 * CHECK  (mirrors Header/Directory/BlockAlloc::CheckForErrors() and
 * GCMemcard::Open()'s corrupted dir/BAT block tolerance + CheckForErrorsWithBat)
 * ==========================================================================
 */

static unsigned bat_get_next_block(const unsigned char* bat, unsigned block)
{
    /* GCMemcard.cpp:557-564. Upper bound is checked against BAT_SIZE (4091),
     * not the card's own block count — an existing Dolphin quirk (flagged
     * there as "FIXME: fishy") that we mirror exactly for fidelity. */
    if (block < GCN_MC_FST_BLOCKS || block > 4091u)
        return 0u;
    return rd_be16(bat + B_MAP + (block - GCN_MC_FST_BLOCKS) * 2u);
}

/* Directory::CheckForErrorsWithBat (GCMemcard.cpp:1368-1423): 1 = consistent. */
static int dir_bat_consistent(const unsigned char* dir, const unsigned char* bat)
{
    unsigned i;
    for (i = 0; i < GCN_MC_DIRLEN; ++i) {
        const unsigned char* e = dir + D_ENTRIES + i * GCN_MC_DENTRY_SIZE;
        unsigned remaining, current;
        int match;

        if (all_ff(e + DE_GAMECODE, 4))
            continue;

        remaining = rd_be16(e + DE_BLOCK_COUNT);
        current = rd_be16(e + DE_FIRST_BLOCK);
        match = 0;
        for (;;) {
            unsigned next;
            if (remaining == 0)
                break; /* ran out before seeing the BAT terminator: inconsistent */
            --remaining;
            next = bat_get_next_block(bat, current);
            if (next == 0)
                break; /* unallocated / out-of-range: inconsistent */
            if (next == 0xFFFFu) {
                match = (remaining == 0);
                break;
            }
            current = next;
        }
        if (!match)
            return 0;
    }
    return 1;
}

int gcn_mc_image_check(const unsigned char* buf, unsigned size_bytes, char* err, int errcap)
{
    unsigned mbits, total_blocks, total_avail;
    const unsigned char* hdr;
    unsigned csum, icsum;
    int hdr_mismatched, hdr_bad_checksum;
    int dir_bad_checksum[2];
    int bat_bad_checksum[2], bat_bad_free[2];
    int num_corrupted;
    int active_dir, active_bat;
    unsigned k;

    if (!buf) {
        set_err(err, errcap, "null buffer");
        return 0;
    }
    if (!gcn_mc_image_valid_size(size_bytes)) {
        set_err(err, errcap, "invalid card size");
        return 0;
    }
    mbits = mbits_for_bytes(size_bytes);
    total_blocks = size_bytes / GCN_MC_BLOCK_SIZE;
    total_avail = total_blocks - GCN_MC_FST_BLOCKS;

    /* ---- Header::CheckForErrors (GCMemcard.cpp:1288-1309) ---- */
    hdr = buf + 0;
    hdr_mismatched = (rd_be16(hdr + H_SIZE_MB) != mbits);
    calc_checksums(hdr + 0, H_CHECKSUM_AREA, &csum, &icsum);
    hdr_bad_checksum = (csum != rd_be16(hdr + H_CHECKSUM)) ||
                       (icsum != rd_be16(hdr + H_CHECKSUM_INV));
    /* unused-area fill is DATA_IN_UNUSED_AREA, non-critical per HasCriticalErrors() */

    if (hdr_mismatched) {
        set_err(err, errcap, "header: card size mismatch");
        return 0;
    }
    if (hdr_bad_checksum) {
        set_err(err, errcap, "header: invalid checksum");
        return 0;
    }

    /* ---- Directory::CheckForErrors, both copies (GCMemcard.cpp:1352-1366) ---- */
    for (k = 0; k < 2; ++k) {
        const unsigned char* d = buf + (size_t)GCN_MC_BLOCK_SIZE * (1u + k);
        calc_checksums(d + 0, D_CHECKSUM_AREA, &csum, &icsum);
        dir_bad_checksum[k] = (csum != rd_be16(d + D_CHECKSUM)) ||
                              (icsum != rd_be16(d + D_CHECKSUM_INV));
        /* m_padding fill is DATA_IN_UNUSED_AREA, non-critical; not tracked here. */
    }

    /* ---- BlockAlloc::CheckForErrors, both copies (GCMemcard.cpp:645-687) ---- */
    for (k = 0; k < 2; ++k) {
        const unsigned char* b = buf + (size_t)GCN_MC_BLOCK_SIZE * (3u + k);
        unsigned in_use = 0, i;
        unsigned free_declared = rd_be16(b + B_FREE_BLOCKS);
        unsigned free_computed;

        calc_checksums(b + B_CHECKSUM_AREA_START, GCN_MC_BLOCK_SIZE - B_CHECKSUM_AREA_START,
                       &csum, &icsum);
        bat_bad_checksum[k] = (csum != rd_be16(b + B_CHECKSUM)) ||
                              (icsum != rd_be16(b + B_CHECKSUM_INV));

        for (i = 0; i < total_avail; ++i) {
            if (rd_be16(b + B_MAP + i * 2u) != 0)
                ++in_use;
        }
        free_computed = total_avail - in_use;
        bat_bad_free[k] = (free_computed != free_declared);
        /* entries beyond total_avail being non-zero is DATA_IN_UNUSED_AREA,
         * non-critical; not tracked here (our own writer never does this). */
    }

    num_corrupted = (dir_bad_checksum[0] ? 1 : 0) + (dir_bad_checksum[1] ? 1 : 0) +
                    (bat_bad_checksum[0] || bat_bad_free[0] ? 1 : 0) +
                    (bat_bad_checksum[1] || bat_bad_free[1] ? 1 : 0);

    if (num_corrupted > 1) {
        set_err(err, errcap, "more than one directory/BAT block is corrupted");
        return 0;
    }

    /* Pick the non-corrupted copy of each pair as "active" (GCMemcard.cpp:255-263
     * picks by update counter; since our own writer always keeps both mirrors
     * identical, any tie-break is equivalent — we just avoid a corrupted one). */
    active_dir = dir_bad_checksum[0] ? 1 : 0;
    active_bat = (bat_bad_checksum[0] || bat_bad_free[0]) ? 1 : 0;

    if (dir_bad_checksum[active_dir]) {
        set_err(err, errcap, "directory: invalid checksum");
        return 0;
    }
    if (bat_bad_checksum[active_bat]) {
        set_err(err, errcap, "BAT: invalid checksum");
        return 0;
    }
    if (bat_bad_free[active_bat]) {
        set_err(err, errcap, "BAT: free_blocks mismatch");
        return 0;
    }

    if (!dir_bat_consistent(buf + (size_t)GCN_MC_BLOCK_SIZE * (1u + (unsigned)active_dir),
                            buf + (size_t)GCN_MC_BLOCK_SIZE * (3u + (unsigned)active_bat))) {
        set_err(err, errcap, "directory/BAT inconsistent (DIR_BAT_INCONSISTENT)");
        return 0;
    }

    return 1;
}

/* ==========================================================================
 * LIST
 * ==========================================================================
 */

/* GetSaveDataBytes (GCMemcard.cpp:452-510): walk the BAT chain from
 * first_block, copying `length` bytes starting at byte `offset` into the
 * save's own flattened data. Returns the number of bytes actually copied
 * (0 on a hard failure / out-of-range chain). */
static unsigned read_save_bytes(const unsigned char* buf, unsigned size_bytes,
                                 unsigned first_block, unsigned block_count,
                                 unsigned offset, unsigned length, unsigned char* out)
{
    const unsigned char* bat = buf + (size_t)GCN_MC_BLOCK_SIZE * 3;
    unsigned total_blocks = size_bytes / GCN_MC_BLOCK_SIZE;
    unsigned long file_size, bytes_to_copy, copied;
    unsigned current, off_in_block;

    if (block_count == 0xFFFFu || first_block < GCN_MC_FST_BLOCKS || first_block >= total_blocks)
        return 0;

    file_size = (unsigned long)block_count * GCN_MC_BLOCK_SIZE;
    if (offset >= file_size)
        return 0;

    bytes_to_copy = (unsigned long)length;
    if ((unsigned long)offset + bytes_to_copy > file_size)
        bytes_to_copy = file_size - offset;

    current = first_block;
    off_in_block = offset;
    while (off_in_block >= GCN_MC_BLOCK_SIZE) {
        off_in_block -= GCN_MC_BLOCK_SIZE;
        current = bat_get_next_block(bat, current);
        if (current < GCN_MC_FST_BLOCKS || current >= total_blocks)
            return 0;
    }

    copied = 0;
    for (;;) {
        const unsigned char* blockptr =
            buf + (size_t)GCN_MC_BLOCK_SIZE * GCN_MC_FST_BLOCKS +
            (size_t)(current - GCN_MC_FST_BLOCKS) * GCN_MC_BLOCK_SIZE;
        unsigned long avail = GCN_MC_BLOCK_SIZE - off_in_block;
        unsigned long take = (bytes_to_copy - copied < avail) ? (bytes_to_copy - copied) : avail;

        memcpy(out + copied, blockptr + off_in_block, (size_t)take);
        copied += take;
        if (copied >= bytes_to_copy)
            break;

        off_in_block = 0;
        current = bat_get_next_block(bat, current);
        if (current < GCN_MC_FST_BLOCKS || current >= total_blocks)
            return (unsigned)copied; /* short read: caller checks against expected length */
    }
    return (unsigned)copied;
}

static void copy_field(char* dst, size_t dstcap, const unsigned char* src, size_t len)
{
    size_t n = (len < dstcap - 1) ? len : dstcap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void strip_at_null(char* s, size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i) {
        if (s[i] == '\0')
            return;
    }
}

int gcn_mc_image_list(const unsigned char* buf, unsigned size_bytes,
                      gcn_mc_dirent_t* out, int out_cap)
{
    const unsigned char* dir;
    unsigned i;
    int count = 0;

    if (!buf || !gcn_mc_image_valid_size(size_bytes))
        return 0;

    dir = buf + (size_t)GCN_MC_BLOCK_SIZE * 1;

    for (i = 0; i < GCN_MC_DIRLEN; ++i) {
        const unsigned char* e = dir + D_ENTRIES + i * GCN_MC_DENTRY_SIZE;
        gcn_mc_dirent_t entry;
        unsigned comments_addr;
        unsigned first_block, block_count;

        if (all_ff(e + DE_GAMECODE, 4))
            continue;

        memset(&entry, 0, sizeof entry);
        copy_field(entry.gamecode, sizeof entry.gamecode, e + DE_GAMECODE, 4);
        copy_field(entry.makercode, sizeof entry.makercode, e + DE_MAKERCODE, 2);
        copy_field(entry.filename, sizeof entry.filename, e + DE_FILENAME, GCN_MC_DENTRY_STRLEN);
        strip_at_null(entry.filename, sizeof entry.filename);

        first_block = rd_be16(e + DE_FIRST_BLOCK);
        block_count = rd_be16(e + DE_BLOCK_COUNT);
        entry.first_block = first_block;
        entry.block_count = block_count;

        comments_addr = (unsigned)rd_be32(e + DE_COMMENTS_ADDR);
        entry.comment1[0] = '\0';
        entry.comment2[0] = '\0';
        if (comments_addr != 0xFFFFFFFFu) {
            unsigned char raw[GCN_MC_DENTRY_STRLEN * 2];
            unsigned got = read_save_bytes(buf, size_bytes, first_block, block_count,
                                           comments_addr, (unsigned)sizeof raw, raw);
            if (got == sizeof raw) {
                copy_field(entry.comment1, sizeof entry.comment1, raw, GCN_MC_DENTRY_STRLEN);
                strip_at_null(entry.comment1, sizeof entry.comment1);
                copy_field(entry.comment2, sizeof entry.comment2,
                          raw + GCN_MC_DENTRY_STRLEN, GCN_MC_DENTRY_STRLEN);
                strip_at_null(entry.comment2, sizeof entry.comment2);
            }
        }

        if (out && count < out_cap)
            out[count] = entry;
        ++count;
    }

    return count;
}

/* ==========================================================================
 * IMPORT SAVE
 * ==========================================================================
 */

/* ByteswapDEntrySavHeader (GCMemcardUtils.cpp:91-96): the fixed list of
 * byte-pair swaps that convert between the SAV (MaxDrive) on-disk DEntry
 * byte layout and the internal/card DEntry byte layout. The transform is an
 * involution, so the same function is used for both extract and inject. */
static void sav_swap_pairs(unsigned char e[GCN_MC_DENTRY_SIZE])
{
    static const unsigned offs[] = {0x06u, 0x2Cu, 0x2Eu, 0x30u, 0x32u,
                                    0x34u, 0x36u, 0x38u, 0x3Au, 0x3Cu, 0x3Eu};
    size_t i;
    for (i = 0; i < sizeof(offs) / sizeof(offs[0]); ++i) {
        unsigned char t = e[offs[i]];
        e[offs[i]] = e[offs[i] + 1];
        e[offs[i] + 1] = t;
    }
}

/* Detects .gci/.gcs/.sav from filesize % BLOCK_SIZE (GCMemcardUtils.cpp:206-228,
 * ReadSavefile) and extracts a normalized DEntry (in internal/card byte
 * layout) plus a pointer to + count of the raw 8192-byte block payloads.
 * Returns 1 on success, 0 on a corrupt/unrecognized container (err set). */
static int detect_and_parse_save(const unsigned char* save, unsigned save_len,
                                  unsigned char dentry[GCN_MC_DENTRY_SIZE],
                                  const unsigned char** block_data_out,
                                  unsigned* block_count_out, char* err, int errcap)
{
    static const unsigned char GCS_MAGIC[6] = {0x47, 0x43, 0x53, 0x41, 0x56, 0x45}; /* "GCSAVE" */
    static const unsigned char SAV_MAGIC[12] = {0x44, 0x41, 0x54, 0x45, 0x4C, 0x47,
                                                0x43, 0x5F, 0x53, 0x41, 0x56, 0x45}; /* "DATELGC_SAVE" */
    unsigned header_mod;

    if (!save || save_len < GCN_MC_BLOCK_SIZE) {
        set_err(err, errcap, "save file too small");
        return 0;
    }
    header_mod = save_len % GCN_MC_BLOCK_SIZE;

    if (header_mod == GCI_HEADER_SIZE) {
        unsigned block_count;
        unsigned long long expected;

        memcpy(dentry, save, GCN_MC_DENTRY_SIZE);
        block_count = rd_be16(dentry + DE_BLOCK_COUNT);
        expected = (unsigned long long)GCN_MC_DENTRY_SIZE +
                   (unsigned long long)block_count * GCN_MC_BLOCK_SIZE;
        if (expected != (unsigned long long)save_len) {
            set_err(err, errcap, "GCI: size does not match declared block count");
            return 0;
        }
        *block_data_out = save + GCN_MC_DENTRY_SIZE;
        *block_count_out = block_count;
        return 1;
    }

    if (header_mod == GCS_HEADER_SIZE) {
        unsigned long long total_block_bytes;
        unsigned block_count;

        if (save_len < GCS_HEADER_SIZE || memcmp(save, GCS_MAGIC, sizeof GCS_MAGIC) != 0) {
            set_err(err, errcap, "GCS: bad magic");
            return 0;
        }
        memcpy(dentry, save + GCS_DENTRY_OFFSET, GCN_MC_DENTRY_SIZE);

        /* The GCS block-count field is unreliable (Dolphin: "always 1" unless
         * written by the original GameSaves software) — recompute it from the
         * filesize, exactly as ReadSavefileInternalGCS does. */
        total_block_bytes = (unsigned long long)save_len - GCS_HEADER_SIZE;
        if ((total_block_bytes % GCN_MC_BLOCK_SIZE) != 0) {
            set_err(err, errcap, "GCS: block data not a multiple of block size");
            return 0;
        }
        block_count = (unsigned)(total_block_bytes / GCN_MC_BLOCK_SIZE);
        wr_be16(dentry + DE_BLOCK_COUNT, block_count);

        *block_data_out = save + GCS_HEADER_SIZE;
        *block_count_out = block_count;
        return 1;
    }

    if (header_mod == SAV_HEADER_SIZE) {
        unsigned char tmp[GCN_MC_DENTRY_SIZE];
        unsigned block_count;
        unsigned long long expected;

        if (save_len < SAV_HEADER_SIZE || memcmp(save, SAV_MAGIC, sizeof SAV_MAGIC) != 0) {
            set_err(err, errcap, "SAV: bad magic");
            return 0;
        }
        memcpy(tmp, save + SAV_DENTRY_OFFSET, GCN_MC_DENTRY_SIZE);
        sav_swap_pairs(tmp);
        memcpy(dentry, tmp, GCN_MC_DENTRY_SIZE);

        block_count = rd_be16(dentry + DE_BLOCK_COUNT);
        expected = (unsigned long long)SAV_HEADER_SIZE +
                   (unsigned long long)block_count * GCN_MC_BLOCK_SIZE;
        if (expected != (unsigned long long)save_len) {
            set_err(err, errcap, "SAV: size does not match declared block count");
            return 0;
        }
        *block_data_out = save + SAV_HEADER_SIZE;
        *block_count_out = block_count;
        return 1;
    }

    set_err(err, errcap, "unrecognized save container (bad size % BLOCK_SIZE)");
    return 0;
}

/* HasSameIdentity's filename comparison (GCMemcardUtils.cpp:28-76): compare
 * as NUL-terminated strings, not raw fixed-size arrays. */
static int filenames_same_identity(const unsigned char* a, const unsigned char* b, size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i) {
        if (a[i] == 0)
            return b[i] == 0;
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

/* BlockAlloc::NextFreeBlock (GCMemcard.cpp:568-583). Parameters/returns are
 * memory-card block indices (block 5 = first user block). */
static unsigned bat_next_free_block(const unsigned char* bat, unsigned max_block,
                                     unsigned starting_block, unsigned free_blocks_precondition)
{
    unsigned lo = GCN_MC_FST_BLOCKS;
    unsigned hi = GCN_MC_BAT_SIZE + GCN_MC_FST_BLOCKS;
    unsigned start, mx, i;

    if (free_blocks_precondition == 0)
        return 0xFFFFu;

    start = starting_block;
    if (start < lo) start = lo;
    if (start > hi) start = hi;
    mx = max_block;
    if (mx < lo) mx = lo;
    if (mx > hi) mx = hi;

    for (i = start; i < mx; ++i) {
        if (rd_be16(bat + B_MAP + (i - GCN_MC_FST_BLOCKS) * 2u) == 0)
            return i;
    }
    for (i = lo; i < start; ++i) {
        if (rd_be16(bat + B_MAP + (i - GCN_MC_FST_BLOCKS) * 2u) == 0)
            return i;
    }
    return 0xFFFFu;
}

int gcn_mc_image_import_save(unsigned char* buf, unsigned size_bytes,
                              const unsigned char* save, unsigned save_len,
                              char* err, int errcap)
{
    unsigned mbits, max_block;
    unsigned char dentry[GCN_MC_DENTRY_SIZE];
    const unsigned char* blockdata;
    unsigned block_count;
    unsigned char* dir;
    unsigned char* bat;
    int free_slot, num_files;
    unsigned i;
    unsigned free_blocks, last_alloc, first_block;
    unsigned char newentry[GCN_MC_DENTRY_SIZE];
    unsigned csum, icsum;
    unsigned cur;
    int b;

    if (!buf || !save) {
        set_err(err, errcap, "null buffer");
        return 0;
    }
    if (!gcn_mc_image_valid_size(size_bytes)) {
        set_err(err, errcap, "invalid card size");
        return 0;
    }
    mbits = mbits_for_bytes(size_bytes);
    max_block = mbits * GCN_MC_BLOCKS_PER_MBIT;

    if (!detect_and_parse_save(save, save_len, dentry, &blockdata, &block_count, err, errcap))
        return 0;

    dir = buf + (size_t)GCN_MC_BLOCK_SIZE * 1;
    bat = buf + (size_t)GCN_MC_BLOCK_SIZE * 3;

    /* GCMemcard::ImportFile (GCMemcard.cpp:713-731): dir-full / out-of-blocks /
     * title-present checks, in that order. */
    free_slot = -1;
    num_files = 0;
    for (i = 0; i < GCN_MC_DIRLEN; ++i) {
        const unsigned char* e = dir + D_ENTRIES + i * GCN_MC_DENTRY_SIZE;
        int unused = all_ff(e + DE_GAMECODE, 4);

        if (unused) {
            if (free_slot < 0)
                free_slot = (int)i;
            continue;
        }
        ++num_files;

        if (memcmp(e + DE_GAMECODE, dentry + DE_GAMECODE, 4) == 0 &&
            memcmp(e + DE_MAKERCODE, dentry + DE_MAKERCODE, 2) == 0 &&
            filenames_same_identity(e + DE_FILENAME, dentry + DE_FILENAME, GCN_MC_DENTRY_STRLEN)) {
            set_err(err, errcap, "a save with this identity is already present on the card");
            return 0;
        }
    }
    if (num_files >= (int)GCN_MC_DIRLEN || free_slot < 0) {
        set_err(err, errcap, "directory is full (OUTOFDIRENTRIES)");
        return 0;
    }

    free_blocks = rd_be16(bat + B_FREE_BLOCKS);
    if (block_count > free_blocks) {
        set_err(err, errcap, "not enough free blocks (OUTOFBLOCKS)");
        return 0;
    }

    /* firstBlock = GetActiveBat().NextFreeBlock(m_size_blocks, last_allocated_block) */
    last_alloc = rd_be16(bat + B_LAST_ALLOC);
    first_block = bat_next_free_block(bat, max_block, last_alloc, free_blocks);
    if (first_block == 0xFFFFu) {
        set_err(err, errcap, "no free block found (OUTOFBLOCKS)");
        return 0;
    }

    /* Place the DEntry: copy, then fix up m_first_block and bump m_copy_counter
     * (GCMemcard.cpp:745-747). m_block_count is already correct from parsing. */
    memcpy(newentry, dentry, GCN_MC_DENTRY_SIZE);
    wr_be16(newentry + DE_FIRST_BLOCK, first_block);
    newentry[DE_COPY_COUNTER] = (unsigned char)(newentry[DE_COPY_COUNTER] + 1);
    memcpy(dir + D_ENTRIES + (size_t)free_slot * GCN_MC_DENTRY_SIZE, newentry, GCN_MC_DENTRY_SIZE);
    {
        unsigned uc = rd_be16(dir + D_UPDATE_COUNTER);
        wr_be16(dir + D_UPDATE_COUNTER, (uc + 1) & 0xFFFFu);
    }

    /* Copy block data and chain the BAT map (GCMemcard.cpp:760-775). Each
     * loop iteration marks the CURRENT block with a pointer to the NEXT free
     * block found from current+1 (or 0xFFFF on the last block), mirroring
     * Dolphin's "no free-space fragmentation" assumption exactly. */
    cur = first_block;
    for (b = 0; b < (int)block_count; ++b) {
        unsigned char* dst = buf + (size_t)GCN_MC_BLOCK_SIZE * GCN_MC_FST_BLOCKS +
                             (size_t)(cur - GCN_MC_FST_BLOCKS) * GCN_MC_BLOCK_SIZE;
        unsigned next;

        memcpy(dst, blockdata + (size_t)b * GCN_MC_BLOCK_SIZE, GCN_MC_BLOCK_SIZE);

        if (b == (int)block_count - 1)
            next = 0xFFFFu;
        else
            next = bat_next_free_block(bat, max_block, cur + 1, free_blocks);

        wr_be16(bat + B_MAP + (cur - GCN_MC_FST_BLOCKS) * 2u, next);
        wr_be16(bat + B_LAST_ALLOC, cur);
        cur = next;
    }

    wr_be16(bat + B_FREE_BLOCKS, (free_blocks - block_count) & 0xFFFFu);
    {
        unsigned uc = rd_be16(bat + B_UPDATE_COUNTER);
        wr_be16(bat + B_UPDATE_COUNTER, (uc + 1) & 0xFFFFu);
    }

    /* Fix checksums (GCMemcard::FixChecksums, GCMemcard.cpp:350-362 — header
     * is untouched by an import so it needs no recompute). */
    calc_checksums(dir + 0, D_CHECKSUM_AREA, &csum, &icsum);
    wr_be16(dir + D_CHECKSUM, csum);
    wr_be16(dir + D_CHECKSUM_INV, icsum);
    calc_checksums(bat + B_CHECKSUM_AREA_START, GCN_MC_BLOCK_SIZE - B_CHECKSUM_AREA_START,
                   &csum, &icsum);
    wr_be16(bat + B_CHECKSUM, csum);
    wr_be16(bat + B_CHECKSUM_INV, icsum);

    /* Mirror into the backup copies (blocks 2 and 4). Deviation from Dolphin:
     * real GCMemcard::UpdateDirectory/UpdateBat (GCMemcard.cpp:287-301) write
     * only the *inactive* half of each double-buffered pair and flip which
     * one is active, leaving the other stale as a crash-recovery backup. This
     * module instead keeps both copies always identical, per this module's
     * brief — simpler and always self-consistent for a single offline import,
     * at the cost of not modeling Dolphin's power-loss journaling. */
    memcpy(buf + (size_t)GCN_MC_BLOCK_SIZE * 2, dir, GCN_MC_BLOCK_SIZE);
    memcpy(buf + (size_t)GCN_MC_BLOCK_SIZE * 4, bat, GCN_MC_BLOCK_SIZE);

    return 1;
}

/* ==========================================================================
 * SAVE FILE
 * ==========================================================================
 */

int gcn_mc_image_save_file(const char* path, const unsigned char* buf, unsigned size_bytes)
{
    FILE* f;
    size_t written;

    if (!path || !buf)
        return 0;

    f = fopen(path, "wb");
    if (!f)
        return 0;

    written = fwrite(buf, 1, (size_t)size_bytes, f);
    if (fclose(f) != 0)
        return 0;

    return written == (size_t)size_bytes;
}
