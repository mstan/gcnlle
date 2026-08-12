/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Native Win32 host window (opt-in, GCN_WINDOW=1): displays the emulated
 * XFB at full frame rate, replacing tools/gcn_viewer.py's TCP/PPM/Tk poll
 * loop for interactive use. See src/host_window.c for the implementation —
 * the window-thread/message-pump design, the double-buffer/publish scheme,
 * the aspect-locked resize/presentation path, and the keyboard->SI pad mapping
 * (kept identical to gcn_viewer.py).
 *
 * Default OFF: with GCN_WINDOW unset, every entry point below is a cheap
 * no-op (one cached getenv check) — no other behavior changes, so the
 * byte-exact golden gates and oracle diff are unaffected.
 */
#ifndef GCN_HOST_HOST_WINDOW_H
#define GCN_HOST_HOST_WINDOW_H

#include "cpu/cpu.h"

/* Cached (lazy -1 sentinel, same pattern as gx_raster.c's s_no_simd)
 * GCN_WINDOW=1 check. */
int gcn_host_window_enabled(void);

/* Cached (same pattern) GCN_PRESENT_STATS=1 check — opt-in VI/presenter
 * cadence counters (posts/coalesced/distinct/latency here, field ticks in
 * vi.c). Independent of GCN_WINDOW: reading it costs one getenv the first
 * call, an int compare every call after, and is false (all counters
 * inert) whenever the var is unset — see host_window.c's present path and
 * vi.c's vi_advance_halfline for the gated increments. */
int gcn_present_stats_enabled(void);

/* Snapshot of the present/VI cadence counters GCN_PRESENT_STATS=1 collects,
 * mirroring GcnHostAudioStats's role for "audio_state" (host_audio.h): a
 * flat copy for the teardown print and the TCP debug server's
 * "present_state" query. All fields are 0 when GCN_PRESENT_STATS is unset. */
typedef struct {
    bool enabled;
    u64  posts;             /* gcn_host_window_present() calls                    */
    u64  coalesced;         /* posts that overwrote a still-pending mailbox slot  */
    u64  distinct;          /* fields actually converted+presented by the window
                              * thread (do_present() reaching the draw/blit path) */
    u64  latency_samples;   /* presents with a measured post->present latency     */
    double latency_sum_ms;  /* sum of those latencies, ms (avg = sum/samples)     */
    double latency_max_ms;
} GcnPresentStats;

void gcn_host_window_get_stats(GcnPresentStats* out);

/* Print the "host_window: present stats ..." teardown summary (house style:
 * see gx_vulkan.c/host_audio.c's own one-line summaries). No-op unless
 * GCN_PRESENT_STATS=1. The window thread itself is never stopped by this —
 * it lives for the process like every other host_window state — this only
 * reads the counters, so it is safe to call once, any time after the
 * dispatch loop that drove the presents has stopped. */
void gcn_host_window_print_stats(void);

/* Spawn the window thread (registers the window class, creates the window,
 * runs the message pump) and block until it is ready (or a startup timeout
 * elapses, logged loudly). No-op if GCN_WINDOW is unset, or if already
 * started (idempotent — safe to call once from boot.c). `cpu` is accepted
 * for symmetry with gcn_gx_init's CPUState* wiring (a future direct-memory
 * reader could use it); the current design does not dereference it — all
 * guest-RAM access happens through gcn_host_window_present's `xfb` pointer,
 * copied out on the MAIN thread. Call once, before the dispatch loop runs. */
void gcn_host_window_start(CPUState* cpu);

/* Publish one XFB field to the window. MUST be called on the MAIN emulation
 * thread: it synchronously memcpys `stride*height` bytes out of guest RAM
 * into an internal buffer (guarded by an SRWLock) before returning, because
 * the guest can rewrite the XFB immediately after a VI field boundary. The
 * window thread later reads that buffer under the same lock, converts
 * YUY2->RGB, and blits — so no torn frame is ever displayed. No-op if the
 * window is not enabled/started. */
void gcn_host_window_present(const u8* xfb, u32 width, u32 height, u32 stride);

/* 1 once the window has received WM_CLOSE/WM_DESTROY (mirrors
 * gcn_debug_server_quit_requested — dispatch.c polls this to end the run
 * cleanly). 0 if the window was never enabled/started. */
int gcn_host_window_quit_requested(void);

#endif /* GCN_HOST_HOST_WINDOW_H */
