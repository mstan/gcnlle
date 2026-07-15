/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gcnrecomp runtime — TCP debug server (mirrors psxrecomp's debug_server.c).
 *
 * Protocol: JSON-over-newline, one request object per line, one response line
 * back on the same connection. Request:  {"id":N,"cmd":"<name>",...params}
 *           Success:  {"id":N,"ok":true,...data}   Failure: {"ok":false,"error":..}
 *
 * The server is NON-BLOCKING and pumped from the main dispatch loop
 * (gcn_debug_server_pump, called every block). It never spawns a thread and
 * never blocks the guest: it accepts, drains ready bytes, answers complete
 * lines, and returns. Because it is pumped every block, it stays responsive
 * even while the guest busy-waits on unmodeled hardware (which is exactly when
 * you want to inspect it).
 *
 * The always-on ring buffers (debug/rings.h) record regardless; this server is
 * the QUERY surface over them, plus live CPU/RAM state. It only listens when
 * GCN_DEBUG_PORT is set in the environment (rings still record when it isn't).
 */
#ifndef GCN_DEBUG_SERVER_H
#define GCN_DEBUG_SERVER_H

#include "cpu/cpu.h"
#include "di/di.h"   /* ROADMAP M5: insert_disc/eject_disc reach into the DI device */
#include "exi/exi.h" /* rtc_state: live one-shot-synchronized RTC state */

#ifdef __cplusplus
extern "C" {
#endif

/* What the server can reach into to answer state queries. Fields may be NULL;
 * a command needing a NULL dependency returns an "unavailable" error. */
typedef struct {
    CPUState* cpu;   /* regs + RAM (get_registers, read_ram, write_ram)        */
    GcnDi*    di;     /* disc interface (insert_disc, eject_disc)              */
    GcnExi*   exi;    /* EXI RTC state (rtc_state)                             */
} GcnDebugCtx;

/* Start listening if GCN_DEBUG_PORT is set (else a no-op returning 0). Returns
 * the bound port on success, 0 if disabled, -1 on socket error. `ctx` is
 * borrowed (must outlive the server — boot.c keeps it in static/stack scope). */
int  gcn_debug_server_start(const GcnDebugCtx* ctx);

/* Non-blocking service: accept pending clients, process complete request lines,
 * send responses. Call once per block from the dispatch loop. No-op if disabled. */
void gcn_debug_server_pump(void);

/* True after a client issued "quit" (or "continue"-style stop) — lets the run
 * loop end cleanly on request. */
int  gcn_debug_server_quit_requested(void);

/* Block pumping the server (with a 1ms idle sleep) until a client issues "quit".
 * Used to keep the process — and its always-on rings — alive for inspection
 * after the guest stops or parks on unmodeled hardware. No-op if disabled. */
void gcn_debug_server_park(void);

/* Close the listen + client sockets. */
void gcn_debug_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* GCN_DEBUG_SERVER_H */
