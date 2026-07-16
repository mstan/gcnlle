/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * DSP interface model. The 0xCC005000 block splits two ways:
 *   - mailboxes (0x00-0x06) and the control register's DSP-owned bits
 *     (DSP_CONTROL_MASK) route to the REAL DSP running its firmware, via the
 *     dsp_lle C API (runtime/dsp_lle/ — Dolphin's DSP-LLE interpreter + our
 *     Host binding). No HLE: the DSP boots its IROM and runs the ucode the IPL
 *     uploads, posting real mail.
 *   - the CPU-side CSR bits (interrupt status/masks, ARAM-DMA in-progress) and
 *     the AR/AI DMA engine (0x12/0x20/0x24/0x28) are Flipper hardware, modeled
 *     here. The AR DMA moves bytes between MEM1 and the 16 MB ARAM for real.
 *
 * CSR read/write mirror Dolphin's DSP.cpp: the DSP-owned bits come from the core
 * (dsp_lle_read_control), the rest from this CPU-side state.
 */
#include "dsp/dsp.h"
#include "dsp_lle_c.h"
#include "debug/rings.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>     /* [PERF] DSP worker thread: CreateThread +
                          * WaitOnAddress/WakeByAddressAll — same futex-style
                          * idiom as gx_raster.c's GX-MT pool. */
#include <immintrin.h>   /* _mm_pause in the spin phases */

/* The dispatch loop ticks one DSP (like the one VI/DI); registered at init. */
static GcnDsp* s_dsp = NULL;

/* Lazy DSP RunCycles batching (perf). dsp_lle_update() -> DSPCore::RunCycles()
 * carries a measured ~330 ns FIXED per-call overhead (cache-cold re-entry into
 * the interpreter) that was 89% of total runtime when paid once per executed
 * PPC block (dispatch.c's per-block gcn_dsp_tick call). Instead of stepping the
 * core every block, accumulate the PPC-cycle debt here and run the SAME total
 * DSP cycles in far fewer, larger RunCycles calls (gcn_dsp_flush), flushing
 * whenever the CPU is about to observe DSP state or the debt crosses
 * s_batch_ppc. Total DSP cycles executed over a run is unchanged; only call
 * granularity is. */
static u32 s_owed_ppc = 0;      /* PPC-cycle debt not yet run on the DSP core */
static int s_batch = -1;        /* GCN_DSP_BATCH: default on, "0" disables    */
static u32 s_batch_ppc = 4096;  /* GCN_DSP_BATCH_PPC: flush cap (PPC cycles)  */

/* ---- [PERF, opt-in] DSP worker thread (GCN_DSP_THREAD=1) ----
 *
 * Attribution at perf-campaign-2/E1 (12M-block derived boot): the DSP bucket
 * is ~20% of wall — 240k flush calls x ~62 interpreted ROM-idle-loop steps
 * each (~0.53 s of pc=0x0033 mail-wait spinning; halt-skip doesn't apply
 * because the core isn't halted, it's busy-waiting). That work is pure core
 * execution with no CPU-visible effect until a flush point, so it can run on
 * a worker while the CPU executes guest blocks.
 *
 * Grant/drain contract (the exactness argument):
 *   - GRANT: at exactly the points the synchronous design called
 *     gcn_dsp_flush (batch cap / full-flush sites), the whole-multiple-of-6
 *     part of the debt is handed to the worker (cumulative counter,
 *     release-store + wake). The batch-cap grant does NOT wait — that window
 *     is where the overlap comes from.
 *   - DRAIN: before ANY CPU observation of core state — every path that
 *     flushed (or lazily declined with owed<min) in the synchronous design —
 *     wait until consumed == granted (acquire). At every observation point
 *     the guest therefore sees exactly the synchronous state: all cycles up
 *     to the last cap executed, the sub-min residue not. The <min lazy
 *     branch drains but does NOT grant the residue, mirroring the
 *     synchronous "skip" exactly.
 *   - The core (g_dsp, mailboxes, ARAM, control) is touched by exactly ONE
 *     thread at a time: the worker only between grant and drain, the CPU
 *     only after a drain (all dsp_lle_* MMIO calls sit behind drained
 *     paths). The consumed release/acquire pair is the fence that also
 *     covers the ucode's host DMA writes into MEM1/ARAM.
 *
 * Known divergence risk: a DSP->CPU interrupt raised INSIDE an async window
 * latches when the worker actually sets g_dsp_int_pending instead of at a
 * deterministic emulated-cycle boundary. The four standard gates usually
 * hold, but rare schedule-sensitive XFB/CPU-state outliers were observed under
 * sustained A/B work on 2026-07-13. Keep this path explicit opt-in; the exact
 * synchronous LLE path is the default. NOT an idle-skip and NOT a park: every
 * granted cycle executes on the real core, in order, exactly once. */
