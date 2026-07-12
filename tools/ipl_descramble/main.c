/*
 * ipl_descramble — offline GameCube IPL descrambler (M0 tooling).
 *
 * Reads a scrambled IPL dump, applies segher's descrambler to the documented
 * body range, and writes: (a) the full descrambled image, and/or (b) the
 * plaintext BS1+BS2 payload sliced at file offset 0x100 (oracle-corrected,
 * docs/M1_PLAN.md §1/§5 — NOT 0x820) — the M0 seed input, loaded at and
 * entered from 0x81200000 (the Dolphin-HLE BS2 landing point is 0x81200150;
 * true BS1 file offset 0x820 is not where 0x81300000 BS2 begins). The
 * IPL_BS2_* macro names/constants in descramble_core.h are the ground truth;
 * this comment mirrors them, not the other way around.
 *
 * This is the "descramble offline, recompile the plaintext" half of M0 (see
 * docs/ROADMAP.md). The faithful in-CPU/EXI BS1 descramble that M1 adds lives
 * in the RUNTIME (exi.c gcn_exi_set_rom_scrambled) — this tool's algorithm
 * (descramble_core.c) is the one vendored source both share, not duplicated.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "descramble_core.h"

static int usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s <ipl.bin> [-o <descrambled.bin>] [--bs2 <bs2.bin>]\n"
        "       %s <ipl.bin> --dump-keystream <n>   (debug: emit first n keystream bytes)\n\n"
        "  -o    write the full descrambled IPL image\n"
        "  --bs2 write the plaintext BS2 payload (file offset 0x%X..0x%X),\n"
        "        which loads at and enters from 0x%08X\n",
        argv0, argv0, IPL_BS2_FILE_OFF, IPL_SCRAMBLE_END, IPL_BS2_LOAD_ADDR);
    return 2;
}

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "error: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fprintf(stderr, "error: %s is empty\n", path); fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "error: short read on %s\n", path);
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_size = (size_t)n;
    return buf;
}

static int write_file(const char *path, const uint8_t *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "error: cannot write %s\n", path); return 0; }
    int ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    if (!ok) fprintf(stderr, "error: short write on %s\n", path);
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage(argv[0]);

    /* debug: dump keystream head (descramble a zero buffer) — used to pin the
     * algorithm in the self-test without needing a real ROM. */
    if (argc == 4 && strcmp(argv[2], "--dump-keystream") == 0) {
        int n = atoi(argv[3]);
        if (n <= 0 || n > 4096) { fprintf(stderr, "n out of range\n"); return 2; }
        uint8_t *ks = calloc((size_t)n, 1);
        ipl_descramble(ks, (size_t)n);
        for (int i = 0; i < n; i++) printf("%02x%s", ks[i], (i % 16 == 15) ? "\n" : " ");
        if (n % 16) printf("\n");
        free(ks);
        return 0;
    }

    const char *in_path = argv[1];
    const char *out_path = NULL;
    const char *bs2_path = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out_path = argv[++i];
        else if (strcmp(argv[i], "--bs2") == 0 && i + 1 < argc) bs2_path = argv[++i];
        else return usage(argv[0]);
    }
    if (!out_path && !bs2_path) return usage(argv[0]);

    size_t size = 0;
    uint8_t *buf = read_file(in_path, &size);
    if (!buf) return 1;

    if (size < IPL_SCRAMBLE_END) {
        fprintf(stderr,
            "error: %s is 0x%zX bytes; expected at least 0x%X "
            "(is this a real IPL dump?)\n", in_path, size, IPL_SCRAMBLE_END);
        free(buf);
        return 1;
    }

    ipl_descramble(buf + IPL_SCRAMBLE_START, IPL_SCRAMBLE_END - IPL_SCRAMBLE_START);
    printf("descrambled [0x%X, 0x%X) of %s\n",
           IPL_SCRAMBLE_START, IPL_SCRAMBLE_END, in_path);

    int rc = 0;
    if (out_path && !write_file(out_path, buf, size)) rc = 1;
    if (bs2_path) {
        size_t bs2_size = IPL_SCRAMBLE_END - IPL_BS2_FILE_OFF;
        if (write_file(bs2_path, buf + IPL_BS2_FILE_OFF, bs2_size)) {
            printf("wrote BS2 payload: %s (0x%zX bytes)\n", bs2_path, bs2_size);
            printf("  load address / entry PC: 0x%08X\n", IPL_BS2_LOAD_ADDR);
        } else rc = 1;
    }

    free(buf);
    return rc;
}
