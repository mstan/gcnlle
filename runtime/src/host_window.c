/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Native Win32 host window (impl). See include/host/host_window.h for scope.
 *
 * Threading model:
 *   - The MAIN emulation thread calls gcn_host_window_present() once per VI
 *     field boundary (hooked from vi.c). It memcpys the guest XFB bytes into
 *     a shared buffer under an SRWLock, then PostMessage()s the window
 *     thread — it never blocks on the window thread and never touches
 *     Win32 UI state itself.
 *   - A dedicated WINDOW thread (created once by gcn_host_window_start,
 *     mirrors gx_raster.c's GX-MT worker threads: CreateThread, never
 *     joined, lives for the process) owns the window class, the HWND, and
 *     the message pump. On WM_GCN_PRESENT it copies the shared buffer out
 *     under the SAME lock into a thread-local scratch buffer (fast memcpy,
 *     lock held only that long), then — lock released — does the slow part
 *     (YUY2->RGB conversion, resize-if-geometry-changed, StretchDIBits
 *     blit) entirely off the lock, so the main emulation thread is never
 *     stalled by the ~ms of conversion+blit work.
 *
 * No torn frames: the shared buffer is always a complete, self-consistent
 * field (present() holds the lock for the whole memcpy; the window thread's
 * copy-out holds the same lock for the whole memcpy) — never a mix of two
 * fields' bytes.
 *
 * YUY2->RGB conversion is gcn_yuy2_to_rgb (include/vi/yuy2.h) — the exact
 * inverse-BT.601 math debug_server.c's "screenshot" command uses, so the
 * window shows exactly what a screenshot shows.
 *
 * Keyboard -> SI pad mapping is transcribed to be IDENTICAL to
 * tools/gcn_viewer.py's Viewer.apply_key (see that file's BUTTON_KEYS /
 * STICK_KEYS / CSTICK_KEYS tables): a stick-axis key press sets that axis to
 * the offset value; release resets it to center (0x80) — not a
 * left/right-cancel model, deliberately matching the Python client so the
 * two input surfaces feel identical to a player switching between them.
 *
 * Presentation backend: the converted s_rgb (0x00RRGGBB) buffer is uploaded
 * to a GL texture and committed with a true double-buffered SwapBuffers (WGL,
 * fixed-function GL 1.1). This is the default because redirected GDI window
 * surfaces can be sampled by DWM halfway through a blit, producing visible
 * half/quarter-surface tears. GCN_GL=0 or any WGL setup failure selects the
 * retained GDI fallback. See gl_init(), gl_upload_texture(), gl_draw_quad().
 */
#include "host/host_window.h"
#include "si/si.h"
#include "vi/yuy2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>     /* CreateThread, window class/message pump, SRWLock,
                          * StretchDIBits — same include style as gx_raster.c's
                          * GX-MT worker pool. */
#include <GL/gl.h>       /* Default WGL present path (GCN_GL != 0) —
                          * fixed-function GL 1.1 only: no extension loader,
                          * no core profile. wgl* entry points come from
                          * wingdi.h (pulled in by windows.h regardless of
                          * WIN32_LEAN_AND_MEAN). Linked via opengl32
                          * (CMakeLists.txt). */

/* Custom message posted by the main thread's present() to wake the window
 * thread's pump; WM_APP+N is the documented private range (winuser.h). */
#define WM_GCN_PRESENT (WM_APP + 1)

/* GC pad button bits (si.h GCN_SI_PAD_*), duplicated here as plain integer
 * literals rather than pulling every si.h macro in by name — matches
 * gcn_viewer.py's own BUTTON_KEYS table 1:1 (see that file's header comment
 * for the canonical source: runtime/include/si/si.h). */
#define PAD_LEFT   0x0001u
#define PAD_RIGHT  0x0002u
#define PAD_DOWN   0x0004u
#define PAD_UP     0x0008u
#define PAD_Z      0x0010u
#define PAD_R      0x0020u
#define PAD_L      0x0040u
#define PAD_A      0x0100u
#define PAD_B      0x0200u
#define PAD_X      0x0400u
#define PAD_Y      0x0800u
#define PAD_START  0x1000u

/* ---- GCN_WINDOW=1 cached check (lazy -1 sentinel, gx_raster.c's
 * s_no_simd pattern) ---- */
int gcn_host_window_enabled(void) {
    static int s_enabled = -1;
    if (s_enabled < 0) {
        const char* e = getenv("GCN_WINDOW");
        s_enabled = (e && *e && *e != '0') ? 1 : 0;
    }
    return s_enabled;
}

/* ---- GCN_PRESENT_STATS=1 cached check (same lazy -1 sentinel pattern) ---- */
int gcn_present_stats_enabled(void) {
    static int s_enabled = -1;
    if (s_enabled < 0) {
        const char* e = getenv("GCN_PRESENT_STATS");
        s_enabled = (e && *e && *e != '0') ? 1 : 0;
    }
    return s_enabled;
}

/* ---- opt-in present/VI cadence stats (GCN_PRESENT_STATS=1) ----
 * Counts distinct presents, coalesced posts, and post->present latency for
 * the mailbox above. Zero cost when unset beyond the cached getenv check
 * above: no new locks anywhere in the present path, and every
 * counter here is a plain (non-atomic) u64 touched only by its OWNING
 * thread — posts/coalesced by the main emulation thread inside
 * gcn_host_window_present, distinct/latency by the window thread inside
 * do_present. gcn_host_window_get_stats() below does a lock-free struct
 * copy for the teardown print and the TCP debug server's "present_state"
 * query; a racy read of in-progress stats counters is acceptable (this is
 * observability, never guest-visible state) the same way host_audio.c's
 * GcnHostAudioStats is read without a lock. */