static volatile s64 s_thr_granted  = 0;  /* cumulative whole-of-6 PPC cycles granted  */
static volatile s64 s_thr_consumed = 0;  /* cumulative cycles the worker has executed */
static volatile s32 s_thr_quit     = 0;
static volatile s32 s_thr_parked   = 0;  /* worker is inside WaitOnAddress             */
static volatile s32 s_thr_waiting  = 0;  /* CPU drain is inside WaitOnAddress          */
static HANDLE       s_thr_h        = NULL;
static int          s_thr_on       = -1; /* GCN_DSP_THREAD=1 enables; resolved once */

/* Wake elision: WakeByAddressAll is a syscall; at ~240k grants per 12M-block
 * boot paying it unconditionally is measurable. Each side advertises when it
 * is actually parked, and the other side only syscalls then. The advertise-
 * then-WaitOnAddress order makes the elision safe: if the flag read races to
 * 0 while the sleeper is between advertising and sleeping, the sleeper's
 * WaitOnAddress compare sees the already-updated counter and returns
 * immediately — a missed wake is impossible by construction. */
static DWORD WINAPI dsp_thread_main(LPVOID param) {
    (void)param;
    s64 consumed = 0;   /* worker-private mirror of s_thr_consumed */
    for (;;) {
        s64 granted = __atomic_load_n(&s_thr_granted, __ATOMIC_ACQUIRE);
        if (granted == consumed) {
            if (__atomic_load_n(&s_thr_quit, __ATOMIC_ACQUIRE)) return 0;
            int spins = 0;
            for (;;) {
                granted = __atomic_load_n(&s_thr_granted, __ATOMIC_ACQUIRE);
                if (granted != consumed ||
                    __atomic_load_n(&s_thr_quit, __ATOMIC_ACQUIRE)) break;
                if (++spins < 16384) { _mm_pause(); continue; }
                s64 expect = granted;
                __atomic_store_n(&s_thr_parked, 1, __ATOMIC_SEQ_CST);
                WaitOnAddress((volatile VOID*)&s_thr_granted, &expect,
                              sizeof expect, INFINITE);
                __atomic_store_n(&s_thr_parked, 0, __ATOMIC_SEQ_CST);
            }
            continue;
        }
        dsp_lle_update((int)(granted - consumed));
        consumed = granted;
        __atomic_store_n(&s_thr_consumed, consumed, __ATOMIC_RELEASE);
        if (__atomic_load_n(&s_thr_waiting, __ATOMIC_SEQ_CST))
            WakeByAddressAll((PVOID)&s_thr_consumed);
    }
}

static int dsp_thread_on(void) {
    if (s_thr_on < 0) {
        /* Explicit opt-in: worker completion can move mailbox-interrupt
         * publication according to host scheduling. The synchronous LLE path
         * is the deterministic default; GCN_DSP_THREAD=1 is experimental. */
        const char* e = getenv("GCN_DSP_THREAD");
        s_thr_on = (e && e[0] == '1') ? 1 : 0;
        if (s_thr_on) {
            s_thr_h = CreateThread(NULL, 0, dsp_thread_main, NULL, 0, NULL);
            if (!s_thr_h) s_thr_on = 0;   /* creation failure = stay synchronous */
        }
    }
    return s_thr_on;
}

static void dsp_thread_grant(u32 whole_ppc) {
    __atomic_store_n(&s_thr_granted, s_thr_granted + (s64)whole_ppc,
                     __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_thr_parked, __ATOMIC_SEQ_CST))
        WakeByAddressAll((PVOID)&s_thr_granted);
}

