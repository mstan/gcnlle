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
#include "dsp/dsp.h" /* audio_state: current AID DMA registers */
#include "vi/vi.h"
#include "si/si.h"
#include "pi/pi.h"
#include "ai/ai.h"
#include "mi/mi.h"
#include "cp/cp.h"
#include "pe/pe.h"
#include "gp/gp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the server can reach into to answer state queries. Fields may be NULL;
 * a command needing a NULL dependency returns an "unavailable" error.
 *
 * SNAPSHOT_RESUME (docs/SNAPSHOT_RESUME.md): vi/si/pi/ai/mi/cp/pe/gp were
 * added so runtime/src/snapshot.c has one place to reach every device
 * boot.c constructs as function-static locals in main() — the same
 * "boot.c owns the instances, hands out borrowed pointers" pattern this
 * struct already used for cpu/di/exi/dsp. gcn_debug_server_ctx() exposes
 * the registered struct read-only for that purpose (and any future one). */
typedef struct {
    CPUState* cpu;   /* regs + RAM (get_registers, read_ram, write_ram)        */
    GcnDi*    di;     /* disc interface (insert_disc, eject_disc)              */
    GcnExi*   exi;    /* EXI RTC state (rtc_state)                             */
    GcnDsp*   dsp;    /* DSP AID DMA state (audio_state)                       */
    GcnVi*    vi;
    GcnSi*    si;
    GcnPi*    pi;
    GcnAi*    ai;
    GcnMi*    mi;
    GcnCp*    cp;
    GcnPe*    pe;
    GcnGp*    gp;
} GcnDebugCtx;

/* Start listening if GCN_DEBUG_PORT is set (else a no-op returning 0). Returns
 * the bound port on success, 0 if disabled, -1 on socket error. `ctx` is
 * borrowed (must outlive the server — boot.c keeps it in static/stack scope). */
int  gcn_debug_server_start(const GcnDebugCtx* ctx);

/* SNAPSHOT_RESUME: read-only access to the ctx registered by
 * gcn_debug_server_start, for runtime/src/snapshot.c's SAVE path. Returns a
 * pointer to an all-NULL static (never NULL itself) so callers never need a
 * NULL check on the return value, only on its fields. NOTE: gcn_debug_server
 * _start returns early (never populating the ctx) unless GCN_DEBUG_PORT is
 * set — same precondition GCN_CHECKPOINT_PC already has (it's parsed in the
 * same early-return-gated body) — so GCN_SNAPSHOT_SAVE inherits that
 * requirement too: it only ever fires from inside the checkpoint-park path,
 * which cannot itself be armed without GCN_DEBUG_PORT. */
const GcnDebugCtx* gcn_debug_server_ctx(void);

/* Non-blocking service: accept pending clients, process complete request lines,
 * send responses. Call once per block from the dispatch loop. No-op if disabled. */
void gcn_debug_server_pump(void);

/* Differential co-simulation control. GCN_COSIM=1 plus GCN_DEBUG_PORT parks
 * the CPU before the first guest instruction and makes the TCP coordinator
 * grant deterministic instruction budgets with `cosim_step`. The ordinary
 * runtime remains AOT-first; this interpreter-only mode is a diagnostic
 * surface for full-state first-divergence work, never the shipping path.
 *
 * before_instruction blocks (while pumping TCP) until a budget is available.
 * after_instruction retires exactly one granted instruction. */
int  gcn_debug_server_cosim_enabled(void);
int  gcn_debug_server_cosim_before_instruction(void);
void gcn_debug_server_cosim_after_instruction(void);

/* AOT-safe milestone checkpoint. `checkpoint_arm` names a dispatcher-entry PC
 * and may additionally require one GPR value. Normal native dispatch remains
 * enabled. When the condition matches, this parks before the block executes
 * while continuing to serve TCP queries until `checkpoint_resume`. */
int  gcn_debug_server_checkpoint_before_block(void);

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
