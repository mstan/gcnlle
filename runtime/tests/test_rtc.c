/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "exi/rtc.h"

#include <stdio.h>

static int failures;

#define CHECK(cond, msg) do {                                                \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; }       \
    else fprintf(stdout, "PASS: %s\n", msg);                               \
} while (0)

int main(void) {
    GcnRtc rtc = {0};

    CHECK(gcn_rtc_from_unix_local(GCN_RTC_GC_EPOCH_UNIX, 0) == 0u,
          "GC epoch converts to counter zero");
    CHECK(gcn_rtc_from_unix_local(GCN_RTC_GC_EPOCH_UNIX + 3600, -3600) == 0u,
          "local UTC offset is applied exactly once");

    gcn_rtc_set_fixed(&rtc, 1234u);
    CHECK(gcn_rtc_read(&rtc, 10ull * GCN_RTC_CORE_CLOCK_HZ) == 1234u,
          "deterministic fixture remains fixed");

    gcn_rtc_start(&rtc, 100u, 50u);
    CHECK(gcn_rtc_read(&rtc, 50u + GCN_RTC_CORE_CLOCK_HZ - 1u) == 100u,
          "running RTC waits for a complete emulated second");
    CHECK(gcn_rtc_read(&rtc, 50u + GCN_RTC_CORE_CLOCK_HZ) == 101u,
          "running RTC advances after one emulated second");
    CHECK(gcn_rtc_read(&rtc, 50u + 3u * GCN_RTC_CORE_CLOCK_HZ + 17u) == 103u,
          "running RTC advances multiple seconds and retains residue");

    gcn_rtc_write(&rtc, 900u, 50u + 3u * GCN_RTC_CORE_CLOCK_HZ + 17u);
    CHECK(gcn_rtc_read(&rtc, 50u + 4u * GCN_RTC_CORE_CLOCK_HZ + 16u) == 900u,
          "guest write rebases the running RTC");
    CHECK(gcn_rtc_read(&rtc, 50u + 4u * GCN_RTC_CORE_CLOCK_HZ + 17u) == 901u,
          "guest-written RTC continues advancing from emulated time");

    gcn_rtc_start(&rtc, 77u, 1000u);
    CHECK(gcn_rtc_read(&rtc, 999u) == 77u,
          "cycle regression rebases without fabricating elapsed time");

    if (failures) {
        fprintf(stderr, "%d RTC test(s) failed\n", failures);
        return 1;
    }
    fprintf(stdout, "all RTC tests passed\n");
    return 0;
}