static void dsp_thread_drain(void) {
    s64 granted = __atomic_load_n(&s_thr_granted, __ATOMIC_RELAXED);
    int spins = 0;
    for (;;) {
        s64 c = __atomic_load_n(&s_thr_consumed, __ATOMIC_ACQUIRE);
        if (c == granted) return;
        if (++spins < 16384) { _mm_pause(); continue; }
        s64 expect = c;
        __atomic_store_n(&s_thr_waiting, 1, __ATOMIC_SEQ_CST);
        WaitOnAddress((volatile VOID*)&s_thr_consumed, &expect,
                      sizeof expect, INFINITE);
        __atomic_store_n(&s_thr_waiting, 0, __ATOMIC_SEQ_CST);
    }
}

void gcn_dsp_set_irq(GcnDsp* dsp, GcnDspIrqFn fn, void* user) {
    dsp->irq = fn;
    dsp->irq_user = user;
}

void gcn_dsp_set_audio_sink(GcnDsp* dsp,
                            void (*sink)(void* user, const u8* samples, u32 bytes),
                            void* user) {
    dsp->audio_sink = sink;
    dsp->audio_user = user;
}

/* ---- interrupt line (DSP.cpp UpdateInterrupts:372-382) ----
 * Dolphin computes ((Hex>>1) & Hex & (INT_DSP|INT_ARAM|INT_AID)) != 0 over the
 * control register — i.e. the line is high when any interrupt status bit is set
 * AND the enable mask bit directly to its left is set. All six of those bits
 * (AID/ARAM/DSP interrupt + masks) live outside DSP_CONTROL_MASK, so they are
 * CPU-side (dsp->csr) and the line reads entirely from it. Pushed to the PI DSP
 * cause line on EVERY evaluation (not just edges): PI INTSR is W1C, so an
 * acked-but-still-asserted level must re-appear — exactly as in Dolphin. */
static void dsp_update_interrupts(GcnDsp* dsp) {
    int level =
        ((dsp->csr & GCN_DSP_CSR_AIDINT) && (dsp->csr & GCN_DSP_CSR_AIDMASK)) ||
        ((dsp->csr & GCN_DSP_CSR_ARINT)  && (dsp->csr & GCN_DSP_CSR_ARMASK))  ||
        ((dsp->csr & GCN_DSP_CSR_DSPINT) && (dsp->csr & GCN_DSP_CSR_DSPMASK));

    if (dsp->irq)
        dsp->irq(dsp->irq_user, level);
    if (level != dsp->irq_level) {
        gcn_ring_event(level ? GCN_EV_IRQ_RAISE : GCN_EV_IRQ_CLEAR,
                       /*source*/ 6u /* PI cause bit index of INT_CAUSE_DSP=0x40 */,
                       0u, 0u);
        dsp->irq_level = level;
    }
}

/* Consume a DSP->CPU mailbox interrupt request from the real core (if any) and
 * latch the CPU-side DSP mailbox interrupt status bit. Consume-once in the core;
 * the latch persists in dsp->csr until the guest W1C-acks it. */
static void dsp_latch_mailbox_int(GcnDsp* dsp) {
    if (dsp_lle_take_interrupt())
        dsp->csr |= GCN_DSP_CSR_DSPINT;
}

void gcn_dsp_init(GcnDsp* dsp, const u8* irom, const u8* coef, u8* mem1, u32 mem1_size) {
    memset(dsp, 0, sizeof *dsp);
    dsp->mem1 = mem1;
    dsp->mem1_size = mem1_size;
    dsp_lle_init(irom, coef, mem1, mem1_size);   /* brings up the core + ARAM */
    s_dsp = dsp;
}

void gcn_dsp_free(GcnDsp* dsp) {
    (void)dsp;
    if (s_thr_on == 1 && s_thr_h) {
        /* Retire the worker BEFORE the core it executes on is deleted. */
        dsp_thread_drain();
        __atomic_store_n(&s_thr_quit, 1, __ATOMIC_RELEASE);
        WakeByAddressAll((PVOID)&s_thr_granted);
        WaitForSingleObject(s_thr_h, 2000);
        CloseHandle(s_thr_h);
        s_thr_h = NULL;
    }
    dsp_lle_shutdown();   /* frees the shared ARAM */
}

