/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host/host_audio.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); ++failures; } \
    else { fprintf(stdout, "PASS: %s\n", msg); } \
} while (0)

int main(void) {
    const u8 be_pcm[] = {
        0x12, 0x34, 0xFE, 0xDC, /* R=0x1234, L=-292 */
        0x80, 0x00, 0x7F, 0xFF, /* R/L extrema */
    };
    const u8 expected_le[] = {
        0xDC, 0xFE, 0x34, 0x12,
        0xFF, 0x7F, 0x00, 0x80,
    };
    u8 out[sizeof be_pcm];
    memset(out, 0, sizeof out);

    CHECK(gcn_host_audio_convert_be_s16(out, be_pcm, sizeof be_pcm) == sizeof be_pcm,
          "conversion reports every complete s16 byte");
    CHECK(memcmp(out, expected_le, sizeof out) == 0,
          "GameCube big-endian R/L becomes native little-endian L/R PCM");
    CHECK(gcn_host_audio_convert_be_s16(out, be_pcm, 7u) == 4u,
          "conversion truncates an incomplete final stereo frame safely");
    CHECK(gcn_host_audio_convert_be_s16(NULL, be_pcm, sizeof be_pcm) == 0u,
          "conversion rejects a null destination");

    if (failures) {
        fprintf(stderr, "%d host-audio test(s) failed\n", failures);
        return 1;
    }
    fprintf(stdout, "all host-audio tests passed\n");
    return 0;
}
