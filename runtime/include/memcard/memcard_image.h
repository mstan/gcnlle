/* SPDX-License-Identifier: GPL-3.0-only
 *
 * gcnrecomp — GameCube memory-card *image* module.
 *
 * Standalone C11 module that formats/validates/lists/imports a Dolphin-compatible
 * flat `.raw` memory-card image. This is NOT the runtime's live EXI memcard device
 * model (see runtime/src/memcard.c) — it is offline image tooling used to
 * provision a `.raw` file that the runtime's memcard device can then mount.
 *
 * Every offset, field width, endianness and checksum below is transcribed from
 * the Dolphin oracle (GPL-2.0-or-later), cross-checked against the project's
 * derived memory-card format notes:
 *   - Sizes/blocks/header/directory/BAT layout & the checksum algorithm:
 *     oracle/dolphin/Source/Core/Core/HW/GCMemcard/GCMemcard.h, GCMemcard.cpp
 *     (CalculateMemcardChecksums:326-348, Format:1017-1053,
 *      InitializeHeaderData:1198-1221, Header::CheckForErrors:1288-1309,
 *      Directory():1316-1322, Directory::CheckForErrors[WithBat]:1352-1423,
 *      BlockAlloc():549-555, GetNextBlock:557-564, NextFreeBlock:568-583,
 *      AssignBlocksContiguous:614-630 (not used directly; see memcard_image.c),
 *      BlockAlloc::CheckForErrors:645-687, GCMemcard::ImportFile:713-784).
 *   - .gci/.gcs/.sav container formats:
 *     oracle/dolphin/Source/Core/Core/HW/GCMemcard/GCMemcardUtils.cpp
 *     (ReadSavefileInternalGCI/GCS/SAV:131-204, ByteswapDEntrySavHeader:91-96).
 *
 * All multi-byte on-card fields are BIG-ENDIAN regardless of host (the GameCube
 * is big-endian PPC). This module always writes/reads explicit big-endian bytes
 * itself; it never relies on host struct layout or byte order.
 */
#ifndef GCNRECOMP_MEMCARD_IMAGE_H
#define GCNRECOMP_MEMCARD_IMAGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Format constants (GCMemcard.h) ---------------------------------- */

#define GCN_MC_BLOCK_SIZE       0x2000u   /* GCMemcard.h:89 */
#define GCN_MC_BLOCKS_PER_MBIT  16u       /* GCMemcard.h:92 (MBIT_TO_BLOCKS) */
#define GCN_MC_FST_BLOCKS       5u        /* GCMemcard.h:95 (MC_FST_BLOCKS) */
#define GCN_MC_DIRLEN           127u      /* GCMemcard.h:98 (DIRLEN) */
#define GCN_MC_DENTRY_SIZE      0x40u     /* GCMemcard.h:104 (DENTRY_SIZE) */
#define GCN_MC_DENTRY_STRLEN    0x20u     /* GCMemcard.h:101 (DENTRY_STRLEN) */
#define GCN_MC_BAT_SIZE         0xFFBu    /* GCMemcard.h:107 (BAT_SIZE = 4091) */

/* The six card sizes GCMemcard::Open() accepts, in Mbit (GCMemcard.h:112-117). */
#define GCN_MC_MBIT_59    0x04u
#define GCN_MC_MBIT_123   0x08u
#define GCN_MC_MBIT_251   0x10u
#define GCN_MC_MBIT_507   0x20u
#define GCN_MC_MBIT_1019  0x40u
#define GCN_MC_MBIT_2043  0x80u

/* ---- Directory listing entry ------------------------------------------ */

typedef struct gcn_mc_dirent {
    char gamecode[5];      /* 4-char game code + NUL, e.g. "GALE" */
    char makercode[3];     /* 2-char maker code + NUL, e.g. "01"  */
    char filename[33];     /* up to 32 chars + NUL                */
    char comment1[33];     /* big BIOS comment line, blank if unresolved */
    char comment2[33];     /* small BIOS comment line, blank if unresolved */
    unsigned first_block;  /* DEntry::m_first_block                */
    unsigned block_count;  /* DEntry::m_block_count                */
} gcn_mc_dirent_t;