/* Audio-DMA block pump (DSP.cpp UpdateAudioDMA:424-454): every audio-block
 * period, consume one 32-byte block; when the count runs out, auto-reload from
 * the programmed source/length and raise the AID interrupt. The one-block
 * "fifo start" interrupt latched at enable time (aid_int_pending — Dolphin
 * schedules it 200 cycles after the CONTROL_LEN write) also lands here. */
static void dsp_aid_tick(GcnDsp* dsp, u32 core_cycles) {
    if (dsp->aid_int_pending) {
        dsp->aid_int_pending = 0;
        dsp->csr |= GCN_DSP_CSR_AIDINT;
    }
    dsp->aid_accum += core_cycles;
    if (dsp->aid_accum < GCN_DSP_AID_CORE_CYCLES_PER_BLOCK)
        return;
    dsp->aid_accum -= GCN_DSP_AID_CORE_CYCLES_PER_BLOCK;
    if (!(dsp->aid_ctrl & GCN_DSP_AID_ENABLE))
        return;
    /* Dolphin sends the current 32-byte block to the external audio FIFO
     * before updating the zero-based countdown/address. Keep that exact
     * ordering: the interrupt handler may already have programmed the next
     * autoload buffer while this one is still playing. */
    if (dsp->audio_sink && dsp->mem1 &&
        (u64)dsp->aid_cur_addr + 32u <= dsp->mem1_size) {
        dsp->audio_sink(dsp->audio_user, dsp->mem1 + dsp->aid_cur_addr, 32u);
    }
    if (dsp->aid_blocks_left != 0) {
        dsp->aid_blocks_left--;
        dsp->aid_cur_addr += 32;
    }
    if (dsp->aid_blocks_left == 0) {
        dsp->aid_cur_addr    = dsp->aid_source;
        dsp->aid_blocks_left = (u16)(dsp->aid_ctrl & GCN_DSP_AID_NUMBLOCKS);
        dsp->csr |= GCN_DSP_CSR_AIDINT;
    }
}

/* Run all owed DSP cycles now (declared in dsp.h). Called before any CPU
 * observation of DSP state: DSP MMIO read/write (below), PI INTSR/INTMR
 * (pi.c), and the debug_server dsp_state query — the exact points where the
 * guest (or the debugger) could otherwise see a batched-but-not-yet-run core.
 * No-op when batching is off (s_owed_ppc stays 0, see gcn_dsp_tick) or when
 * nothing is owed. dsp_lle_update() itself divides by 6 for the DSP:CPU clock
 * ratio (dsp_lle_c.cpp:132); truncating that division away here would lose
 * sub-/6 cycles at every flush and change the total DSP cycles executed over
 * the run, so only the WHOLE multiple-of-6 part of the debt is run now and the
 * remainder stays owed — the same total executes regardless of where flushes
 * land, only call granularity changes.
 *
 * Do NOT add flush call sites beyond the ones enumerated above (no
 * EE-transition hook, no AI regs, no VI): the per-block s_batch_ppc cap
 * already bounds how stale DSP-driven state can get on any path that isn't a
 * direct observation. */
void gcn_dsp_flush(void) {
    /* Threaded (GCN_DSP_THREAD=1): quiesce any in-flight batch-cap grant
     * first, then run the residue inline exactly like the synchronous
     * design. Full flushes carry small residues (the tick cap grants
     * everything >= 4096 away asynchronously), and a grant+drain round trip
     * through a parked worker costs a kernel wake — inline is both simpler
     * and faster here. With the worker drained the core is exclusively
     * ours, so the inline dsp_lle_update below is race-free. */
    if (dsp_thread_on()) dsp_thread_drain();

    u32 whole = s_owed_ppc - (s_owed_ppc % 6u);   /* keep the sub-/6 residue owed */
    if (whole == 0u) return;

    s_owed_ppc -= whole;
    dsp_lle_update((int)whole);
    /* A flush can complete a mailbox post or raise the core's interrupt
     * request; project it to dsp->csr and the PI line immediately, exactly as
     * the per-block path in gcn_dsp_tick does after every dsp_lle_update. The
     * interrupt chain invariant is that a core step is always immediately
     * followed by latch+update — a flush that skipped this would leave a
     * freshly-posted mailbox or ARAM/AID condition visible to the caller (e.g.
     * gcn_dsp_read of MBOX_OUT, just below) while the CSR/PI line still
     * reported the pre-flush (stale) state until the next per-block tick. */
    if (s_dsp) {
        dsp_latch_mailbox_int(s_dsp);
        dsp_update_interrupts(s_dsp);
    }
}

