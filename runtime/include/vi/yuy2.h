/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * XFB YUY2 -> RGB pixel decode, shared by every XFB display consumer (the
 * debug-server "screenshot"/"screenshot_file" command in debug_server.c and
 * the native host window in host_window.c). One algorithm, every call site
 * uses it (PRINCIPLES.md "one algorithm, every call site switches to it") —
 * this math used to live inlined in debug_server.c's screenshot case only;
 * host_window.c needs the IDENTICAL conversion (so the live window shows
 * exactly what a screenshot shows), so it moved here instead of being
 * copy-pasted a second time.
 *
 * Inverse BT.601, transcribed from Dolphin's XFB decode
 * (VideoBackends/Software/../TextureConversionShader.cpp:1009-1035): the XFB
 * is YUY2 — 4 bytes [Y0,U,Y1,V] per 2 horizontal pixels (two luma samples
 * sharing one chroma pair).
 */
#ifndef GCN_VI_YUY2_H
#define GCN_VI_YUY2_H

#include "cpu/cpu.h"

typedef struct {
    double y[256], rv[256], gv[256], gu[256], bu[256];
    int initialized;
} GcnYuy2Tables;

static inline GcnYuy2Tables* gcn_yuy2_tables(void) {
    static GcnYuy2Tables t;
    if (!t.initialized) {
        for (int i = 0; i < 256; ++i) {
            t.y[i]  = 1.164 * ((double)i - 16.0);
            t.rv[i] = 1.596 * ((double)i - 128.0);
            t.gv[i] = 0.813 * ((double)i - 128.0);
            t.gu[i] = 0.391 * ((double)i - 128.0);
            t.bu[i] = 2.018 * ((double)i - 128.0);
        }
        t.initialized = 1;
    }
    return &t;
}

static inline u8 gcn_yuy2_clamp(double c) {
    return (u8)(c < 0 ? 0 : c > 255 ? 255 : c);
}

/* Decode one pixel of a YUY2 macropixel. `pair` points at the 4-byte
 * [Y0,U,Y1,V] group for the pixel's (x/2) column; `odd` selects Y0 (x even)
 * or Y1 (x odd). Writes clamped 8-bit r/g/b and returns the source luma byte
 * (screenshot's mean_luma accumulation gets it for free, no second pass). */
static inline u8 gcn_yuy2_to_rgb(const u8* pair, int odd, u8* r, u8* g, u8* b) {
    u8 y8 = odd ? pair[2] : pair[0];
    GcnYuy2Tables* t = gcn_yuy2_tables();
    double yc = t->y[y8];
    *r = gcn_yuy2_clamp(yc + t->rv[pair[3]]);
    *g = gcn_yuy2_clamp(yc - t->gv[pair[3]] - t->gu[pair[1]]);
    *b = gcn_yuy2_clamp(yc + t->bu[pair[1]]);
    return y8;
}

/* Decode both pixels while loading their shared chroma and coefficient-table
 * entries once.  This is algebraically and byte-for-byte the same BT.601
 * formula as gcn_yuy2_to_rgb, but is the natural fast path for a live frame. */
static inline void gcn_yuy2_pair_to_rgb(const u8* pair, u8 rgb0[3], u8 rgb1[3]) {
    GcnYuy2Tables* t = gcn_yuy2_tables();
    double rv = t->rv[pair[3]], gv = t->gv[pair[3]];
    double gu = t->gu[pair[1]], bu = t->bu[pair[1]];
    double yc0 = t->y[pair[0]], yc1 = t->y[pair[2]];
    rgb0[0] = gcn_yuy2_clamp(yc0 + rv);
    rgb0[1] = gcn_yuy2_clamp(yc0 - gv - gu);
    rgb0[2] = gcn_yuy2_clamp(yc0 + bu);
    rgb1[0] = gcn_yuy2_clamp(yc1 + rv);
    rgb1[1] = gcn_yuy2_clamp(yc1 - gv - gu);
    rgb1[2] = gcn_yuy2_clamp(yc1 + bu);
}

#endif /* GCN_VI_YUY2_H */