static u64    s_stat_posts            = 0;
static u64    s_stat_coalesced        = 0;
static u64    s_stat_distinct         = 0;
static u64    s_stat_latency_samples  = 0;
static double s_stat_latency_sum_ms   = 0.0;
static double s_stat_latency_max_ms   = 0.0;

/* Latest-post wall-clock timestamp: written by the main thread on every
 * post, read by the window thread once per do_present() to measure
 * post->present latency. A single 64-bit slot with no lock — if a second
 * post lands between the write and the read, the measurement is against
 * whichever post's timestamp is newest, which is exactly the frame
 * do_present() is about to draw (the mailbox already coalesces to "latest
 * frame wins"), so this is race-TOLERANT, not merely race-ignored. */
static volatile LONGLONG s_stat_last_post_qpc = 0;

static double stat_qpc_freq_hz(void) {
    static LARGE_INTEGER s_freq;
    static int s_ready = 0;
    if (!s_ready) {
        QueryPerformanceFrequency(&s_freq);
        s_ready = 1;
    }
    return (double)s_freq.QuadPart;
}

void gcn_host_window_get_stats(GcnPresentStats* out) {
    if (!out) return;
    out->enabled = gcn_present_stats_enabled() ? true : false;
    out->posts = s_stat_posts;
    out->coalesced = s_stat_coalesced;
    out->distinct = s_stat_distinct;
    out->latency_samples = s_stat_latency_samples;
    out->latency_sum_ms = s_stat_latency_sum_ms;
    out->latency_max_ms = s_stat_latency_max_ms;
}

void gcn_host_window_print_stats(void) {
    if (!gcn_present_stats_enabled()) return;
    double avg_ms = s_stat_latency_samples
        ? s_stat_latency_sum_ms / (double)s_stat_latency_samples : 0.0;
    fprintf(stderr,
            "host_window: present stats posts=%llu distinct=%llu coalesced=%llu "
            "latency avg=%.2fms max=%.2fms\n",
            (unsigned long long)s_stat_posts,
            (unsigned long long)s_stat_distinct,
            (unsigned long long)s_stat_coalesced,
            avg_ms, s_stat_latency_max_ms);
}

/* ---- shared publish buffer (main thread writes, window thread reads) ---- */
static SRWLOCK s_lock = SRWLOCK_INIT;
static u8*  s_shared_data  = NULL;
static u32  s_shared_cap   = 0;      /* bytes allocated                    */
static u32  s_shared_w     = 0;
static u32  s_shared_h     = 0;
static u32  s_shared_stride= 0;
static int  s_shared_valid = 0;
static volatile LONG s_present_posted = 0; /* latest-frame mailbox wake bit */

/* ---- window-thread-owned state ---- */
static HWND     s_hwnd = NULL;
static CPUState* s_cpu = NULL;          /* see host_window.h doc: unused today */
static volatile LONG s_quit = 0;        /* 1 once WM_DESTROY has been seen     */

static u8*  s_scratch     = NULL;       /* window-thread copy-out of the shared frame */
static u32  s_scratch_cap = 0;
static u32* s_rgb         = NULL;       /* converted DIB pixel buffer (0x00RRGGBB)    */
static u32  s_rgb_cap     = 0;          /* pixels allocated                            */
static u32  s_rgb_w = 0, s_rgb_h = 0;
static int  s_have_frame  = 0;
static u32  s_last_w = 0, s_last_h = 0; /* geometry the window was last sized for      */
static u32  s_display_aspect_num = 4, s_display_aspect_den = 3;
static HDC      s_gdi_mem_dc = NULL;    /* persistent off-screen GDI front buffer       */
static HBITMAP  s_gdi_bitmap = NULL;
static HGDIOBJ  s_gdi_old_bitmap = NULL;
static void*    s_gdi_bits = NULL;
static u32      s_gdi_w = 0, s_gdi_h = 0;

/* ---- injected pad state (mirrors gcn_viewer.py's Viewer.buttons/stick/cstick) ---- */
static u16 s_buttons  = 0;
static u8  s_stick[2]  = { 0x80, 0x80 };
static u8  s_cstick[2] = { 0x80, 0x80 };
static int s_key_down[256];   /* pressed-set, indexed by virtual-key code — debounces
                                * Windows key auto-repeat alongside the WM_KEYDOWN
                                * lParam bit-30 check in handle_key() below. */

/* Push the current pad state to the SI model (mirrors gcn_viewer.py's
 * send_input: triggers are derived from the L/R button bits, not tracked
 * separately — a held Q/E reports the trigger fully pressed (255)). */
static void send_input(void) {
    gcn_si_debug_set_input(
        1, s_buttons,
        1, s_stick[0], 1, s_stick[1],
        1, s_cstick[0], 1, s_cstick[1],
        1, (s_buttons & PAD_L) ? 255u : 0u,
        1, (s_buttons & PAD_R) ? 255u : 0u,
        0 /* reset */);
}

/* Apply one key transition. `down`: 1 = WM_KEYDOWN (first press, not a
 * repeat), 0 = WM_KEYUP. Mirrors gcn_viewer.py's apply_key: a button key
 * ORs/ANDs its bit into s_buttons; a stick key sets its one axis to the
 * offset value on press and back to center (0x80) on release — release
 * always recenters that axis, it does not check whether an opposing key on
 * the same axis is still held (identical simplification to the Python
 * client, so both input surfaces behave the same way to a player). */
