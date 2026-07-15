/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "exi/rtc.h"

#include <time.h>

void gcn_rtc_set_fixed(GcnRtc* rtc, u32 counter) {
    if (!rtc) return;
    rtc->counter = counter;
    rtc->anchor_cycles = 0;
    rtc->running = 0;
}

void gcn_rtc_start(GcnRtc* rtc, u32 counter, u64 core_cycles) {
    if (!rtc) return;
    rtc->counter = counter;
    rtc->anchor_cycles = core_cycles;
    rtc->running = 1;
}

u32 gcn_rtc_read(GcnRtc* rtc, u64 core_cycles) {
    if (!rtc || !rtc->running) return rtc ? rtc->counter : 0u;

    /* CPUState.cycles is monotonic. Defensively rebase if a caller restores
     * an older CPU state without restoring the RTC anchor alongside it. */
    if (core_cycles < rtc->anchor_cycles) {
        rtc->anchor_cycles = core_cycles;
        return rtc->counter;
    }

    u64 elapsed = core_cycles - rtc->anchor_cycles;
    u64 seconds = elapsed / GCN_RTC_CORE_CLOCK_HZ;
    if (seconds) {
        rtc->counter += (u32)seconds; /* real 32-bit hardware wrap semantics */
        rtc->anchor_cycles += seconds * GCN_RTC_CORE_CLOCK_HZ;
    }
    return rtc->counter;
}

void gcn_rtc_write(GcnRtc* rtc, u32 counter, u64 core_cycles) {
    if (!rtc) return;
    rtc->counter = counter;
    if (rtc->running)
        rtc->anchor_cycles = core_cycles;
}

u32 gcn_rtc_from_unix_local(s64 unix_seconds, s32 utc_offset_seconds) {
    s64 local_seconds = unix_seconds + (s64)utc_offset_seconds;
    return (u32)(local_seconds - (s64)GCN_RTC_GC_EPOCH_UNIX);
}

u32 gcn_rtc_sample_host_local(void) {
    time_t now = time(NULL);
    struct tm* local = localtime(&now);
    /* _mkgmtime(localtime(t)) - t is the current UTC offset including DST on
     * the Windows CRT used by this project. */
    time_t offset = 0;
    if (local) {
#ifdef _WIN32
        offset = _mkgmtime(local) - now;
#else
        offset = timegm(local) - now;
#endif
    }
    return gcn_rtc_from_unix_local((s64)now, (s32)offset);
}