/* Lazy observation flush (derived cycle accuracy follow-up). Under real
 * per-block cycle costs the guest's genuine busy-wait loops observe DSP-
 * coupled state (PI INTSR, the DSP->CPU mailbox) every few cycles — many
 * times more often per emulated second than the uniform-charging world the
 * flush-per-observation contract was validated in — and every flush pays
 * ~750ns of RunCycles entry for a core that then mostly discards its budget
 * at the idle check ([dsp-stats]: 97% idle-discarded). Rather than classify
 * loops (an idle-park attempt hung the boot: analyzer-marked "idle" loops
 * can be SELF-PROGRESSING countdown waits that Dolphin's per-call preamble
 * steps still advance), bound the flush RATE quantitatively: run the core
 * only once >= GCN_DSP_FLUSH_MIN owed PPC cycles have accrued (default 96 —
 * exactly one uniform-world block, i.e. the same worst-case staleness every
 * observation already had when each poll-loop iteration charged 96 cycles
 * and the DSP advanced once per iteration). Progress is preserved (the core
 * still runs every 96 owed cycles and at the gcn_dsp_tick cap), writes are
 * NEVER lazy (gcn_dsp_write keeps the full flush — mail ordering), and
 * GCN_DSP_FLUSH_MIN=0 restores flush-every-observation for A/B. */
static u32 s_flush_min = 0;   /* 0 = flush-every-observation (the legacy
                               * contract, and the UNIFORM-mode default);
                               * dispatch.c raises it via
                               * gcn_dsp_set_flush_min ONLY in derived mode. */

void gcn_dsp_set_flush_min(u32 ppc_cycles) {
    const char* e = getenv("GCN_DSP_FLUSH_MIN");   /* explicit override wins */
    s_flush_min = (e && *e) ? (u32)strtoul(e, NULL, 0) : ppc_cycles;
}

void gcn_dsp_flush_lazy(void) {
    if (s_owed_ppc < s_flush_min) {
        /* Declined lazy flush ahead of a DSP-block MMIO read: the caller
         * reads core state (mailboxes/CSR) directly next, so an in-flight
         * grant must be waited out — both for coherence (sync semantics =
         * everything up to the last cap executed) and because the worker
         * would otherwise be mutating the core under the read. Granting
         * nothing preserves the residue-unexecuted contract. */
        if (dsp_thread_on()) dsp_thread_drain();
        return;
    }
    gcn_dsp_flush();
}

void gcn_dsp_flush_lazy_pi(void) {
    if (s_owed_ppc < s_flush_min) return;   /* no core access follows — see dsp.h */
    gcn_dsp_flush();
}