static void apply_key(int vk, int down) {
    int changed = 0;
    u16 bit = 0;
    switch (vk) {
    case 'X': bit = PAD_A; break;
    case 'Z': bit = PAD_B; break;
    case 'C': bit = PAD_X; break;
    case 'V': bit = PAD_Y; break;
    case VK_RETURN: case VK_SPACE: bit = PAD_START; break;
    case 'Q': bit = PAD_L; break;
    case 'E': bit = PAD_R; break;
    case 'W': bit = PAD_UP; break;
    case 'S': bit = PAD_DOWN; break;
    case 'A': bit = PAD_LEFT; break;
    case 'D': bit = PAD_RIGHT; break;
    default: break;
    }
    if (bit) {
        u16 nb = down ? (u16)(s_buttons | bit) : (u16)(s_buttons & ~bit);
        changed |= (nb != s_buttons);
        s_buttons = nb;
    }

    switch (vk) {
    case VK_UP:    s_stick[1]  = down ? (u8)(0x80 + 127) : 0x80u; changed = 1; break;
    case VK_DOWN:  s_stick[1]  = down ? (u8)(0x80 - 127) : 0x80u; changed = 1; break;
    case VK_LEFT:  s_stick[0]  = down ? (u8)(0x80 - 127) : 0x80u; changed = 1; break;
    case VK_RIGHT: s_stick[0]  = down ? (u8)(0x80 + 127) : 0x80u; changed = 1; break;
    case 'I':      s_cstick[1] = down ? (u8)(0x80 + 127) : 0x80u; changed = 1; break;
    case 'K':      s_cstick[1] = down ? (u8)(0x80 - 127) : 0x80u; changed = 1; break;
    case 'J':      s_cstick[0] = down ? (u8)(0x80 - 127) : 0x80u; changed = 1; break;
    case 'L':      s_cstick[0] = down ? (u8)(0x80 + 127) : 0x80u; changed = 1; break;
    default: break;
    }

    if (changed)
        send_input();
}

/* WM_KEYDOWN/WM_KEYUP handler. `repeat_bit` is lParam bit 30 (1 = this
 * WM_KEYDOWN is an OS auto-repeat of a key already down) — combined with the
 * s_key_down[] pressed-set so a held key neither re-triggers apply_key() nor
 * (on the eventual WM_KEYUP) gets treated as a release of a key we never
 * saw a fresh press for. */
static void handle_key(WPARAM wp, LPARAM lp, int down) {
    int vk = (int)wp;
    if (vk < 0 || vk > 255) return;
    if (down) {
        int repeat = (lp >> 30) & 1;
        if (repeat || s_key_down[vk]) return;   /* auto-repeat: ignore */
        s_key_down[vk] = 1;
    } else {
        if (!s_key_down[vk]) return;            /* release of a key we never tracked as down */
        s_key_down[vk] = 0;
    }
    apply_key(vk, down);
}

/* ---- OpenGL presentation path (default; GCN_GL=0 opts out) ----
 * Alternate present path alongside the GDI/StretchDIBits one above: fixed-
 * function GL 1.1 (no extension loader, no core profile) textured-quad blit
 * of the same s_rgb buffer produced below, then SwapBuffers. Falls back to
 * the (byte-identical) GDI path if GCN_GL=0 or any WGL setup step fails —
 * see gl_init(). */
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367  /* GL 1.2 core; absent from mingw's GL 1.1 gl.h */
#endif

typedef BOOL (WINAPI *gcn_wglSwapIntervalEXT_t)(int);

static HDC    s_gl_hdc = NULL;
static HGLRC  s_gl_rc  = NULL;
static int    s_gl_active = 0;        /* 1 once a WGL context is current for this window */
static GLuint s_gl_tex = 0;
static u32    s_gl_tex_src_w = 0, s_gl_tex_src_h = 0; /* source dims last uploaded — detects
                                                        * an XFB geometry change needing a
                                                        * fresh glTexImage2D allocation      */
static float  s_gl_tex_u = 1.0f, s_gl_tex_v = 1.0f;   /* UV scale: src / pow2 (pow2 texture
                                                        * storage has unused padding)         */

/* Default-on cached check (same lazy-static pattern as
 * gcn_host_window_enabled above). GCN_GL=0 retains the diagnostic GDI
 * fallback; any other value, including unset, uses atomic WGL swaps. */
static int gcn_gl_enabled(void) {
    static int s_enabled = -1;
    if (s_enabled < 0) {
        const char* e = getenv("GCN_GL");
        s_enabled = (!e || !*e || *e != '0') ? 1 : 0;
    }
    return s_enabled;
}

/* GCN_GL_FILTER=nearest selects GL_NEAREST; anything else (including unset)
 * keeps the default GL_LINEAR. */
static GLint gcn_gl_filter(void) {
    static GLint s_filter = 0;
    if (!s_filter) {
        const char* e = getenv("GCN_GL_FILTER");
        s_filter = (e && strcmp(e, "nearest") == 0) ? GL_NEAREST : GL_LINEAR;
    }
    return s_filter;
}

/* Vsync defaults on; GCN_GL_VSYNC=0 requests wglSwapIntervalEXT(0). Vsync
 * (if the driver exposes it — see gl_init()) only ever blocks THIS window
 * thread's SwapBuffers call, never the emu thread: GCN_THROTTLE's own
 * Sleep-based pacing (dispatch.c) is what paces the emu thread. */
static int gcn_gl_vsync_wanted(void) {
    const char* e = getenv("GCN_GL_VSYNC");
    return (!e || !*e || *e != '0') ? 1 : 0;
}

