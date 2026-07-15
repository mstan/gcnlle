/* SPDX-License-Identifier: GPL-3.0-only
 *
 * gcnrecomp — self-test for runtime/src/memcard_image.c.
 *
 * Exercises: format a blank 16 Mbit (0x200000-byte) card, check it valid,
 * cross-check the Directory block's closed-form checksum (0xF003 / 0,
 * GCMemcard.cpp:1316-1322) against an independently-written checksum
 * routine (not the module's own calc_checksums(), which is static/private —
 * this test re-derives the algorithm straight from the spec so a bug shared
 * between format() and check() can't hide from us), then imports the real
 * Super Smash Bros. Melee GCS save and re-validates the card.
 *
 * Returns 0 on pass, non-zero on the first failure (prints a diagnostic).
 */
#include "memcard/memcard_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

/* ---- Independent re-implementation of CalculateMemcardChecksums, written
 * straight from Dolphin's GCMemcard.cpp:326-348, deliberately NOT sharing code with
 * runtime/src/memcard_image.c's calc_checksums() — this is a cross-check,
 * not a re-test of the same function. */
static unsigned test_rd_be16(const unsigned char* p)
{
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}

static void test_wr_be16(unsigned char* p, unsigned value)
{
    p[0] = (unsigned char)(value >> 8);
    p[1] = (unsigned char)value;
}

