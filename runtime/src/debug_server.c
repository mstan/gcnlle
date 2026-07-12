/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * TCP debug server (see include/debug/debug_server.h). Winsock, non-blocking,
 * pumped from the dispatch loop. JSON-over-newline. Query surface over the
 * always-on rings (debug/rings.h) + live CPU/RAM state.
 *
 * Deliberately small: one listen socket, a few clients, a hand-rolled parser
 * for the flat {"cmd":..,"key":num,..} requests our own client emits. It grows
 * command-by-command alongside the device models, the same way the psxrecomp
 * surface did.
 */
#include "debug/debug_server.h"
#include "debug/rings.h"
#include "dsp_lle_c.h"     /* dsp_state: live DSP pc/control/mailbox peeks */
#include "vi/vi.h"         /* screenshot: XFB scanout geometry */
#include "si/si.h"         /* set_input: injected pad-report surface */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GCN_DBG_MAX_CLIENTS 4
#define GCN_DBG_LINE_CAP    (64 * 1024)      /* max request line               */
#define GCN_DBG_RESP_CAP    (4 * 1024 * 1024) /* max response (ring dumps)      */
#define GC_RAM_BASE_ADDR    0x80000000u

typedef struct {
    SOCKET sock;
    int    len;                    /* bytes buffered in `line`                 */
    char   line[GCN_DBG_LINE_CAP];
} Client;

static int      s_enabled;
static SOCKET   s_listen = INVALID_SOCKET;
static Client   s_clients[GCN_DBG_MAX_CLIENTS];
static GcnDebugCtx s_ctx;
static int      s_quit;
static char     s_resp[GCN_DBG_RESP_CAP];

/* ---- tiny JSON field extractors (flat objects only) ---- */

/* Locate `"key"`, skip to the value after the colon. Returns pointer at the
 * first value char, or NULL. */
static const char* json_find_value(const char* s, const char* key) {
    char pat[64];
    int kl = snprintf(pat, sizeof pat, "\"%s\"", key);
    if (kl <= 0) return NULL;
    const char* p = strstr(s, pat);
    if (!p) return NULL;
    p += kl;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Parse an unsigned integer field (decimal or 0x-hex). Returns 1 if found. */
static int json_uint(const char* s, const char* key, u32* out) {
    const char* p = json_find_value(s, key);
    if (!p) return 0;
    *out = (u32)strtoul(p, NULL, 0);
    return 1;
}

/* Copy a string field into out (without quotes). Returns 1 if found. */
static int json_str(const char* s, const char* key, char* out, int cap) {
    const char* p = json_find_value(s, key);
    if (!p || *p != '"') return 0;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < cap - 1) out[i++] = *p++;
    out[i] = 0;
    return 1;
}

/* ---- response send (blocking-ish, bounded; localhost dev use) ---- */

static void client_close(Client* c) {
    if (c->sock != INVALID_SOCKET) { closesocket(c->sock); c->sock = INVALID_SOCKET; }
    c->len = 0;
}

static void send_all(Client* c, const char* buf, int len) {
    int off = 0, guard = 0;
    while (off < len) {
        int w = send(c->sock, buf + off, len - off, 0);
        if (w == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                if (++guard > 100000) { client_close(c); return; }  /* client stalled */
                continue;
            }
            client_close(c);
            return;
        }
        off += w; guard = 0;
    }
}

/* ---- command dispatch ---- */

static u8* ram_ptr(u32 guest_addr, u32* avail) {
    CPUState* cpu = s_ctx.cpu;
    if (!cpu || !cpu->ram) return NULL;
    u32 phys = guest_addr & 0x1FFFFFFFu;           /* cached/uncached mirror -> MEM1 */
    if (phys >= cpu->ram_size) return NULL;
    *avail = cpu->ram_size - phys;
    return cpu->ram + phys;
}