static u32 next_pow2_u32(u32 v) {
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return v + 1;
}

/* Create a WGL context on `hwnd`, fixed-function GL 1.1 only. Runs on the
 * WINDOW thread, once, right after the window is created/shown and before
 * the message pump starts (window_thread_proc below). On success sets
 * s_gl_active=1; on any failure logs one line naming the failing step and
 * leaves s_gl_active=0, so callers fall back to the untouched GDI path. */
static void gl_init(HWND hwnd) {
    if (!gcn_gl_enabled()) return;

    HDC hdc = GetDC(hwnd);
    if (!hdc) {
        fprintf(stderr, "host_window: GL init failed (GetDC), falling back to GDI\n");
        return;
    }

    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize      = sizeof pfd;
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pf = ChoosePixelFormat(hdc, &pfd);
    if (!pf) {
        fprintf(stderr, "host_window: GL init failed (ChoosePixelFormat), falling back to GDI\n");
        ReleaseDC(hwnd, hdc);
        return;
    }
    if (!SetPixelFormat(hdc, pf, &pfd)) {
        fprintf(stderr, "host_window: GL init failed (SetPixelFormat), falling back to GDI\n");
        ReleaseDC(hwnd, hdc);
        return;
    }

    HGLRC rc = wglCreateContext(hdc);
    if (!rc) {
        fprintf(stderr, "host_window: GL init failed (wglCreateContext), falling back to GDI\n");
        ReleaseDC(hwnd, hdc);
        return;
    }
    if (!wglMakeCurrent(hdc, rc)) {
        fprintf(stderr, "host_window: GL init failed (wglMakeCurrent), falling back to GDI\n");
        wglDeleteContext(rc);
        ReleaseDC(hwnd, hdc);
        return;
    }

    s_gl_hdc = hdc;
    s_gl_rc  = rc;
    s_gl_active = 1;

    /* Looked up, not assumed: GL 1.1 has no vsync control of its own. If the
     * driver doesn't expose the extension, GCN_GL_VSYNC is silently a no-op
     * rather than a fallback — vsync is a nicety, not a requirement for the
     * GL path to work. */
    gcn_wglSwapIntervalEXT_t swap_interval =
        (gcn_wglSwapIntervalEXT_t)wglGetProcAddress("wglSwapIntervalEXT");
    int vsync = gcn_gl_vsync_wanted();
    int swap_interval_set = swap_interval ? swap_interval(vsync ? 1 : 0) : 0;

    fprintf(stderr,
            "host_window: WGL presenter active (double-buffered, vsync=%s%s)\n",
            vsync ? "on" : "off",
            swap_interval ? (swap_interval_set ? "" : ", driver rejected interval")
                          : ", swap-control unavailable");

    glEnable(GL_TEXTURE_2D);
}

/* Center the image without changing the display aspect selected when the
 * current XFB geometry arrived. WM_SIZING normally keeps the client at this
 * ratio; the letterbox/pillarbox fallback also covers maximize and programmatic
 * resizes, which do not pass through WM_SIZING. */
static void compute_dest_rect(int client_w, int client_h, u32 src_w, u32 src_h, RECT* out) {
    u32 an = s_display_aspect_num ? s_display_aspect_num : src_w;
    u32 ad = s_display_aspect_den ? s_display_aspect_den : src_h;
    int dw = client_w, dh = client_h;
    if (an && ad && (u64)client_w * ad > (u64)client_h * an)
        dw = (int)((u64)client_h * an / ad);
    else if (an && ad)
        dh = (int)((u64)client_w * ad / an);
    out->left = (client_w - dw) / 2;
    out->top = (client_h - dh) / 2;
    out->right = out->left + dw;
    out->bottom = out->top + dh;
}

/* Maintain the current display aspect while the user drags any window edge.
 * The proposed WM_SIZING rectangle is an OUTER window rectangle, so account
 * for caption/borders before applying the client-area ratio. */
static void constrain_sizing_rect(HWND hwnd, WPARAM edge, RECT* r) {
    u32 an = s_display_aspect_num, ad = s_display_aspect_den;
    if (!an || !ad || !r) return;

    DWORD style = (DWORD)GetWindowLongPtrA(hwnd, GWL_STYLE);
    DWORD exstyle = (DWORD)GetWindowLongPtrA(hwnd, GWL_EXSTYLE);
    RECT frame = { 0, 0, 0, 0 };
    AdjustWindowRectEx(&frame, style, FALSE, exstyle);
    LONG frame_w = frame.right - frame.left;
    LONG frame_h = frame.bottom - frame.top;
    LONG cw = (r->right - r->left) - frame_w;
    LONG ch = (r->bottom - r->top) - frame_h;
    if (cw < 1 || ch < 1) return;

    LONG h_from_w = (LONG)((u64)cw * ad / an);
    LONG w_from_h = (LONG)((u64)ch * an / ad);
    int adjust_width;
    if (edge == WMSZ_LEFT || edge == WMSZ_RIGHT)
        adjust_width = 0;
    else if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM)
        adjust_width = 1;
    else
        adjust_width = labs(w_from_h - cw) <= labs(h_from_w - ch);

    if (adjust_width) {
        LONG outer_w = w_from_h + frame_w;
        if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
            r->left = r->right - outer_w;
        else
            r->right = r->left + outer_w;
    } else {
        LONG outer_h = h_from_w + frame_h;
        if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
            r->top = r->bottom - outer_h;
        else
            r->bottom = r->top + outer_h;
    }
}