static void test_wr_be32(unsigned char* p, unsigned value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

static void test_calc_checksums(const unsigned char* data, size_t size,
                                unsigned* csum_out, unsigned* icsum_out)
{
    unsigned csum = 0, icsum = 0;
    size_t i;
    for (i = 0; i < size; i += 2) {
        unsigned d = test_rd_be16(&data[i]);
        csum = (csum + d) & 0xFFFFu;
        icsum = (icsum + (d ^ 0xFFFFu)) & 0xFFFFu;
    }
    if (csum == 0xFFFFu) csum = 0u;
    if (icsum == 0xFFFFu) icsum = 0u;
    *csum_out = csum;
    *icsum_out = icsum;
}

static unsigned char* read_whole_file(const char* path, unsigned* size_out)
{
    FILE* f = fopen(path, "rb");
    long sz;
    unsigned char* buf;
    size_t got;

    if (!f) {
        fprintf(stderr, "cannot open '%s'\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    buf = (unsigned char*)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return NULL;
    }
    *size_out = (unsigned)sz;
    return buf;
}

int main(void)
{
    unsigned size_bytes = 0x200000u; /* 16 Mbit, per spec table: 251 usable blocks */
    unsigned char* card;
    static const unsigned char flash_id[12] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC
    };
    char err[256];
    const unsigned char* dir1;
    const unsigned char* hdr;
    const unsigned char* bat0;
    unsigned csum, icsum;
    unsigned char* save;
    unsigned save_size;
    gcn_mc_dirent_t entries[GCN_MC_DIRLEN];
    int count, i;
    int found_melee;
    unsigned char old_dir[GCN_MC_BLOCK_SIZE];
    unsigned char old_bat[GCN_MC_BLOCK_SIZE];
    unsigned char synthetic_gci[GCN_MC_DENTRY_SIZE + GCN_MC_BLOCK_SIZE];

    /* --- sizing helpers --- */
    CHECK(gcn_mc_image_valid_size(0x200000u), "0x200000 must be a valid card size");
    CHECK(!gcn_mc_image_valid_size(0x200001u), "0x200001 must NOT be a valid card size");
    CHECK(gcn_mc_image_bytes_for_mbits(16u) == 0x200000u, "16 Mbit must be 0x200000 bytes");

    card = (unsigned char*)malloc(size_bytes);
    if (!card) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    /* --- format --- */
    CHECK(gcn_mc_image_format(card, size_bytes, flash_id, 0u, 0u, 0ULL) == 1,
         "gcn_mc_image_format should succeed for a valid size");

    err[0] = '\0';
    CHECK(gcn_mc_image_check(card, size_bytes, err, (int)sizeof err) == 1,
         "freshly formatted card should check valid");
    if (err[0])
        fprintf(stderr, "  (check reported: %s)\n", err);

    /* --- known closed-form Directory checksum, GCMemcard.cpp:1316-1322 ---
     * m_checksum swap16(0xF003) / m_checksum_inv 0, at block-1 offsets
     * 0x1FFC / 0x1FFE. This is a strong cross-check independent of whatever
     * calc_checksums() the module itself uses, since the value is a fixed
     * closed-form constant, not merely "whatever the module computed". */
    dir1 = card + (size_t)GCN_MC_BLOCK_SIZE * 1;
    CHECK(test_rd_be16(dir1 + 0x1FFC) == 0xF003u,
         "blank Directory checksum must be big-endian 0xF003 at 0x1FFC");
    CHECK(test_rd_be16(dir1 + 0x1FFE) == 0x0000u,
         "blank Directory checksum_inv must be 0 at 0x1FFE");

    /* Independently recompute the Directory checksum over [0, 0x1FFC) and
     * confirm it lands on the same 0xF003/0 the module wrote. */
    test_calc_checksums(dir1 + 0, 0x1FFC, &csum, &icsum);
    CHECK(csum == 0xF003u, "independently recomputed Directory checksum must be 0xF003");
    CHECK(icsum == 0x0000u, "independently recomputed Directory checksum_inv must be 0");

    /* --- independently recompute Header checksum (GCMemcard.cpp:1275-1286,
     * sum over [0x0000, 0x01FC)) and confirm it matches the stored field. */
    hdr = card + 0;
    test_calc_checksums(hdr + 0, 0x01FCu, &csum, &icsum);
    CHECK(csum == test_rd_be16(hdr + 0x01FC), "independently recomputed Header checksum must match stored value");
    CHECK(icsum == test_rd_be16(hdr + 0x01FE), "independently recomputed Header checksum_inv must match stored value");
    CHECK(test_rd_be16(hdr + 0x0022) == 16u, "Header m_size_mb must be 16 for a 0x200000-byte card");

    /* --- independently recompute BAT checksum (GCMemcard.cpp:632-643, sum
     * over [0x0004, 0x2000)) and confirm it matches the stored field, and
     * that m_free_blocks / m_last_allocated_block match the blank-card
     * formula (251 free blocks, last_allocated_block == 4). */
    bat0 = card + (size_t)GCN_MC_BLOCK_SIZE * 3;
    test_calc_checksums(bat0 + 0x0004u, GCN_MC_BLOCK_SIZE - 0x0004u, &csum, &icsum);
    CHECK(csum == test_rd_be16(bat0 + 0x0000), "independently recomputed BAT checksum must match stored value");
    CHECK(icsum == test_rd_be16(bat0 + 0x0002), "independently recomputed BAT checksum_inv must match stored value");
    CHECK(test_rd_be16(bat0 + 0x0006) == 251u, "blank 16 Mbit card must report 251 free blocks");
    CHECK(test_rd_be16(bat0 + 0x0008) == 4u, "blank card's last_allocated_block must be 4");

    /* both directory copies and both BAT copies must be byte-identical right
     * after format (Format() memcpy's the same object to both, GCMemcard.cpp:1028-1031) */
    CHECK(memcmp(card + (size_t)GCN_MC_BLOCK_SIZE * 1, card + (size_t)GCN_MC_BLOCK_SIZE * 2,
                GCN_MC_BLOCK_SIZE) == 0,
         "directory blocks 1 and 2 must be identical after format");
    CHECK(memcmp(card + (size_t)GCN_MC_BLOCK_SIZE * 3, card + (size_t)GCN_MC_BLOCK_SIZE * 4,
                GCN_MC_BLOCK_SIZE) == 0,
         "BAT blocks 3 and 4 must be identical after format");

    /* Preserve counter-0 metadata so the import test can reproduce the IPL's
     * normal ping-pong journal: stale valid copy 0, newer valid copy 1. */
    memcpy(old_dir, card + (size_t)GCN_MC_BLOCK_SIZE * 1, GCN_MC_BLOCK_SIZE);
    memcpy(old_bat, card + (size_t)GCN_MC_BLOCK_SIZE * 3, GCN_MC_BLOCK_SIZE);

    /* --- list should be empty on a blank card --- */
    count = gcn_mc_image_list(card, size_bytes, entries, (int)(sizeof entries / sizeof entries[0]));
    CHECK(count == 0, "blank card must list zero directory entries");

    /* --- ROM-free import + journal-selection regression. Build a minimal,
     * self-authored one-block GCI in memory so the active-copy behavior is
     * always covered in CI and never depends on a copyrighted save fixture. */
    memset(synthetic_gci, 0, sizeof synthetic_gci);
    memcpy(synthetic_gci + 0x00u, "GTST", 4);
    memcpy(synthetic_gci + 0x04u, "00", 2);
    memcpy(synthetic_gci + 0x08u, "gcnrecomp-journal-test", 22);
    test_wr_be16(synthetic_gci + 0x36u, 0xFFFFu); /* replaced during import */
    test_wr_be16(synthetic_gci + 0x38u, 1u);
    test_wr_be32(synthetic_gci + 0x3Cu, 0xFFFFFFFFu); /* no comments */
    for (i = GCN_MC_DENTRY_SIZE; i < (int)sizeof synthetic_gci; ++i)
        synthetic_gci[i] = (unsigned char)(i * 37u + 11u);

    err[0] = '\0';
    CHECK(gcn_mc_image_import_save(card, size_bytes, synthetic_gci,
                                   (unsigned)sizeof synthetic_gci,
                                   err, (int)sizeof err) == 1,
          "self-authored GCI import must succeed");
    if (err[0])
        fprintf(stderr, "  (synthetic import reported: %s)\n", err);
    CHECK(gcn_mc_image_check(card, size_bytes, err, (int)sizeof err) == 1,
          "card must remain valid after self-authored import");

    /* CARD writes only the inactive journal half. Leave imported counter-1
     * metadata in copies 1 and restore counter-0 blank metadata to copies 0.
     * A checker that blindly forces pair 0 reports DIR_BAT_INCONSISTENT. */
    memcpy(card + (size_t)GCN_MC_BLOCK_SIZE * 1, old_dir, GCN_MC_BLOCK_SIZE);
    memcpy(card + (size_t)GCN_MC_BLOCK_SIZE * 3, old_bat, GCN_MC_BLOCK_SIZE);
    err[0] = '\0';
    CHECK(gcn_mc_image_check(card, size_bytes, err, (int)sizeof err) == 1,
          "IPL-style ping-pong metadata must select newer update counters");
    if (err[0])
        fprintf(stderr, "  (ping-pong check reported: %s)\n", err);

    memset(entries, 0, sizeof entries);
    count = gcn_mc_image_list(card, size_bytes, entries,
                              (int)(sizeof entries / sizeof entries[0]));
    CHECK(count == 1 && strncmp(entries[0].gamecode, "GTST", 4) == 0,
          "list must read the newer directory and BAT journal copies");
    CHECK(entries[0].block_count == 1,
          "newer BAT journal must resolve the synthetic one-block chain");

    /* Reset to a blank card before the optional external-container check. */
    CHECK(gcn_mc_image_format(card, size_bytes, flash_id, 0u, 0u, 0ULL) == 1,
          "reformat after synthetic import must succeed");
    memcpy(old_dir, card + (size_t)GCN_MC_BLOCK_SIZE * 1, GCN_MC_BLOCK_SIZE);
    memcpy(old_bat, card + (size_t)GCN_MC_BLOCK_SIZE * 3, GCN_MC_BLOCK_SIZE);

    /* --- Optional real-save import. The user supplies the fixture explicitly;
     * no save data or machine-local path is part of this repository. The
     * format/check/checksum coverage above is always self-contained. --- */
    {
        const char* gcs_path = getenv("GCN_MELEE_GCS");
        save = (gcs_path && gcs_path[0])
            ? read_whole_file(gcs_path, &save_size) : NULL;
    }
    if (!save) {
        printf("SKIP: Melee GCS save not found — import sub-test skipped "
               "(set GCN_MELEE_GCS to enable)\n");
    } else {
        err[0] = '\0';
        CHECK(gcn_mc_image_import_save(card, size_bytes, save, save_size, err, (int)sizeof err) == 1,
             "importing the Melee GCS save should succeed");
        if (err[0])
            fprintf(stderr, "  (import reported: %s)\n", err);
        free(save);

        count = gcn_mc_image_list(card, size_bytes, entries, (int)(sizeof entries / sizeof entries[0]));
        CHECK(count == 1, "card must list exactly one entry after importing one save");

        found_melee = 0;
        if (count >= 1) {
            printf("directory listing after import:\n");
            printf("%-3s %-6s %-4s %-32s %6s %6s  %s\n", "#", "code", "mkr", "filename",
                  "first", "blocks", "comment");
            for (i = 0; i < count && i < (int)(sizeof entries / sizeof entries[0]); ++i) {
                const gcn_mc_dirent_t* e = &entries[i];
                printf("%-3d %-6s %-4s %-32s %6u %6u  %s / %s\n", i, e->gamecode, e->makercode,
                      e->filename, e->first_block, e->block_count, e->comment1, e->comment2);

                if (strncmp(e->gamecode, "GALE", 4) == 0 ||
                    strstr(e->comment1, "Smash") != NULL || strstr(e->comment2, "Smash") != NULL ||
                    strstr(e->comment1, "Melee") != NULL || strstr(e->comment2, "Melee") != NULL) {
                    found_melee = 1;
                }
            }
        }
        CHECK(found_melee, "imported entry must identify as Melee (gamecode GALE* or comment "
                          "mentions Smash/Melee)");

        err[0] = '\0';
        CHECK(gcn_mc_image_check(card, size_bytes, err, (int)sizeof err) == 1,
             "card must still check valid after import (dir+BAT consistent)");
        if (err[0])
            fprintf(stderr, "  (post-import check reported: %s)\n", err);

        /* Repeat the journal check with the optional external container. */
        memcpy(card + (size_t)GCN_MC_BLOCK_SIZE * 1, old_dir, GCN_MC_BLOCK_SIZE);
        memcpy(card + (size_t)GCN_MC_BLOCK_SIZE * 3, old_bat, GCN_MC_BLOCK_SIZE);
        err[0] = '\0';
        CHECK(gcn_mc_image_check(card, size_bytes, err, (int)sizeof err) == 1,
             "IPL-style ping-pong metadata must select newer update counters");
        if (err[0])
            fprintf(stderr, "  (ping-pong check reported: %s)\n", err);

        memset(entries, 0, sizeof entries);
        count = gcn_mc_image_list(card, size_bytes, entries,
                                  (int)(sizeof entries / sizeof entries[0]));
        CHECK(count == 1 && strncmp(entries[0].gamecode, "GALE", 4) == 0,
             "list must read the newer directory and BAT journal copies");
        CHECK(strstr(entries[0].comment1, "Smash") != NULL ||
              strstr(entries[0].comment2, "Smash") != NULL,
             "list comments must follow the newer BAT chain");
    }

    free(card);

    if (g_failures == 0) {
        printf("test_memcard_image: ALL PASS\n");
        return 0;
    }
    fprintf(stderr, "test_memcard_image: %d FAILURE(S)\n", g_failures);
    return 1;
}