static void handle_line(Client* c, const char* line) {
    char cmd[48] = {0};
    u32 id = 0; int have_id = json_uint(line, "id", &id);
    if (!json_str(line, "cmd", cmd, sizeof cmd)) {
        int n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                         "{\"ok\":false,\"error\":\"missing cmd\"}\n");
        send_all(c, s_resp, n);
        return;
    }

    int n = 0;
    CPUState* cpu = s_ctx.cpu;

    if (!strcmp(cmd, "ping") || !strcmp(cmd, "frame")) {
        n = snprintf(s_resp, GCN_DBG_RESP_CAP,
            "{\"ok\":true,\"block\":%llu,\"pc\":%u}\n",
            (unsigned long long)gcn_ring_block_index(), cpu ? cpu->pc : 0);
    }
    else if (!strcmp(cmd, "get_registers") || !strcmp(cmd, "regs")) {
        if (!cpu) { n = snprintf(s_resp, GCN_DBG_RESP_CAP, "{\"ok\":false,\"error\":\"no cpu\"}\n"); }
        else {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP, "{\"ok\":true,\"gpr\":[");
            for (int i = 0; i < 32; i++)
                n += snprintf(s_resp + n, GCN_DBG_RESP_CAP - n, "%s%u", i ? "," : "", cpu->gpr[i]);
            n += snprintf(s_resp + n, GCN_DBG_RESP_CAP - n,
                "],\"pc\":%u,\"lr\":%u,\"ctr\":%u,\"cr\":%u,\"xer\":%u,\"msr\":%u,"
                "\"srr0\":%u,\"srr1\":%u,\"dar\":%u,\"dsisr\":%u,\"exception\":%u,"
                "\"timebase\":%llu}\n",
                cpu->pc, cpu->lr, cpu->ctr, cpu->cr, cpu->xer, cpu->msr,
                cpu->srr0, cpu->srr1, cpu->dar, cpu->dsisr, cpu->exception,
                (unsigned long long)cpu->timebase);
        }
    }
    else if (!strcmp(cmd, "read_ram") || !strcmp(cmd, "dump_ram")) {
        u32 addr = 0, len = 0;
        json_uint(line, "addr", &addr); json_uint(line, "len", &len);
        if (len > 65536) len = 65536;
        u32 avail = 0; u8* p = ram_ptr(addr, &avail);
        if (!p) { n = snprintf(s_resp, GCN_DBG_RESP_CAP, "{\"ok\":false,\"error\":\"addr out of range\"}\n"); }
        else {
            if (len > avail) len = avail;
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":true,\"addr\":%u,\"len\":%u,\"hex\":\"", addr, len);
            static const char hexd[] = "0123456789abcdef";
            for (u32 i = 0; i < len && n < GCN_DBG_RESP_CAP - 4; i++) {
                s_resp[n++] = hexd[p[i] >> 4];
                s_resp[n++] = hexd[p[i] & 0xF];
            }
            n += snprintf(s_resp + n, GCN_DBG_RESP_CAP - n, "\"}\n");
        }
    }
    else if (!strcmp(cmd, "write_ram")) {
        u32 addr = 0; char hex[8192] = {0};
        json_uint(line, "addr", &addr);
        json_str(line, "hex", hex, sizeof hex);
        u32 avail = 0; u8* p = ram_ptr(addr, &avail);
        int nbytes = (int)strlen(hex) / 2;
        if (!p) { n = snprintf(s_resp, GCN_DBG_RESP_CAP, "{\"ok\":false,\"error\":\"addr out of range\"}\n"); }
        else {
            int wrote = 0;
            for (int i = 0; i < nbytes && (u32)i < avail; i++) {
                char b[3] = { hex[2*i], hex[2*i+1], 0 };
                p[i] = (u8)strtoul(b, NULL, 16); wrote++;
            }
            n = snprintf(s_resp, GCN_DBG_RESP_CAP, "{\"ok\":true,\"wrote\":%d}\n", wrote);
        }
    }
    else if (!strcmp(cmd, "mmio_dump")) {
        u32 count = 256, addr = 0, rw = 0;
        int have_addr = json_uint(line, "addr", &addr);
        int have_rw = json_uint(line, "rw", &rw);
        json_uint(line, "count", &count);
        n = gcn_ring_mmio_json(s_resp, GCN_DBG_RESP_CAP, (int)count,
                               addr, have_addr, have_rw ? (int)rw : -1);
    }
    else if (!strcmp(cmd, "block_dump")) {
        u32 count = 256; json_uint(line, "count", &count);
        n = gcn_ring_block_json(s_resp, GCN_DBG_RESP_CAP, (int)count);
    }
    else if (!strcmp(cmd, "event_dump")) {
        u32 count = 256; json_uint(line, "count", &count);
        n = gcn_ring_event_json(s_resp, GCN_DBG_RESP_CAP, (int)count);
    }
    else if (!strcmp(cmd, "fifo_dump")) {
        /* GX gather-pipe burst recorder (ROADMAP M2 packet inventory). */
        u32 count = 64; json_uint(line, "count", &count);
        n = gcn_ring_fifo_json(s_resp, GCN_DBG_RESP_CAP, (int)count);
    }
    else if (!strcmp(cmd, "card_traffic")) {
        /* EXI memory-card transaction recorder (ROADMAP M4). Sparse always-on
         * ring — the directory read/save read/write command stream stays
         * queryable long after it happened (unlike the shallow MMIO ring). */
        u32 count = 256; json_uint(line, "count", &count);
        n = gcn_ring_memcard_json(s_resp, GCN_DBG_RESP_CAP, (int)count);
    }
    else if (!strcmp(cmd, "dsp_state")) {
        /* Live DSP-LLE core state: pc, control reg, and non-consuming peeks of
         * both mailboxes (bit 31 = mail pending). Diagnoses CPU<->DSP task
         * handshake stalls without perturbing the run. */
        n = snprintf(s_resp, GCN_DBG_RESP_CAP,
            "{\"ok\":true,\"pc\":%u,\"control\":%u,"
            "\"mbox_dsp_to_cpu\":%u,\"mbox_cpu_to_dsp\":%u}\n",
            (unsigned)dsp_lle_pc(), (unsigned)dsp_lle_read_control(),
            (unsigned)dsp_lle_peek_mbox_dsp(), (unsigned)dsp_lle_peek_mbox_cpu());
    }
    else if (!strcmp(cmd, "screenshot") || !strcmp(cmd, "screenshot_file")) {
        /* Decode the XFB the VI is scanning out into a PPM on disk. Geometry
         * comes from the guest-programmed VI registers (gcn_vi_xfb_info); the
         * XFB pixel format is YUY2 — 4 bytes [Y0,U,Y1,V] per 2 px — converted
         * with the inverse BT.601 matrix exactly as Dolphin's XFB decode
         * (TextureConversionShader.cpp:1009-1035). If the guest has not
         * programmed an XFB, report that honestly (no fake image). */
        u32 fb_addr, fb_w, fb_h, fb_stride;
        char path[512] = {0};
        if (!json_str(line, "path", path, sizeof path))
            snprintf(path, sizeof path, "_work/screenshot.ppm");
        if (!gcn_vi_xfb_info(&fb_addr, &fb_w, &fb_h, &fb_stride)) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"VI has no XFB programmed (base/width/ACV zero)\"}\n");
        } else if (!cpu || !cpu->ram ||
                   (u64)fb_addr + (u64)fb_stride * fb_h > cpu->ram_size) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"XFB out of MEM1 range\",\"addr\":%u}\n", fb_addr);
        } else {
            FILE* f = fopen(path, "wb");
            if (!f) {
                n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                    "{\"ok\":false,\"error\":\"cannot open output path\"}\n");
            } else {
                fprintf(f, "P6\n%u %u\n255\n", fb_w, fb_h);
                u64 luma_sum = 0;
                for (u32 y = 0; y < fb_h; y++) {
                    const u8* row = cpu->ram + fb_addr + (u64)y * fb_stride;
                    for (u32 x = 0; x < fb_w; x++) {
                        const u8* px = row + (x / 2u) * 4u;
                        double Y = (x & 1u) ? px[2] : px[0];
                        double U = px[1], V = px[3];
                        double yc = 1.164 * (Y - 16.0);
                        double r = yc + 1.596 * (V - 128.0);
                        double g = yc - 0.813 * (V - 128.0) - 0.391 * (U - 128.0);
                        double b = yc + 2.018 * (U - 128.0);
                        u8 rgb[3];
                        rgb[0] = (u8)(r < 0 ? 0 : r > 255 ? 255 : r);
                        rgb[1] = (u8)(g < 0 ? 0 : g > 255 ? 255 : g);
                        rgb[2] = (u8)(b < 0 ? 0 : b > 255 ? 255 : b);
                        fwrite(rgb, 1, 3, f);
                        luma_sum += (u8)Y;
                    }
                }
                fclose(f);
                /* mean_luma lets a client assert non-black without fetching
                 * the image (16 = XFB black level). */
                n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                    "{\"ok\":true,\"path\":\"%s\",\"width\":%u,\"height\":%u,"
                    "\"xfb_addr\":%u,\"mean_luma\":%.1f}\n",
                    path, fb_w, fb_h, fb_addr,
                    (double)luma_sum / ((double)fb_w * fb_h));
            }
        }
    }
    else if (!strcmp(cmd, "set_input")) {
        /* Debug-surface pad injection (ROADMAP M3/M4): drives the menu through
         * the SI model instead of the hardcoded-neutral report. Every field is
         * optional — unspecified means "leave unchanged" — except "reset":1,
         * which snaps back to the power-on-neutral report (buttons=0, sticks
         * centered 0x80, triggers 0) and ignores every other field. Applies to
         * the single SI instance registered by gcn_si_init (boot.c's GcnSi). */
        u32 buttons = 0, stick_x = 0, stick_y = 0, substick_x = 0, substick_y = 0;
        u32 trigger_l = 0, trigger_r = 0, reset = 0;
        int have_buttons    = json_uint(line, "buttons", &buttons);
        int have_stick_x    = json_uint(line, "stick_x", &stick_x);
        int have_stick_y    = json_uint(line, "stick_y", &stick_y);
        int have_substick_x = json_uint(line, "substick_x", &substick_x);
        int have_substick_y = json_uint(line, "substick_y", &substick_y);
        int have_trigger_l  = json_uint(line, "trigger_l", &trigger_l);
        int have_trigger_r  = json_uint(line, "trigger_r", &trigger_r);
        json_uint(line, "reset", &reset);

        if (!gcn_si_debug_set_input(have_buttons, buttons, have_stick_x, stick_x,
                                     have_stick_y, stick_y, have_substick_x, substick_x,
                                     have_substick_y, substick_y, have_trigger_l, trigger_l,
                                     have_trigger_r, trigger_r, (int)reset)) {
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":false,\"error\":\"SI not initialized yet\"}\n");
        } else {
            GcnSiPadInput cur;
            gcn_si_debug_get_input(&cur);
            n = snprintf(s_resp, GCN_DBG_RESP_CAP,
                "{\"ok\":true,\"buttons\":%u,\"stick_x\":%u,\"stick_y\":%u,"
                "\"substick_x\":%u,\"substick_y\":%u,\"trigger_l\":%u,\"trigger_r\":%u}\n",
                (unsigned)cur.buttons, (unsigned)cur.stick_x, (unsigned)cur.stick_y,
                (unsigned)cur.substick_x, (unsigned)cur.substick_y,
                (unsigned)cur.trigger_l, (unsigned)cur.trigger_r);
        }
    }
    else if (!strcmp(cmd, "quit")) {
        s_quit = 1;
        n = snprintf(s_resp, GCN_DBG_RESP_CAP, "{\"ok\":true,\"quit\":true}\n");
    }
    else {
        n = snprintf(s_resp, GCN_DBG_RESP_CAP,
            "{\"ok\":false,\"error\":\"unknown cmd\",\"cmd\":\"%s\"}\n", cmd);
    }

    /* Splice the request id into the response if one was supplied. Static scratch
     * (single-threaded main-loop pump) — a stack buffer this size would overflow. */
    static char s_withid[GCN_DBG_RESP_CAP];
    if (have_id && n > 1 && s_resp[0] == '{') {
        int m = snprintf(s_withid, sizeof s_withid, "{\"id\":%u,%s", id, s_resp + 1);
        if (m > 0 && m < (int)sizeof s_withid) { memcpy(s_resp, s_withid, m); n = m; }
    }
    send_all(c, s_resp, n);
}