/* Upload s_rgb (0x00RRGGBB per pixel — the same buffer StretchDIBits reads)
 * into s_gl_tex. Texture storage is allocated at the next power-of-two >=
 * the source dims (kept for broad GPU compatibility rather than assuming
 * NPOT support) and only reallocated (glTexImage2D) when the XFB geometry
 * changes; the common case is a glTexSubImage2D into existing storage.
 * GL_BGRA_EXT + GL_UNSIGNED_INT_8_8_8_8_REV read the existing 0x00RRGGBB
 * DWORDs directly with no per-pixel repacking. */
static void gl_upload_texture(const u32* rgb, u32 w, u32 h) {
    if (!s_gl_tex)
        glGenTextures(1, &s_gl_tex);
    glBindTexture(GL_TEXTURE_2D, s_gl_tex);

    if (w != s_gl_tex_src_w || h != s_gl_tex_src_h) {
        u32 pw = next_pow2_u32(w), ph = next_pow2_u32(h);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)pw, (GLsizei)ph, 0,
                     GL_BGRA_EXT, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
        GLint filter = gcn_gl_filter();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        s_gl_tex_src_w = w; s_gl_tex_src_h = h;
        s_gl_tex_u = (float)w / (float)pw;
        s_gl_tex_v = (float)h / (float)ph;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h,
                    GL_BGRA_EXT, GL_UNSIGNED_INT_8_8_8_8_REV, rgb);
}

/* Draw the currently-uploaded texture as a quad covering compute_dest_rect()
 * and SwapBuffers. Pixel-space glOrtho (y growing downward, origin top-left)
 * matches both Win32 client coordinates and the top-down DIB row order the
 * YUY2->RGB conversion below produces, so texcoord v=0 (the first uploaded
 * row) lands at the top of the window with no separate flip step. Safe to
 * call without a fresh upload (WM_PAINT expose/resize redraw of the last
 * frame) as long as gl_upload_texture() has run at least once. */
static void gl_draw_quad(HWND hwnd, u32 src_w, u32 src_h) {
    RECT rc; GetClientRect(hwnd, &rc);
    int cw = (int)(rc.right - rc.left), ch = (int)(rc.bottom - rc.top);
    if (cw <= 0 || ch <= 0) return;

    glViewport(0, 0, cw, ch);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, (double)cw, (double)ch, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    RECT dst;
    compute_dest_rect(cw, ch, src_w, src_h, &dst);

    glBindTexture(GL_TEXTURE_2D, s_gl_tex);
    glBegin(GL_QUADS);
        glTexCoord2f(0.0f,       0.0f);       glVertex2i(dst.left,  dst.top);
        glTexCoord2f(s_gl_tex_u, 0.0f);       glVertex2i(dst.right, dst.top);
        glTexCoord2f(s_gl_tex_u, s_gl_tex_v); glVertex2i(dst.right, dst.bottom);
        glTexCoord2f(0.0f,       s_gl_tex_v); glVertex2i(dst.left,  dst.bottom);
    glEnd();

    /* Some desktop-capture/DWM paths can sample the newly swapped surface
     * before an asynchronously queued texture upload + quad has finished.
     * That exposes a one-frame clear/partial quad even though the context is
     * double-buffered.  Complete this tiny presentation workload before the
     * atomic swap; this blocks only the coalescing window thread. */
    glFinish();
    SwapBuffers(s_gl_hdc);
}

/* Black-screen equivalent of the GDI path's FillRect(BLACK_BRUSH) fallback —
 * used for the window between gl_init() succeeding and the first frame
 * landing (s_gl_tex not yet allocated). */
static void gl_draw_black(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    glViewport(0, 0, (int)(rc.right - rc.left), (int)(rc.bottom - rc.top));
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    SwapBuffers(s_gl_hdc);
}

/* Copy the shared frame out under the lock, resize the window if its
 * geometry changed, convert YUY2->RGB, and present it. Runs on the window
 * thread only. */