void gcn_dsp_tick(u32 ppc_cycles, u32 aid_core_cycles) {
    /* bisect guard, read once (getenv per block was a measurable hot-path cost). */
    static int s_notick = -1;
    if (s_notick < 0) s_notick = getenv("GCN_DSP_NOTICK") ? 1 : 0;
    if (s_notick) return;

    /* Batching config, read once (same lazy pattern as s_notick above). */
    if (s_batch < 0) {
        const char* e = getenv("GCN_DSP_BATCH");
        s_batch = (e && !strcmp(e, "0")) ? 0 : 1;
        const char* k = getenv("GCN_DSP_BATCH_PPC");
        if (k && *k) {
            u32 v = (u32)strtoul(k, NULL, 0);
            s_batch_ppc = (v < 6u) ? 6u : v;   /* a cap under 6 could never flush a whole cycle */
        }
    }

    if (s_batch) {
        /* Accrue debt; run it in one larger RunCycles call once it crosses the
         * cap, rather than a tiny one every block. RunCycles returning early on
         * an idle/halted core (dsp_lle_c.cpp:129) reports the budget consumed
         * BY DESIGN — a waiting DSP discards wall-clock rather than banking it
         * — so debt is cleared at flush unconditionally; it is never re-credited
         * based on how much the core actually stepped. */
        s_owed_ppc += ppc_cycles;
        if (s_owed_ppc >= s_batch_ppc) {
            if (dsp_thread_on()) {
                /* The overlap point: grant without waiting. The worker burns
                 * the (overwhelmingly idle-loop) cycles while the CPU keeps
                 * executing guest blocks; every core-observing path drains.
                 * Halted-core windows are discarded outright when the worker
                 * is idle (safe to peek control_reg then): dsp_lle_update's
                 * halt-skip would no-op the whole grant anyway, so skipping
                 * the round trip is byte-identical and saves the wake. */
                u32 whole = s_owed_ppc - (s_owed_ppc % 6u);
                if (whole) {
                    if (__atomic_load_n(&s_thr_consumed, __ATOMIC_ACQUIRE) ==
                            __atomic_load_n(&s_thr_granted, __ATOMIC_RELAXED) &&
                        dsp_lle_halted()) {
                        s_owed_ppc -= whole;
                    } else {
                        s_owed_ppc -= whole;
                        dsp_thread_grant(whole);
                    }
                }
            } else {
                gcn_dsp_flush();
            }
        }
    } else {
        dsp_lle_update((int)ppc_cycles);   /* legacy per-block path, byte-identical */
    }

    /* Latch a pending DSP->CPU mailbox interrupt and (re)assert the PI line every
     * block. This is what lets the interrupt reach PI while the guest sits in the
     * OS idle loop waiting on it — delivery no longer depends on the guest
     * happening to read the CSR (see read_csr). Level-triggered re-assertion also
     * restores the line after a W1C ack of a still-pending source. This cheap
     * CPU-side bookkeeping stays PER BLOCK regardless of batching — only the
     * expensive DSP core step above is deferred. */
    if (s_dsp) {
        dsp_latch_mailbox_int(s_dsp);
        dsp_aid_tick(s_dsp, aid_core_cycles);
        dsp_update_interrupts(s_dsp);
    }
}

static u16 idx16(u32 off) { return (u16)(off >> 1); }

/* Perform the ARAM DMA the guest just kicked (Flipper AR engine, CPU-side). See
 * the header for the register layout; direction bit 31 of AR_CNT. */
static void aram_dma_kick(GcnDsp* dsp, CPUState* cpu, u32 cnt) {
    u32 mmaddr = ((u32)dsp->reg[idx16(GCN_DSP_AR_MMADDR)]     << 16) |
                  (u32)dsp->reg[idx16(GCN_DSP_AR_MMADDR) + 1];
    u32 araddr = ((u32)dsp->reg[idx16(GCN_DSP_AR_ARADDR)]     << 16) |
                  (u32)dsp->reg[idx16(GCN_DSP_AR_ARADDR) + 1];
    u32 len    = cnt & 0x03FFFFFFu;
    int to_aram = ((cnt & 0x80000000u) == 0);
    u8* aram = dsp_lle_aram();          /* shared with the DSP accelerator */
    u32 aram_size = dsp_lle_aram_size();

    if (aram && len &&
        (u64)mmaddr + len <= cpu->ram_size &&
        (u64)araddr + len <= aram_size) {
        if (to_aram) {
            memcpy(aram + araddr, cpu->ram + mmaddr, len);
        } else {
            gcn_ring_watch_check_span(mmaddr, len, 0xD5BD5B00u); /* [gcn-watch] */
            memcpy(cpu->ram + mmaddr, aram + araddr, len);
        }
    }

    dsp->dma_active = true;
    dsp->dma_polls_left = GCN_DSP_ARAM_DMA_NOMINAL_POLLS;
    gcn_ring_event(GCN_EV_ARAM_DMA, (u32)to_aram, len, cpu->pc);
}

