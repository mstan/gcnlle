/* SPDX-License-Identifier: GPL-3.0-only
 *
 * gcnrecomp — gcn_memcard: standalone provisioning CLI for Dolphin-compatible
 * GameCube memory-card .raw images. Thin wrapper over
 * runtime/src/memcard_image.c (see that file / memcard_image.h for the
 * Dolphin-oracle provenance of every on-disk offset/checksum).
 *
 * Usage:
 *   gcn_memcard format <out.raw> [--mbits 16]
 *   gcn_memcard import <card.raw> <save.gcs|.gci|.sav>
 *   gcn_memcard list   <card.raw>
 *   gcn_memcard check  <card.raw>
 */
#include "memcard/memcard_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char* read_whole_file(const char* path, unsigned* size_out)
{
    FILE* f;
    long sz;
    unsigned char* buf;
    size_t got;

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "gcn_memcard: cannot open '%s' for reading\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    buf = (unsigned char*)malloc((size_t)sz > 0 ? (size_t)sz : 1);
    if (!buf) {
        fclose(f);
        fprintf(stderr, "gcn_memcard: out of memory reading '%s'\n", path);
        return NULL;
    }

    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        fprintf(stderr, "gcn_memcard: short read on '%s'\n", path);
        free(buf);
        return NULL;
    }

    *size_out = (unsigned)sz;
    return buf;
}

static void print_usage(void)
{
    fprintf(stderr,
            "usage:\n"
            "  gcn_memcard format <out.raw> [--mbits 16]\n"
            "  gcn_memcard import <card.raw> <save.gcs|.gci|.sav>\n"
            "  gcn_memcard list   <card.raw>\n"
            "  gcn_memcard check  <card.raw>\n");
}

static int cmd_format(int argc, char** argv)
{
    const char* out_path;
    unsigned mbits = 16u; /* default: 16 Mbit = 251-block, 0x200000 bytes */
    unsigned size_bytes;
    unsigned char* buf;
    /* Fixed placeholder inputs — acceptance does not depend on these values. */
    static const unsigned char flash_id[12] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    int i;

    if (argc < 1) {
        print_usage();
        return 1;
    }
    out_path = argv[0];

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--mbits") == 0 && i + 1 < argc) {
            mbits = (unsigned)strtoul(argv[i + 1], NULL, 10);
            ++i;
        }
    }

    size_bytes = gcn_mc_image_bytes_for_mbits(mbits);
    if (!gcn_mc_image_valid_size(size_bytes)) {
        fprintf(stderr, "gcn_memcard: %u Mbit is not a valid card size "
                        "(valid: 4, 8, 16, 32, 64, 128)\n", mbits);
        return 1;
    }

    buf = (unsigned char*)malloc(size_bytes);
    if (!buf) {
        fprintf(stderr, "gcn_memcard: out of memory allocating %u bytes\n", size_bytes);
        return 1;
    }

    if (!gcn_mc_image_format(buf, size_bytes, flash_id, 0u, 0u, 0ULL)) {
        fprintf(stderr, "gcn_memcard: format failed\n");
        free(buf);
        return 1;
    }

    if (!gcn_mc_image_save_file(out_path, buf, size_bytes)) {
        fprintf(stderr, "gcn_memcard: could not write '%s'\n", out_path);
        free(buf);
        return 1;
    }

    printf("formatted %u Mbit card (%u bytes) -> %s\n", mbits, size_bytes, out_path);
    free(buf);
    return 0;
}

