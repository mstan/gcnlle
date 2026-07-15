/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Interactive host audio for the IPL's real DSP Audio DMA stream.
 * GameCube AID supplies interleaved big-endian signed 16-bit stereo PCM.
 * The Windows implementation converts it to native PCM and feeds WASAPI;
 * headless runs remain silent unless GCN_AUDIO=1 explicitly opts in.
 */
#ifndef GCN_HOST_HOST_AUDIO_H
#define GCN_HOST_HOST_AUDIO_H

#include "cpu/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    bool device_open;
    u32  sample_rate;
    u32  peak;
    u32  buffered_frames;
    u64  frames_received;
    u64  audible_frames;
    u64  buffers_submitted;
    u64  underruns;
    u64  wait_milliseconds;
    u64  dropped_frames;
} GcnHostAudioStats;

/* Default-on with GCN_WINDOW=1; default-off for headless/oracle runs.
 * GCN_AUDIO=1 or 0 overrides that policy. Returns true when a device opened. */
bool gcn_host_audio_start(u32 sample_rate);
void gcn_host_audio_stop(void);

/* GcnDspAudioFn-compatible sink. `bytes` is normally one 32-byte AID block. */
void gcn_host_audio_submit_be_s16(void* user, const u8* samples, u32 bytes);

/* Pure GameCube-AID conversion kept public so the ROM-free unit test covers
 * the exact bytes sent to WASAPI: big-endian R/L -> little-endian L/R.
 * Input/output may not overlap. */
u32 gcn_host_audio_convert_be_s16(u8* dst, const u8* src, u32 bytes);

void gcn_host_audio_get_stats(GcnHostAudioStats* out);

#ifdef __cplusplus
}
#endif

#endif /* GCN_HOST_HOST_AUDIO_H */