/* CPU-side CSR bits (masks, interrupt status, ARAM-DMA bit), combined with the
 * DSP-owned bits read from the real core. */
static u32 read_csr(GcnDsp* dsp) {
    /* A DSP->CPU interrupt request from the core latches the mailbox int bit
     * (consume-once; the tick also polls this — whichever runs first wins, the
     * latch in dsp->csr is shared). */
    dsp_latch_mailbox_int(dsp);

    u32 v = (dsp->csr & ~GCN_DSP_CONTROL_MASK)
          | (dsp->dma_active ? GCN_DSP_CSR_DMA : 0u)
          | ((u32)dsp_lle_read_control() & GCN_DSP_CONTROL_MASK);

    if (dsp->dma_active && --dsp->dma_polls_left == 0) {
        dsp->dma_active = false;
        dsp->csr |= GCN_DSP_CSR_ARINT;   /* AR DMA complete raises the ARAM int */
    }
    /* Any of the six interrupt/mask bits may have just changed (DSPINT latched
     * above, ARINT on DMA completion): re-evaluate the line. */
    dsp_update_interrupts(dsp);
    return v;
}

static void write_csr(GcnDsp* dsp, u16 w) {
    /* DSP-owned bits (reset/halt/init) drive the real core. */
    dsp_lle_write_control((uint16_t)(w & GCN_DSP_CONTROL_MASK));

    /* CPU-side interrupt status is write-1-to-clear. */
    dsp->csr &= (u16)~(w & (GCN_DSP_CSR_AIDINT | GCN_DSP_CSR_ARINT | GCN_DSP_CSR_DSPINT));
    /* CPU-side interrupt masks are stored. */
    const u16 MASKS = GCN_DSP_CSR_AIDMASK | GCN_DSP_CSR_ARMASK | GCN_DSP_CSR_DSPMASK;
    dsp->csr = (u16)((dsp->csr & ~MASKS) | (w & MASKS));
    /* W1C acks and mask changes both move the line (DSP.cpp write handler ends in
     * UpdateInterrupts:301). */
    dsp_update_interrupts(dsp);
}

/* Audio-DMA 16-bit register read (DSP.cpp RegisterMMIO:314-361). */
static u32 read16_aid(GcnDsp* dsp, u32 off) {
    switch (off) {
    case GCN_DSP_AID_START_HI:   return dsp->aid_source >> 16;
    case GCN_DSP_AID_START_LO:   return dsp->aid_source & 0xFFFFu;
    case GCN_DSP_AID_CTRL:       return dsp->aid_ctrl;
    case GCN_DSP_AID_BLOCKS_LEFT:
        /* Zero-based countdown (DSP.cpp:354-359). */
        return dsp->aid_blocks_left > 0 ? (u32)dsp->aid_blocks_left - 1u : 0u;
    default:                     return dsp->reg[idx16(off)];
    }
}

u32 gcn_dsp_read(void* user, CPUState* cpu, u32 addr, u8 size) {
    (void)cpu; (void)size;
    GcnDsp* dsp = (GcnDsp*)user;
    u32 off = addr - GCN_DSP_BASE;
    /* Catch the core up before any observation: mailbox reads (both boxes,
     * both halves), CSR read, and the AR/AID register reads below must all see
     * a core that has run every PPC cycle owed to it so far, not one still
     * sitting on batched-but-unrun debt. */
    gcn_dsp_flush_lazy();
    switch (off) {
    case GCN_DSP_MBOX_IN_H:  return dsp_lle_read_mbox_hi(1);  /* CPU->DSP box */
    case GCN_DSP_MBOX_IN_L:  return dsp_lle_read_mbox_lo(1);
    case GCN_DSP_MBOX_OUT_H: return dsp_lle_read_mbox_hi(0);  /* DSP->CPU box */
    case GCN_DSP_MBOX_OUT_L: return dsp_lle_read_mbox_lo(0);
    case GCN_DSP_CSR:        return read_csr(dsp);
    default: break;
    }
    if (off >= GCN_DSP_AID_START_HI && off <= GCN_DSP_AID_BLOCKS_LEFT) {
        if (size == 4)
            return (read16_aid(dsp, off) << 16) | read16_aid(dsp, off + 2u);
        return read16_aid(dsp, off);
    }
    if (size == 4)
        return ((u32)dsp->reg[idx16(off)] << 16) | (u32)dsp->reg[idx16(off) + 1];
    return dsp->reg[idx16(off)];
}

