/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GameCube EXI RTC clock state. Host synchronization is a one-shot boot
 * operation; after seeding, the clock advances only from emulated cycles.
 */
#ifndef GCN_EXI_RTC_H
#define GCN_EXI_RTC_H

#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GCN_RTC_GC_EPOCH_UNIX 0x386D4380u
#define GCN_RTC_CORE_CLOCK_HZ  486000000ull

typedef struct {
    u32 counter;
    u64 anchor_cycles;
    int running;
} GcnRtc;

void gcn_rtc_set_fixed(GcnRtc* rtc, u32 counter);
void gcn_rtc_start(GcnRtc* rtc, u32 counter, u64 core_cycles);
u32 gcn_rtc_read(GcnRtc* rtc, u64 core_cycles);
void gcn_rtc_write(GcnRtc* rtc, u32 counter, u64 core_cycles);

/* Pure host-local-time conversion plus the production one-shot sampler. */
u32 gcn_rtc_from_unix_local(s64 unix_seconds, s32 utc_offset_seconds);
u32 gcn_rtc_sample_host_local(void);

#ifdef __cplusplus
}
#endif

#endif /* GCN_EXI_RTC_H */
