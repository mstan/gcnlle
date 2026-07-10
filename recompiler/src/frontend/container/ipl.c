// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 gcnrecomp

#include "ipl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool ipl_load(IPLFile* ipl, const char* path, u32 base_address, u32 entry_point) {
    memset(ipl, 0, sizeof(*ipl));

    if ((base_address & 3u) != 0) {
        fprintf(stderr, "error: IPL base address 0x%08X is not instruction-aligned\n",
                base_address);
        return false;
    }
    if ((entry_point & 3u) != 0) {
        fprintf(stderr, "error: IPL entry point 0x%08X is not instruction-aligned\n",
                entry_point);
        return false;
    }

    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: can't open '%s'\n", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fprintf(stderr, "error: IPL file is empty or unreadable\n");
        fclose(f);
        return false;
    }
    if ((u64)base_address + (u64)size > 0x100000000ull) {
        fprintf(stderr, "error: IPL image at 0x%08X (%ld bytes) overflows the address space\n",
                base_address, size);
        fclose(f);
        return false;
    }

    ipl->file_data = (u8*)malloc((size_t)size);
    if (!ipl->file_data) {
        fprintf(stderr, "error: out of memory (%ld bytes)\n", size);
        fclose(f);
        return false;
    }

    if (fread(ipl->file_data, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "error: failed to read file\n");
        free(ipl->file_data);
        ipl->file_data = NULL;
        fclose(f);
        return false;
    }
    fclose(f);

    ipl->file_size = (u32)size;
    ipl->code_size = ipl->file_size & ~3u;   // whole PPC words only
    ipl->base_address = base_address;
    ipl->entry_point = entry_point;

    if (ipl->code_size == 0) {
        fprintf(stderr, "error: IPL file too small to hold a single instruction\n");
        ipl_free(ipl);
        return false;
    }
    if ((ipl->file_size & 3u) != 0) {
        fprintf(stderr,
                "warning: IPL image size 0x%X is not a multiple of 4; ignoring trailing %u byte(s)\n",
                ipl->file_size, ipl->file_size & 3u);
    }

    // The entry PC must land inside the loaded image so the dispatch table can
    // reach it. A flat blob has no section table to cross-check against, so
    // this range check is the only structural guard we have.
    if (entry_point < base_address ||
        entry_point - base_address >= ipl->code_size) {
        fprintf(stderr,
                "error: IPL entry point 0x%08X is outside the image [0x%08X, 0x%08X)\n",
                entry_point, base_address, base_address + ipl->code_size);
        ipl_free(ipl);
        return false;
    }

    return true;
}

void ipl_free(IPLFile* ipl) {
    if (ipl->file_data) {
        free(ipl->file_data);
        ipl->file_data = NULL;
    }
    ipl->file_size = 0;
    ipl->code_size = 0;
}

void ipl_print_info(const IPLFile* ipl, const char* image_name) {
    printf("=== IPL / flat image ===\n");
    if (image_name && image_name[0] != '\0')
        printf("image: %s\n", image_name);
    printf("load base:   0x%08X\n", ipl->base_address);
    printf("entry point: 0x%08X\n", ipl->entry_point);
    printf("file size:   0x%08X (%u bytes)\n", ipl->file_size, ipl->file_size);
    printf("code words:  0x%08X (%u instructions)\n",
           ipl->code_size, ipl->code_size / 4u);
    printf("addr range:  0x%08X - 0x%08X\n",
           ipl->base_address, ipl->base_address + ipl->code_size);
    printf("\n");
    printf("note: flat blob has no section table; the whole image is decoded\n");
    printf("      linearly as code. Data regions (fonts, tables, strings) that\n");
    printf("      are interleaved will be mis-decoded unless caught by the\n");
    printf("      embedded-data heuristic.\n");
}
