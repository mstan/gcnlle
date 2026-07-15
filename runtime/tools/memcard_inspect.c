/* SPDX-License-Identifier: GPL-3.0-only
 * Deterministic validator/listing tool for acceptance checks on persisted
 * Dolphin-compatible .raw memory-card images.
 */
#include "memcard/memcard_image.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: gcn_memcard_inspect <card.raw>\n");
        return 2;
    }

    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "cannot open: %s\n", argv[1]);
        return 2;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 2; }
    long size_long = ftell(f);
    if (size_long <= 0 || (unsigned long)size_long > 0xFFFFFFFFul ||
        fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 2; }
    unsigned size = (unsigned)size_long;
    unsigned char* data = (unsigned char*)malloc(size);
    if (!data) { fclose(f); return 2; }
    size_t got = fread(data, 1, size, f);
    fclose(f);
    if (got != size) { free(data); return 2; }

    char err[256] = {0};
    int valid = gcn_mc_image_check(data, size, err, (int)sizeof err);
    gcn_mc_dirent_t entries[GCN_MC_DIRLEN];
    int count = valid ? gcn_mc_image_list(data, size, entries, GCN_MC_DIRLEN) : -1;

    printf("path=%s\nsize=%u\nvalid=%d\nentries=%d\n", argv[1], size, valid, count);
    if (!valid)
        printf("error=%s\n", err);
    for (int i = 0; i < count && i < (int)GCN_MC_DIRLEN; i++) {
        printf("entry[%d].gamecode=%s\n", i, entries[i].gamecode);
        printf("entry[%d].makercode=%s\n", i, entries[i].makercode);
        printf("entry[%d].filename=%s\n", i, entries[i].filename);
        printf("entry[%d].comment1=%s\n", i, entries[i].comment1);
        printf("entry[%d].comment2=%s\n", i, entries[i].comment2);
        printf("entry[%d].first_block=%u\n", i, entries[i].first_block);
        printf("entry[%d].block_count=%u\n", i, entries[i].block_count);
    }

    free(data);
    return valid ? 0 : 1;
}