/* ---- API --------------------------------------------------------------
 * All functions operate on a caller-owned flat byte buffer that is the exact
 * bit-for-bit image of a `.raw` memory-card file (block 0 = header, blocks
 * 1/2 = directory + backup, blocks 3/4 = BAT + backup, blocks 5..N-1 = user
 * data), per GCMemcard.cpp:1027-1031 / GCMemcard.cpp:308-324.
 */

/* Returns 1 if size_bytes is one of the six valid card sizes, else 0. */
int gcn_mc_image_valid_size(unsigned size_bytes);

/* Returns the total .raw file size in bytes for a card of size_mbits Mbit.
 * (size_mbits need not itself be validated by this helper; combine with
 * gcn_mc_image_valid_size() on the result if you need to check validity.) */
unsigned gcn_mc_image_bytes_for_mbits(unsigned size_mbits);

/* Formats a blank card into a caller-provided buffer of exactly size_bytes
 * bytes (must be one of the six valid sizes). flash_id must point to 12
 * bytes (CardFlashId, Sram.h:47) — feeds the per-card serial LCG
 * (GCMemcard.cpp:1198-1221). rtc_bias/sram_language/format_time are stored
 * informationally in the header and never validated by CheckForErrors().
 * Returns 1 on success, 0 on bad arguments/size. */
int gcn_mc_image_format(unsigned char* buf, unsigned size_bytes,
                         const unsigned char flash_id[12], unsigned rtc_bias,
                         unsigned sram_language, unsigned long long format_time);

/* Validates size, header/directory/BAT checksums, the header's and
 * directory's 0xFF unused-area fill, the BAT's free-block count and
 * unused-map-tail fill, and directory-vs-BAT chain-length consistency —
 * mirroring Header/Directory/BlockAlloc::CheckForErrors() and
 * GCMemcard::Open()'s corrupted-block tolerance (GCMemcard.cpp:143-270).
 * On failure, up to errcap-1 bytes of a human-readable reason are written
 * to err (if err != NULL && errcap > 0), NUL-terminated.
 * Returns 1 if valid, 0 otherwise. */
int gcn_mc_image_check(const unsigned char* buf, unsigned size_bytes, char* err, int errcap);

/* Fills out[0..min(out_cap, n)) from the active signed-update-counter
 * Directory/BAT journal pair (in directory-slot order, skipping unused/0xFF
 * entries) and returns the total
 * number of occupied entries n (which may exceed out_cap; the caller can
 * pass out_cap == 0 / out == NULL to just get the count). */
int gcn_mc_image_list(const unsigned char* buf, unsigned size_bytes,
                      gcn_mc_dirent_t* out, int out_cap);

/* Detects the save container format (.gci / .gcs / .sav, distinguished by
 * filesize % BLOCK_SIZE per GCMemcardUtils.cpp:206-228, ReadSavefile),
 * extracts the DEntry + raw block data, allocates that many free blocks
 * (first search rooted at the BAT's last-allocated block, subsequent
 * blocks rooted at prior+1 — GCMemcard.cpp:734-737,771 NextFreeBlock calls),
 * writes the block data, adds the DEntry to BOTH directory copies (blocks
 * 1 and 2) and updates the BAT map/free_blocks/last_allocated_block in BOTH
 * copies (blocks 3 and 4), and fixes all affected checksums.
 * On failure, up to errcap-1 bytes of a human-readable reason are written
 * to err (if err != NULL && errcap > 0), NUL-terminated.
 * Returns 1 on success, 0 on failure (card full, out of blocks, duplicate
 * title, or an unrecognized/corrupt save container). */
int gcn_mc_image_import_save(unsigned char* buf, unsigned size_bytes,
                              const unsigned char* save, unsigned save_len,
                              char* err, int errcap);

/* Writes buf (size_bytes bytes) to path as a flat .raw file. Returns 1 on
 * success, 0 on failure (e.g. cannot open/write path). */
int gcn_mc_image_save_file(const char* path, const unsigned char* buf, unsigned size_bytes);

#ifdef __cplusplus
}
#endif

#endif /* GCNRECOMP_MEMCARD_IMAGE_H */