static void set_nonblocking(SOCKET s) {
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
}

int gcn_debug_server_start(const GcnDebugCtx* ctx) {
    const char* penv = getenv("GCN_DEBUG_PORT");
    if (!penv || !*penv) return 0;         /* disabled — rings still record */
    int port = atoi(penv);
    if (port <= 0 || port > 65535) return 0;

    s_ctx = *ctx;
    for (int i = 0; i < GCN_DBG_MAX_CLIENTS; i++) s_clients[i].sock = INVALID_SOCKET;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;

    s_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen == INVALID_SOCKET) { WSACleanup(); return -1; }
    int yes = 1;
    setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* localhost only */
    addr.sin_port = htons((u_short)port);
    if (bind(s_listen, (struct sockaddr*)&addr, sizeof addr) == SOCKET_ERROR ||
        listen(s_listen, 4) == SOCKET_ERROR) {
        closesocket(s_listen); s_listen = INVALID_SOCKET; WSACleanup(); return -1;
    }
    set_nonblocking(s_listen);
    s_enabled = 1; s_quit = 0;
    fprintf(stdout, "gcn debug: TCP debug server on 127.0.0.1:%d "
                    "(always-on rings; JSON-over-newline)\n", port);
    fflush(stdout);
    return port;
}

void gcn_debug_server_pump(void) {
    if (!s_enabled) return;

    /* Accept any pending connections into a free slot. */
    for (;;) {
        SOCKET cs = accept(s_listen, NULL, NULL);
        if (cs == INVALID_SOCKET) break;
        int slot = -1;
        for (int i = 0; i < GCN_DBG_MAX_CLIENTS; i++)
            if (s_clients[i].sock == INVALID_SOCKET) { slot = i; break; }
        if (slot < 0) { closesocket(cs); continue; }   /* full */
        set_nonblocking(cs);
        s_clients[slot].sock = cs;
        s_clients[slot].len = 0;
    }

    /* Drain ready bytes from each client, dispatch complete lines. */
    for (int i = 0; i < GCN_DBG_MAX_CLIENTS; i++) {
        Client* c = &s_clients[i];
        if (c->sock == INVALID_SOCKET) continue;

        for (;;) {
            int room = GCN_DBG_LINE_CAP - 1 - c->len;
            if (room <= 0) { c->len = 0; room = GCN_DBG_LINE_CAP - 1; }  /* overlong: reset */
            int r = recv(c->sock, c->line + c->len, room, 0);
            if (r == 0) { client_close(c); break; }         /* peer closed */
            if (r == SOCKET_ERROR) {
                if (WSAGetLastError() != WSAEWOULDBLOCK) client_close(c);
                break;
            }
            c->len += r;
            c->line[c->len] = 0;

            /* Process each newline-terminated request in the buffer. */
            int start = 0;
            for (int j = 0; j < c->len; j++) {
                if (c->line[j] == '\n') {
                    c->line[j] = 0;
                    if (j > start) handle_line(c, c->line + start);
                    if (c->sock == INVALID_SOCKET) break;   /* closed mid-dispatch */
                    start = j + 1;
                }
            }
            if (c->sock == INVALID_SOCKET) break;
            /* Shift any partial trailing line to the front. */
            if (start > 0) {
                c->len -= start;
                memmove(c->line, c->line + start, c->len);
            }
        }
    }
}

int gcn_debug_server_quit_requested(void) { return s_quit; }

void gcn_debug_server_park(void) {
    if (!s_enabled) return;
    fprintf(stdout, "gcn debug: parked — serving debug queries; send \"quit\" to exit.\n");
    fflush(stdout);
    while (s_enabled && !s_quit) {
        gcn_debug_server_pump();
        Sleep(1);
    }
}

void gcn_debug_server_stop(void) {
    if (!s_enabled) return;
    for (int i = 0; i < GCN_DBG_MAX_CLIENTS; i++) client_close(&s_clients[i]);
    if (s_listen != INVALID_SOCKET) { closesocket(s_listen); s_listen = INVALID_SOCKET; }
    WSACleanup();
    s_enabled = 0;
}
