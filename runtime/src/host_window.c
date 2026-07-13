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

/* ---- shared publish buffer (main thread writes, window thread reads) ---- */
static SRWLOCK s_lock = SRWLOCK_INIT;
static u8*  s_shared_data  = NULL;
static u32  s_shared_cap   = 0;      /* bytes allocated                    */
static u32  s_shared_w     = 0;
static u32  s_shared_h     = 0;
static u32  s_shared_stride= 0;
static int  s_shared_valid = 0;

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

/* Copy the shared frame out under the lock, resize the window if its
 * geometry changed, convert YUY2->RGB, and invalidate for WM_PAINT to blit.
 * Runs on the window thread only. */
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

    /* Resize-on-geometry-change: client area is 2x the XFB size, except a
     * half-height VI field (e.g. 592x224, height*2 <= width) gets its
     * vertical zoom doubled again (4x) to look proportionally correct —
     * exactly gcn_viewer.py's poll_frame `ydouble = 2 if h*2<=w else 1`
     * rule, applied to window client size instead of a Tk image zoom. */
    if (w != s_last_w || h != s_last_h) {
        u32 cw, ch;
        if (h * 2u <= w) { cw = w * 2u; ch = h * 4u; }
        else             { cw = w * 2u; ch = h * 2u; }
        RECT rc = { 0, 0, (LONG)cw, (LONG)ch };
        DWORD style   = (DWORD)GetWindowLongPtrA(hwnd, GWL_STYLE);
        DWORD exstyle = (DWORD)GetWindowLongPtrA(hwnd, GWL_EXSTYLE);
        AdjustWindowRectEx(&rc, style, FALSE, exstyle);
        SetWindowPos(hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        s_last_w = w; s_last_h = h;
    }

    u64 npx = (u64)w * (u64)h;
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
        u32* out = s_rgb + (size_t)y * w;
        for (u32 x = 0; x < w; x++) {
            const u8* px = row + (x / 2u) * 4u;
            u8 r, g, b;
            gcn_yuy2_to_rgb(px, (int)(x & 1u), &r, &g, &b);
            out[x] = ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        }
    }
    s_rgb_w = w; s_rgb_h = h; s_have_frame = 1;
    InvalidateRect(hwnd, NULL, FALSE);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_GCN_PRESENT:
        do_present(hwnd);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        if (s_have_frame && s_rgb && s_rgb_w && s_rgb_h) {
            BITMAPINFO bmi;
            memset(&bmi, 0, sizeof bmi);
            bmi.bmiHeader.biSize        = sizeof bmi.bmiHeader;
            bmi.bmiHeader.biWidth       = (LONG)s_rgb_w;
            bmi.bmiHeader.biHeight      = -(LONG)s_rgb_h;   /* top-down DIB */
            bmi.bmiHeader.biPlanes      = 1;
            bmi.bmiHeader.biBitCount    = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                          0, 0, (int)s_rgb_w, (int)s_rgb_h,
                          s_rgb, &bmi, DIB_RGB_COLORS, SRCCOPY);
        } else {
            FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;   /* WM_PAINT always repaints the whole client rect */
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

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize        = sizeof wc;
    wc.style         = CS_HREDRAW | CS_VREDRAW;
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

    PostMessageA(s_hwnd, WM_GCN_PRESENT, 0, 0);
}

int gcn_host_window_quit_requested(void) {
    return (int)__atomic_load_n(&s_quit, __ATOMIC_ACQUIRE);
}