static void do_present(HWND hwnd) {
    u32 w, h, stride;

    AcquireSRWLockShared(&s_lock);
    if (!s_shared_valid) { ReleaseSRWLockShared(&s_lock); return; }
    w = s_shared_w; h = s_shared_h; stride = s_shared_stride;
    size_t need = (size_t)stride * (size_t)h;
    if (need > s_scratch_cap) {
        u8* n = (u8*)realloc(s_scratch, need);
        if (!n) { ReleaseSRWLockShared(&s_lock); return; }
        s_scratch = n; s_scratch_cap = (u32)need;
    }
    memcpy(s_scratch, s_shared_data, need);
    ReleaseSRWLockShared(&s_lock);

    if (w == 0 || h == 0) return;

    /* Resize-on-geometry-change: GDI defaults to a native-width client. A
     * half-height VI field (e.g. 592x224) is expanded to 592x448 by row
     * duplication below, allowing a 1:1 DIB copy instead of a per-frame GPU
     * stretch that can serialize with Vulkan. The GL path keeps its
     * 2x/4x client because its texture path owns scaling.
     *
     * [ENHANCEMENT, opt-in] GCN_ASPECT=16:9 / 21:9 additionally widens the
     * client by target/(4:3) — 4/3 or 7/4 — pairing with gx_raster.c's
     * perspective-projection X-scale (gx_aspect_xscale) so 3D content shows
     * a genuinely wider FOV at the correct proportions; 2D/ortho overlays
     * take the classic Dolphin-widescreen stretch. Unset = exact old rule. */
    if (w != s_last_w || h != s_last_h) {
        u32 cw, ch;
        if (s_gl_active) {
            if (h * 2u <= w) { cw = w * 2u; ch = h * 4u; }
            else             { cw = w * 2u; ch = h * 2u; }
        } else {
            cw = w;
            ch = h * ((h * 2u <= w) ? 2u : 1u);
        }
        {
            static int s_aspect = -1;   /* -1 unresolved, 0 off, 1 = 16:9, 2 = 21:9 */
            if (s_aspect < 0) {
                const char* e = getenv("GCN_ASPECT");
                s_aspect = (e && strcmp(e, "16:9") == 0) ? 1
                         : (e && strcmp(e, "21:9") == 0) ? 2 : 0;
            }
            if (s_aspect == 1)      cw = cw * 4u / 3u;
            else if (s_aspect == 2) cw = cw * 7u / 4u;
        }
        s_display_aspect_num = cw;
        s_display_aspect_den = ch;
        RECT rc = { 0, 0, (LONG)cw, (LONG)ch };
        DWORD style   = (DWORD)GetWindowLongPtrA(hwnd, GWL_STYLE);
        DWORD exstyle = (DWORD)GetWindowLongPtrA(hwnd, GWL_EXSTYLE);
        AdjustWindowRectEx(&rc, style, FALSE, exstyle);
        SetWindowPos(hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        s_last_w = w; s_last_h = h;
    }

    u32 row_repeat = (!s_gl_active && h * 2u <= w) ? 2u : 1u;
    u32 rgb_h = h * row_repeat;
    u64 npx = (u64)w * (u64)rgb_h;
    if (npx > s_rgb_cap) {
        u32* n = (u32*)realloc(s_rgb, (size_t)npx * sizeof(u32));
        if (!n) return;
        s_rgb = n; s_rgb_cap = (u32)npx;
    }

    /* YUY2 -> RGB, gcn_yuy2_to_rgb (vi/yuy2.h) — byte-identical to
     * debug_server.c's screenshot conversion. DIB 32bpp BI_RGB stores bytes
     * B,G,R,pad per pixel in memory, i.e. a little-endian read of the DWORD
     * is 0x00RRGGBB — so packing (R<<16)|(G<<8)|B here is correct. */
    for (u32 y = 0; y < h; y++) {
        const u8* row = s_scratch + (size_t)y * stride;
        u32* out = s_rgb + (size_t)y * row_repeat * w;
        u32 x = 0;
        for (; x + 1u < w; x += 2u) {
            u8 rgb0[3], rgb1[3];
            gcn_yuy2_pair_to_rgb(row + (size_t)x * 2u, rgb0, rgb1);
            out[x] = ((u32)rgb0[0] << 16) | ((u32)rgb0[1] << 8) | rgb0[2];
            out[x + 1u] = ((u32)rgb1[0] << 16) |
                          ((u32)rgb1[1] << 8) | rgb1[2];
        }
        if (x < w) {
            u8 r, g, b;
            gcn_yuy2_to_rgb(row + (size_t)(x / 2u) * 4u, 0, &r, &g, &b);
            out[x] = ((u32)r << 16) | ((u32)g << 8) | b;
        }
        if (row_repeat == 2u)
            memcpy(out + w, out, (size_t)w * sizeof *out);
    }
    s_rgb_w = w; s_rgb_h = rgb_h; s_have_frame = 1;

    if (s_gl_active) {
        /* Draw immediately rather than InvalidateRect->WM_PAINT: every field
         * reaching here is already a fresh, complete frame, so deferring
         * through the message queue buys nothing. WM_PAINT still handles GL
         * (redraws this same texture) for expose/resize — see wnd_proc. */
        gl_upload_texture(s_rgb, w, h);
        gl_draw_quad(hwnd, w, h);
    } else {
        InvalidateRect(hwnd, NULL, FALSE);
    }

    /* GCN_PRESENT_STATS=1: this is the "distinct present" site — reached
     * once per WM_GCN_PRESENT that carried an actual field (not the
     * WM_PAINT expose/resize redraw path above do_present, which never
     * calls this function). For the GL path the SwapBuffers inside
     * gl_draw_quad() has already committed by the time we get here, so the
     * latency below is genuinely post->swap-complete; the GDI path has no
     * swapchain (see gdi_wait_for_compositor's doc comment) and only
     * schedules the blit via InvalidateRect, so its latency is post->
     * repaint-scheduled, a slight underestimate of true post->blit. */
    if (gcn_present_stats_enabled()) {
        s_stat_distinct++;
        LONGLONG post_qpc = s_stat_last_post_qpc;
        if (post_qpc) {
            double freq = stat_qpc_freq_hz();
            if (freq > 0.0) {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                double ms = 1000.0 * (double)(now.QuadPart - post_qpc) / freq;
                if (ms < 0.0) ms = 0.0;   /* defensive only; QPC is monotonic */
                s_stat_latency_sum_ms += ms;
                if (ms > s_stat_latency_max_ms) s_stat_latency_max_ms = ms;
                s_stat_latency_samples++;
            }
        }
    }
}

typedef HRESULT (WINAPI *gcn_DwmFlush_t)(void);

/* GDI has no swapchain. Wait until DWM has consumed the previous surface,
 * then paint the complete off-screen DIB immediately after that boundary;
 * the next compositor sample therefore cannot land in the middle of our
 * blit. This wait is confined to the coalescing window thread. */
static void gdi_wait_for_compositor(void) {
    static int resolved = 0;
    static gcn_DwmFlush_t flush = NULL;
    if (!resolved) {
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        if (dwm)
            flush = (gcn_DwmFlush_t)GetProcAddress(dwm, "DwmFlush");
        resolved = 1;
    }
    if (flush)
        flush();
}

/* Commit a complete RGB frame through an off-screen DIBSection. Direct
 * StretchDIBits writes the top-level window surface progressively; DWM can
 * sample that surface in the middle of the copy, which shows up as layer
 * flicker/tearing. StretchBlt/BitBlt from a fully-populated memory DC makes
 * the visible update one GDI operation and is also the accelerated resize
 * path on current Windows drivers. Runs only on the window thread. */
static void gdi_blit_frame(HDC hdc, const RECT* client) {
    if (!s_rgb || !s_rgb_w || !s_rgb_h) return;
    if (!s_gdi_mem_dc)
        s_gdi_mem_dc = CreateCompatibleDC(hdc);
    if (!s_gdi_mem_dc) return;

    if (!s_gdi_bitmap || s_gdi_w != s_rgb_w || s_gdi_h != s_rgb_h) {
        BITMAPINFO bmi;
        memset(&bmi, 0, sizeof bmi);
        bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
        bmi.bmiHeader.biWidth = (LONG)s_rgb_w;
        bmi.bmiHeader.biHeight = -(LONG)s_rgb_h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = NULL;
        HBITMAP bitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS,
                                          &bits, NULL, 0);
        if (!bitmap || !bits) {
            if (bitmap) DeleteObject(bitmap);
            return;
        }
        HGDIOBJ previous = SelectObject(s_gdi_mem_dc, bitmap);
        if (!s_gdi_old_bitmap)
            s_gdi_old_bitmap = previous;
        if (s_gdi_bitmap)
            DeleteObject(s_gdi_bitmap);
        s_gdi_bitmap = bitmap;
        s_gdi_bits = bits;
        s_gdi_w = s_rgb_w;
        s_gdi_h = s_rgb_h;
    }

    memcpy(s_gdi_bits, s_rgb,
           (size_t)s_rgb_w * (size_t)s_rgb_h * sizeof *s_rgb);
    RECT dst;
    compute_dest_rect((int)(client->right - client->left),
                      (int)(client->bottom - client->top),
                      s_rgb_w, s_rgb_h, &dst);
    if (dst.left || dst.top || dst.right != client->right ||
        dst.bottom != client->bottom)
        FillRect(hdc, client, (HBRUSH)GetStockObject(BLACK_BRUSH));
    if ((u32)(dst.right - dst.left) == s_rgb_w &&
        (u32)(dst.bottom - dst.top) == s_rgb_h) {
        BitBlt(hdc, dst.left, dst.top, (int)s_rgb_w, (int)s_rgb_h,
               s_gdi_mem_dc, 0, 0, SRCCOPY);
    } else {
        SetStretchBltMode(hdc, COLORONCOLOR);
        StretchBlt(hdc, dst.left, dst.top, dst.right - dst.left,
                   dst.bottom - dst.top, s_gdi_mem_dc, 0, 0,
                   (int)s_rgb_w, (int)s_rgb_h, SRCCOPY);
    }
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_GCN_PRESENT:
        /* Clear before consuming the latest shared frame. If the emulation
         * thread publishes while conversion/upload is in progress it can
         * post exactly one successor wake; otherwise repeated VI fields are
         * coalesced instead of growing an unbounded stale-message backlog. */
        InterlockedExchange(&s_present_posted, 0);
        do_present(hwnd);
        return 0;
    case WM_PAINT: {
        if (!s_gl_active && s_have_frame)
            gdi_wait_for_compositor();
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (s_gl_active) {
            /* Window drag/expose/resize: redraw the last uploaded texture
             * (no new frame to convert) so the client area never shows
             * garbage between VI fields. */
            if (s_have_frame && s_gl_tex)
                gl_draw_quad(hwnd, s_rgb_w, s_rgb_h);
            else
                gl_draw_black(hwnd);
        } else {
            RECT rc; GetClientRect(hwnd, &rc);
            if (s_have_frame && s_rgb && s_rgb_w && s_rgb_h) {
                gdi_blit_frame(hdc, &rc);
            } else {
                FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;   /* WM_PAINT always repaints the whole client rect */
    case WM_SIZING:
        constrain_sizing_rect(hwnd, wp, (RECT*)lp);
        return TRUE;
    case WM_KEYDOWN:
        handle_key(wp, lp, 1);
        return 0;
    case WM_KEYUP:
        handle_key(wp, lp, 0);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (s_gdi_mem_dc) {
            if (s_gdi_old_bitmap)
                SelectObject(s_gdi_mem_dc, s_gdi_old_bitmap);
            if (s_gdi_bitmap)
                DeleteObject(s_gdi_bitmap);
            DeleteDC(s_gdi_mem_dc);
            s_gdi_mem_dc = NULL;
            s_gdi_bitmap = NULL;
            s_gdi_old_bitmap = NULL;
            s_gdi_bits = NULL;
        }
        if (s_gl_active) {
            wglMakeCurrent(NULL, NULL);
            wglDeleteContext(s_gl_rc);
            ReleaseDC(hwnd, s_gl_hdc);
            s_gl_active = 0;
        }
        __atomic_store_n(&s_quit, 1, __ATOMIC_RELEASE);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

typedef struct {
    CPUState* cpu;
    HANDLE    ready_event;
} WindowStartCtx;

static DWORD WINAPI window_thread_proc(LPVOID param) {
    WindowStartCtx* ctx = (WindowStartCtx*)param;
    s_cpu = ctx->cpu;
    /* Painting may be intercepted by desktop capture/overlay software and
     * become unexpectedly CPU-heavy.  The mailbox always keeps the newest
     * complete field, so letting this consumer yield to emulation is both
     * lower-latency and safer than starving the audio/GX producer. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize        = sizeof wc;
    /* WGL keeps the HDC for the context lifetime. CS_OWNDC makes that a
     * private, stable window DC instead of retaining a transient common DC. */
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "gcnrecomp_host_window";
    RegisterClassExA(&wc);

    /* No frame has arrived yet at window-creation time; start at a
     * reasonable NTSC-ish default (doubled 640x480) — do_present() resizes
     * to the real geometry (2x/4x rule) the moment the first field lands. */
    RECT rc = { 0, 0, 1280, 960 };
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    s_hwnd = CreateWindowExA(0, wc.lpszClassName, "gcnrecomp", style,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             NULL, NULL, wc.hInstance, NULL);
    if (!s_hwnd) {
        fprintf(stderr, "gcn host window: CreateWindowExA failed (GLE=%lu)\n", GetLastError());
        SetEvent(ctx->ready_event);
        return 0;
    }
    ShowWindow(s_hwnd, SW_SHOW);
    UpdateWindow(s_hwnd);

    /* WGL context creation happens here — on the window thread, after the
     * window exists, before the message pump starts. No-op (leaves
     * s_gl_active=0, GDI path used exactly as before) if GCN_GL=0 or any WGL
     * step fails. */
    gl_init(s_hwnd);

    SetEvent(ctx->ready_event);

    MSG msg;
    BOOL r;
    while ((r = GetMessageA(&msg, NULL, 0, 0)) != 0) {
        if (r == -1) break;   /* GetMessage error */
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

void gcn_host_window_start(CPUState* cpu) {
    if (!gcn_host_window_enabled()) return;

    static int s_started = 0;
    if (s_started) return;
    s_started = 1;

    /* Interactive emulation has a real-time audio deadline.  One
     * above-normal producer thread prevents GDI capture and ordinary desktop
     * work from turning otherwise-sufficient rendered throughput into A/V
     * starvation; the WASAPI callback has its own MMCSS priority. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    static WindowStartCtx ctx;
    ctx.cpu = cpu;
    ctx.ready_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ctx.ready_event) {
        fprintf(stderr, "gcn host window: CreateEventA failed — GCN_WINDOW disabled this run\n");
        return;
    }

    HANDLE th = CreateThread(NULL, 0, window_thread_proc, &ctx, 0, NULL);
    if (!th) {
        fprintf(stderr, "gcn host window: CreateThread failed — GCN_WINDOW disabled this run\n");
        CloseHandle(ctx.ready_event);
        return;
    }
    CloseHandle(th);   /* never joined — the window thread lives for the process,
                        * same lifecycle as gx_raster.c's GX-MT workers. */

    DWORD wr = WaitForSingleObject(ctx.ready_event, 5000);
    CloseHandle(ctx.ready_event);
    if (wr != WAIT_OBJECT_0) {
        fprintf(stderr, "gcn host window: window did not become ready within 5s\n");
        return;
    }
    fprintf(stdout,
        "gcn host window: GCN_WINDOW=1 — native window up (arrows=stick  X=A Z=B "
        "C=X V=Y  Enter/Space=Start  WASD=dpad  IJKL=cstick  Q/E=L/R; close the "
        "window to quit)\n");
    fflush(stdout);
}

void gcn_host_window_present(const u8* xfb, u32 width, u32 height, u32 stride) {
    if (!gcn_host_window_enabled() || !s_hwnd || !xfb) return;
    if (width == 0 || height == 0 || stride == 0) return;

    /* GCN_PRESENT_STATS=1: stamp the post BEFORE the memcpy/lock below so the
     * latency measured at the present site includes the full mailbox publish
     * (matches what "post" means to a caller waiting on the window to catch
     * up), not just the message-post instant. Read once into a local so the
     * getenv-cache check and the timestamp always agree within this call. */
    int stats_on = gcn_present_stats_enabled();
    if (stats_on) {
        s_stat_posts++;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        s_stat_last_post_qpc = now.QuadPart;
    }

    size_t need = (size_t)stride * (size_t)height;
    AcquireSRWLockExclusive(&s_lock);
    if (need > s_shared_cap) {
        u8* n = (u8*)realloc(s_shared_data, need);
        if (!n) { ReleaseSRWLockExclusive(&s_lock); return; }
        s_shared_data = n; s_shared_cap = (u32)need;
    }
    memcpy(s_shared_data, xfb, need);
    s_shared_w = width; s_shared_h = height; s_shared_stride = stride;
    s_shared_valid = 1;
    ReleaseSRWLockExclusive(&s_lock);

    /* InterlockedExchange returning 1 means a previous post's mailbox slot
     * was still pending (the window thread hasn't consumed WM_GCN_PRESENT
     * yet) and this post just overwrote it — that previous field is the one
     * GCN_PRESENT_STATS counts as coalesced, not this one (this one is what
     * do_present() will actually draw). */
    LONG prev_posted = InterlockedExchange(&s_present_posted, 1);
    if (prev_posted == 0) {
        if (!PostMessageA(s_hwnd, WM_GCN_PRESENT, 0, 0))
            InterlockedExchange(&s_present_posted, 0);
    } else if (stats_on) {
        s_stat_coalesced++;
    }
}

int gcn_host_window_quit_requested(void) {
    return (int)__atomic_load_n(&s_quit, __ATOMIC_ACQUIRE);
}
