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

/* Decode one pixel of a YUY2 macropixel. `pair` points at the 4-byte
 * [Y0,U,Y1,V] group for the pixel's (x/2) column; `odd` selects Y0 (x even)
 * or Y1 (x odd). Writes clamped 8-bit r/g/b and returns the source luma byte
 * (screenshot's mean_luma accumulation gets it for free, no second pass). */
static inline u8 gcn_yuy2_to_rgb(const u8* pair, int odd, u8* r, u8* g, u8* b) {
    u8 y8 = odd ? pair[2] : pair[0];
    double Y = y8, U = pair[1], V = pair[3];
    double yc = 1.164 * (Y - 16.0);
    double rf = yc + 1.596 * (V - 128.0);
    double gf = yc - 0.813 * (V - 128.0) - 0.391 * (U - 128.0);
    double bf = yc + 2.018 * (U - 128.0);
    *r = (u8)(rf < 0 ? 0 : rf > 255 ? 255 : rf);
    *g = (u8)(gf < 0 ? 0 : gf > 255 ? 255 : gf);
    *b = (u8)(bf < 0 ? 0 : bf > 255 ? 255 : bf);
    return y8;
}

#endif /* GCN_VI_YUY2_H */