/* Audio-DMA 16-bit register write (DSP.cpp RegisterMMIO:314-361). The GCN
 * address restriction masks are DSP.cpp:200-203; BLOCKS_LEFT is InvalidWrite. */
static void write16_aid(GcnDsp* dsp, u32 off, u16 value) {
    switch (off) {
    case GCN_DSP_AID_START_HI:
        dsp->aid_source = (dsp->aid_source & 0x0000FFFFu) | ((u32)(value & 0x03FFu) << 16);
        return;
    case GCN_DSP_AID_START_LO:
        dsp->aid_source = (dsp->aid_source & 0xFFFF0000u) | (value & 0xFFE0u);
        return;
    case GCN_DSP_AID_CTRL: {
        /* On a 0->1 enable edge, load the transfer and raise the fifo-start AID
         * interrupt one tick later (DSP.cpp:326-347). While already enabled the
         * new values autoload when the current transfer wraps. */
        int already_enabled = (dsp->aid_ctrl & GCN_DSP_AID_ENABLE) != 0;
        dsp->aid_ctrl = value;
        if (!already_enabled && (value & GCN_DSP_AID_ENABLE)) {
            dsp->aid_cur_addr    = dsp->aid_source;
            dsp->aid_blocks_left = (u16)(value & GCN_DSP_AID_NUMBLOCKS);
            dsp->aid_int_pending = 1;
        }
        return;
    }
    case GCN_DSP_AID_BLOCKS_LEFT:
        return;                       /* read-only (DSP.cpp:361 InvalidWrite) */
    default:
        dsp->reg[idx16(off)] = value;
        return;
    }
}

void gcn_dsp_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size) {
    GcnDsp* dsp = (GcnDsp*)user;
    u32 off = addr - GCN_DSP_BASE;

    /* Catch the core up before any write that a batched core could observe
     * out of order: a mailbox write must not later be consumed by a core
     * still running in its own past; a CSR write's DSP-owned bits run
     * CheckExternalInterrupt synchronously (dsp_lle_write_control, below) and
     * must act on a caught-up core; and the AR_CNT ARAM-DMA kick memcpys live
     * ARAM the DSP accelerator writes during RunCycles, so the copy below must
     * see a caught-up ARAM. */
    gcn_dsp_flush();

    switch (off) {
    case GCN_DSP_MBOX_IN_H:  /* CPU->DSP mail (command) — a real handshake edge */
        gcn_ring_event(GCN_EV_DSP_MAIL, /*cpu->dsp*/ 1u, value, cpu->pc);
        dsp_lle_write_mbox_hi((uint16_t)value); return;
    case GCN_DSP_MBOX_IN_L:  dsp_lle_write_mbox_lo((uint16_t)value); return;
    case GCN_DSP_MBOX_OUT_H: /* CPU cannot write the DSP->CPU mailbox */ return;
    case GCN_DSP_MBOX_OUT_L: return;
    case GCN_DSP_CSR:        write_csr(dsp, (u16)value); return;
    default: break;
    }

    if (off >= GCN_DSP_AID_START_HI && off <= GCN_DSP_AID_BLOCKS_LEFT) {
        if (size == 4) {
            write16_aid(dsp, off, (u16)(value >> 16));
            write16_aid(dsp, off + 2u, (u16)value);
        } else {
            write16_aid(dsp, off, (u16)value);
        }
        return;
    }

    /* store into the generic 16-bit backing (32-bit writes fill two halves) */
    if (size == 4) {
        dsp->reg[idx16(off)]     = (u16)(value >> 16);
        dsp->reg[idx16(off) + 1] = (u16)value;
    } else {
        dsp->reg[idx16(off)] = (u16)value;
    }

    /* the AR_CNT write kicks the ARAM DMA (MMADDR/ARADDR were set just before) */
    if (off == GCN_DSP_AR_CNT)
        aram_dma_kick(dsp, cpu, value);
}