static int cmd_import(int argc, char** argv)
{
    const char* card_path;
    const char* save_path;
    unsigned char* card_buf;
    unsigned char* save_buf;
    unsigned card_size, save_size;
    char err[256];
    int ok;

    if (argc < 2) {
        print_usage();
        return 1;
    }
    card_path = argv[0];
    save_path = argv[1];

    card_buf = read_whole_file(card_path, &card_size);
    if (!card_buf)
        return 1;

    save_buf = read_whole_file(save_path, &save_size);
    if (!save_buf) {
        free(card_buf);
        return 1;
    }

    err[0] = '\0';
    ok = gcn_mc_image_import_save(card_buf, card_size, save_buf, save_size, err, (int)sizeof err);
    free(save_buf);
    if (!ok) {
        fprintf(stderr, "gcn_memcard: import failed: %s\n", err[0] ? err : "(unknown error)");
        free(card_buf);
        return 1;
    }

    if (!gcn_mc_image_save_file(card_path, card_buf, card_size)) {
        fprintf(stderr, "gcn_memcard: could not write back '%s'\n", card_path);
        free(card_buf);
        return 1;
    }

    printf("imported '%s' into '%s'\n", save_path, card_path);
    free(card_buf);
    return 0;
}

static int cmd_list(int argc, char** argv)
{
    const char* card_path;
    unsigned char* card_buf;
    unsigned card_size;
    gcn_mc_dirent_t entries[GCN_MC_DIRLEN];
    int count, i, shown;

    if (argc < 1) {
        print_usage();
        return 1;
    }
    card_path = argv[0];

    card_buf = read_whole_file(card_path, &card_size);
    if (!card_buf)
        return 1;

    if (!gcn_mc_image_valid_size(card_size)) {
        fprintf(stderr, "gcn_memcard: '%s' is not a valid-size card image (%u bytes)\n",
                card_path, card_size);
        free(card_buf);
        return 1;
    }

    count = gcn_mc_image_list(card_buf, card_size, entries, (int)(sizeof entries / sizeof entries[0]));
    shown = count < (int)(sizeof entries / sizeof entries[0]) ? count
                                                              : (int)(sizeof entries / sizeof entries[0]);

    printf("%-3s %-6s %-4s %-32s %6s %6s  %s\n", "#", "code", "mkr", "filename", "first", "blocks",
          "comment");
    for (i = 0; i < shown; ++i) {
        const gcn_mc_dirent_t* e = &entries[i];
        char comment[70];

        if (e->comment1[0] || e->comment2[0])
            snprintf(comment, sizeof comment, "%s / %s", e->comment1, e->comment2);
        else
            comment[0] = '\0';

        printf("%-3d %-6s %-4s %-32s %6u %6u  %s\n", i, e->gamecode, e->makercode, e->filename,
              e->first_block, e->block_count, comment);
    }
    printf("%d file(s)\n", count);

    free(card_buf);
    return 0;
}

static int cmd_check(int argc, char** argv)
{
    const char* card_path;
    unsigned char* card_buf;
    unsigned card_size;
    char err[256];
    int ok;

    if (argc < 1) {
        print_usage();
        return 1;
    }
    card_path = argv[0];

    card_buf = read_whole_file(card_path, &card_size);
    if (!card_buf)
        return 1;

    err[0] = '\0';
    ok = gcn_mc_image_check(card_buf, card_size, err, (int)sizeof err);
    free(card_buf);

    if (ok) {
        printf("%s: VALID\n", card_path);
        return 0;
    }

    printf("%s: INVALID (%s)\n", card_path, err[0] ? err : "unknown reason");
    return 1;
}

int main(int argc, char** argv)
{
    const char* cmd;

    if (argc < 2) {
        print_usage();
        return 1;
    }
    cmd = argv[1];

    if (strcmp(cmd, "format") == 0)
        return cmd_format(argc - 2, argv + 2);
    if (strcmp(cmd, "import") == 0)
        return cmd_import(argc - 2, argv + 2);
    if (strcmp(cmd, "list") == 0)
        return cmd_list(argc - 2, argv + 2);
    if (strcmp(cmd, "check") == 0)
        return cmd_check(argc - 2, argv + 2);

    fprintf(stderr, "gcn_memcard: unknown command '%s'\n", cmd);
    print_usage();
    return 1;
}
