/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Serial Interface (SI) model — controllers + the SI/EXI clock lock.
 *
 * The SI drives the four controller ports. Register + bit semantics are
 * transcribed from Dolphin's SerialInterface (oracle/dolphin/.../SI/SI.{h,cpp}
 * and SI_DeviceGCController.cpp), our independent oracle.
 *
 * Two things the IPL boot path exercises:
 *   - SIEXILK (0xCC00643C): the SI/EXI clock lock — plain read/write control
 *     register (early boot writes it and reads it back).
 *   - SICOMCSR (0xCC006434) RDSTINT: the IPL reads the communication CSR and the
 *     oracle returns bit 28 (RDSTINT) set. RDSTINT is DERIVED: it is 1 whenever
 *     any channel's RDST ("read status") bit is set (SI.cpp UpdateInterrupts).
 *     RDST is latched by Dolphin's continuous VI-driven background poll whenever
 *     a connected controller's GetData() returns Success (SI.cpp:536) and cleared
 *     by reading that channel's input buffer (SI.cpp:344-357).
 *
 * Beam-scheduled poll model (retires the earlier timing-free replay). Dolphin
 * does NOT poll on register reads: SICOMCSR and SISR are plain DirectReads of
 * stored state (SI.cpp:364,393) — a read never mutates anything. The poll
 * itself is scheduled off the VI beam (VideoInterface.cpp:950-968), which we
 * already model (vi.c half_line_count / vi_advance_halfline):
 *   - Every halfline, vi.c fires its si_poll_hook (vi.h GcnViSiPollFn) with the
 *     CURRENT (pre-increment) half_line_count and whether this halfline is a
 *     field boundary — unconditionally, independent of any SI register access.
 *     gcn_si_beam_poll (below) is that hook.
 *   - If half_line_count == next_poll_halfline: run one poll (si_poll: GetData
 *     on every channel, SI.cpp:529-548) then end in UpdateInterrupts
 *     (SI.cpp:550, our si_update_interrupts), then reschedule
 *     next_poll_halfline += 2*SIPOLL.X halflines (VideoInterface.cpp:959,
 *     SI.cpp GetPollXLines — SIPOLL.X is USIPoll bits 16-25, GCN_SI_POLL_X_*
 *     below; EN0-3 do NOT gate this poll, they only gate SendCommand on an
 *     SISR.WR write). X multiplies rather than divides, so X==0 just parks the
 *     schedule at the current halfline (next matched only by the field-
 *     boundary reset below) — never a div-by-zero or a spin.
 *   - If this halfline is a field boundary: next_poll_halfline is
 *     unconditionally reset to half_line_count + GCN_SI_NUM_HALF_LINES_FOR_POLL
 *     (VideoInterface.cpp:968, "first results start at vsync"), overriding
 *     whatever the periodic reschedule above just computed — checked in that
 *     order, matching Update()'s statement order exactly.
 * Dolphin keeps this schedule state (m_half_line_of_next_si_poll) on the VI
 * object, but nothing feeds it besides half_line_count and the boundary flag
 * (both passed into the hook), so it lives here (next_poll_halfline, below)
 * instead — vi.c stays free of any si.h dependency, mirroring the
 * GcnPiFifoResetFn / irq-trampoline pattern. Per SI.cpp:529-548, one poll:
 *   - a connected controller answers GetData()==Success: latch RDST and deposit
 *     its pad report into that channel's in_hi/in_lo (SI.cpp:536, the same
 *     neutral bytes CMD_DIRECT returns — factored into si_pad_report so the
 *     polled and direct-command paths cannot drift). RDST is transiently 0
 *     between an input-buffer read and the next poll that re-arms it.
 *   - a channel with no device answers GetData()==ErrorNoResponse (SI_DeviceNull
 *     .cpp:20): SetNoResponse latches NOREP for that channel (SI.cpp:538-539) —
 *     a genuine per-channel latch (GcnSiChannel.norep), same lifecycle as RDST:
 *     set at poll time, cleared by a SISR W1C write (SI.cpp:398-433, mirrored
 *     in gcn_si_write), and re-set only by the NEXT scheduled poll — no longer
 *     re-derived from `connected` on every read (that would show NOREP live
 *     regardless of the beam schedule, which is not what Dolphin does). TSTART
 *     against a disconnected channel latches it too (RunSIBuffer's negative-
 *     response path also calls SetNoResponse, SI.cpp:191-192) — si_run_buffer
 *     mirrors that.
 * Dolphin's default config connects a standard controller on channel 0 only, so
 * ch0 reads RDST=1 (=> RDSTINT) with the neutral report once a poll has run,
 * and ch1-3 read NOREP. A wrong controller-presence choice diverges loudly
 * from the oracle — presence here is the modeled hardware state, never a fudge.
 * gcn_si_init seeds next_poll_halfline = GCN_SI_NUM_HALF_LINES_FOR_POLL (the
 * same "first sampling starts at vsync" Dolphin seeds at Preset — VI.cpp:175)
 * WITHOUT an extra manual poll call: the beam reaches that halfline (and any
 * SIPOLL-default periodic polls after it, SI.cpp:275-276 default X=492, also
 * modeled at init below) well before the IPL's first SISR read deep into BS2
 * init, so RDST0 is genuinely poll-latched by then — not special-cased.
 */
#ifndef GCN_SI_SI_H
#define GCN_SI_SI_H

#include "cpu/cpu.h"

#define GCN_SI_BASE      0xCC006400u
#define GCN_SI_SIZE      0x100u        /* channel regs + control + 128B I/O buf */
#define GCN_SI_CHANNELS  4u
#define GCN_SI_CH_STRIDE 0x0Cu         /* out / in_hi / in_lo per channel */

/* Register offsets (YAGCD / Dolphin SI.cpp:37-56). */
#define GCN_SI_POLL      0x30u         /* SIPOLL   */
#define GCN_SI_COMCSR    0x34u         /* SICOMCSR: communication control/status */
#define GCN_SI_SISR      0x38u         /* SISR: status (per-channel RDST + errors) */
#define GCN_SI_EXILK     0x3Cu         /* SIEXILK: SI/EXI clock lock */
#define GCN_SI_IOBUF     0x80u         /* SI I/O buffer (128 bytes) */

/* SIPOLL bits (SI.h USIPoll:137-153). X is "polls per X lines" — GetPollXLines
 * returns it raw, un-gated by EN0-3 (those only gate SendCommand on WR). */
#define GCN_SI_POLL_X_SHIFT   16u
#define GCN_SI_POLL_X_MASK    0x3FFu   /* BitField<16,10,u32> */
/* Dolphin's power-on default (SI.cpp Init:275-276), before the IPL ever
 * writes SIPOLL: X=492 polls-per-line period, all EN/VBCPY/Y bits 0. */
#define GCN_SI_POLL_DEFAULT   (492u << GCN_SI_POLL_X_SHIFT)
/* VideoInterface.cpp:56: NUM_HALF_LINES_FOR_SI_POLL = (7*2)+1 — halflines from
 * a field boundary to that field's first scheduled poll. */
#define GCN_SI_NUM_HALF_LINES_FOR_POLL 15u

/* SICOMCSR bits (raw u32; SI.h:156-180). */
#define GCN_SI_CSR_TSTART     0x00000001u
#define GCN_SI_CSR_CHANNEL    0x00000006u  /* transfer channel select */
#define GCN_SI_CSR_INLNGTH    0x00007F00u
#define GCN_SI_CSR_OUTLNGTH   0x007F0000u
#define GCN_SI_CSR_RDSTINTMSK 0x08000000u
#define GCN_SI_CSR_RDSTINT    0x10000000u  /* read-status interrupt (derived) */
#define GCN_SI_CSR_COMERR     0x20000000u  /* communication error (no response) */
#define GCN_SI_CSR_TCINTMSK   0x40000000u
#define GCN_SI_CSR_TCINT      0x80000000u  /* transfer-complete interrupt (W1C) */

/* Writable/persistent SICOMCSR fields (the rest are status or W1C). */
#define GCN_SI_CSR_PERSIST \
    (GCN_SI_CSR_CHANNEL | GCN_SI_CSR_INLNGTH | GCN_SI_CSR_OUTLNGTH | \
     GCN_SI_CSR_RDSTINTMSK | GCN_SI_CSR_TCINTMSK)

/* SISR bits (SI.h:183-219). WR triggers SendCommand on all channels. */
#define GCN_SI_SR_WR          0x80000000u
/* RDST bit for channel n (SI.cpp GetRDSTBit): ch0=0x20000000 .. ch3=0x20. */
#define GCN_SI_RDST_BIT(n)    (0x20000000u >> ((n) * 8u))
/* NOREP (no-response error) bit for channel n (SI.h USIStatusReg NOREP0..3:
 * bit27,19,11,3): ch0=0x08000000 .. ch3=0x08 — 0x08 in each per-channel byte. */
#define GCN_SI_NOREP_BIT(n)   (0x08000000u >> ((n) * 8u))

/* Controller mode (SI_DeviceGCController.h:49): default 3. */
#define GCN_SI_CTRL_MODE_DEFAULT 3u

/* GC pad button bits (SI_DeviceGCController.h PadButton / GCPadStatus.h,
 * verified against oracle/dolphin/Source/Core/InputCommon/GCPadStatus.h). ORed
 * with PAD_USE_ORIGIN (0x0080) by MapPadStatus regardless of which of these
 * are set — see GCN_SI_PAD_USE_ORIGIN below. */
#define GCN_SI_PAD_LEFT   0x0001u
#define GCN_SI_PAD_RIGHT  0x0002u
#define GCN_SI_PAD_DOWN   0x0004u
#define GCN_SI_PAD_UP     0x0008u
#define GCN_SI_PAD_Z      0x0010u
#define GCN_SI_PAD_R      0x0020u
#define GCN_SI_PAD_L      0x0040u
#define GCN_SI_PAD_A      0x0100u
#define GCN_SI_PAD_B      0x0200u
#define GCN_SI_PAD_X      0x0400u
#define GCN_SI_PAD_Y      0x0800u
#define GCN_SI_PAD_START  0x1000u
/* PAD_USE_ORIGIN (GCPadStatus.h): MapPadStatus ORs this into the button word
 * unconditionally (SI_DeviceGCController.cpp:234) — it coexists with every
 * real button bit above, it is never itself a "button pressed" signal. */
#define GCN_SI_PAD_USE_ORIGIN 0x0080u

/* Injectable pad state for one (the only currently-modeled) connected GC
 * controller — the Observability "input injection" surface (ROADMAP M3/M4).
 * Defaults to the power-on-neutral report: no buttons, sticks centered,
 * triggers released (GCPadStatus.h MAIN/C_STICK_CENTER = 0x80). si_pad_report
 * packs these into the SI_DeviceGCController mode-3 hi/lo words exactly as
 * MapPadStatus/GetData do (SI_DeviceGCController.cpp:178-236). */
typedef struct {
    u16 buttons;      /* raw GCN_SI_PAD_* bits, no PAD_USE_ORIGIN (added at report time) */
    u8  stick_x;      /* main stick X, default 0x80 (centered)       */
    u8  stick_y;      /* main stick Y, default 0x80 (centered)       */
    u8  substick_x;   /* C-stick X, default 0x80 (centered)          */
    u8  substick_y;   /* C-stick Y, default 0x80 (centered)          */
    u8  trigger_l;    /* analog L, default 0 (released)              */
    u8  trigger_r;    /* analog R, default 0 (released)               */
} GcnSiPadInput;

#define GCN_SI_PAD_INPUT_NEUTRAL { 0, 0x80, 0x80, 0x80, 0x80, 0, 0 }

typedef struct {
    u32 out;         /* SI_CHANNEL_n_OUT  (poll/command word)      */
    u32 in_hi;       /* SI_CHANNEL_n_IN_HI                          */
    u32 in_lo;       /* SI_CHANNEL_n_IN_LO                          */
    u8  connected;   /* 1 = standard GC controller present          */
    u8  rdst;        /* latched read-status for this channel        */
    u8  norep;       /* latched no-response (SetNoResponse, SI.cpp:70-87):
                       * set on a poll/TSTART that finds no device, cleared by
                       * a SISR W1C write, re-set by the NEXT poll — a real
                       * latch, not re-derived from `connected` on every read */
    u8  mode;        /* controller reporting mode                   */
} GcnSiChannel;

/* Level change on the SI->PI interrupt line (level: 1 assert, 0 deassert). */
typedef void (*GcnSiIrqFn)(void* user, int level);

typedef struct {
    GcnSiChannel ch[GCN_SI_CHANNELS];
    u32 poll;        /* SIPOLL   */
    u32 comcsr;      /* SICOMCSR persistent bits (masks/lengths/TCINT/COMERR) */
    u32 sisr;        /* SISR error/WR bits (RDST computed on read)  */
    u32 exilk;       /* SIEXILK  */
    u8  iobuf[128];  /* SI I/O buffer for TSTART transfers          */
    int irq_level;   /* last SI->PI line level (edge detect for the ring) */
    GcnSiIrqFn irq;  /* sink for the SI interrupt line (boot.c -> PI) */
    void*      irq_user;
    GcnSiPadInput input; /* debug-surface-injected report (see gcn_si_set_input) */
    u32 next_poll_halfline; /* beam halfline the next SI poll is scheduled for
                              * (Dolphin's m_half_line_of_next_si_poll, parked
                              * here — see the header's beam-schedule doc) */
} GcnSi;

void gcn_si_init(GcnSi* si);
/* Register the SI interrupt-line sink (boot.c trampoline into PI INT_CAUSE_SI). */
void gcn_si_set_irq(GcnSi* si, GcnSiIrqFn fn, void* user);
u32  gcn_si_read(void* user, CPUState* cpu, u32 addr, u8 size);
void gcn_si_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size);
/* VI-beam per-halfline hook (GcnViSiPollFn, vi.h): matches the Update() schedule
 * transcribed in the header doc above. Wired in boot.c via
 * gcn_vi_set_si_poll_hook(&vi, gcn_si_beam_poll, &si) so vi.c never includes
 * si.h. `user` is the GcnSi*. */
void gcn_si_beam_poll(void* user, u32 half_line_count, int is_at_field_boundary);

/* Replace the whole injected pad state (debug-surface setter). */
void gcn_si_set_input(GcnSi* si, const GcnSiPadInput* input);
/* Reset the injected pad state to the power-on-neutral report. */
void gcn_si_reset_input(GcnSi* si);

/* ---- debug-server singleton accessors (mirrors vi.c's s_vi / gcn_vi_xfb_info:
 * the debug server has no other handle to the live SI instance boot.c owns) ---- */
/* Apply a partial patch to the registered SI instance's injected pad state:
 * each `have_*` flag selects whether the matching field is overwritten (0 =
 * leave unchanged). `reset` (if set) restores neutral and ignores every other
 * argument. Returns 0 if no SI is registered yet (server queried before boot
 * wired it up), else 1. */
int gcn_si_debug_set_input(int have_buttons, u32 buttons,
                            int have_stick_x, u32 stick_x,
                            int have_stick_y, u32 stick_y,
                            int have_substick_x, u32 substick_x,
                            int have_substick_y, u32 substick_y,
                            int have_trigger_l, u32 trigger_l,
                            int have_trigger_r, u32 trigger_r,
                            int reset);
/* Current injected pad state of the registered SI instance. Returns 0 (out
 * left neutral) if none is registered yet. */
int gcn_si_debug_get_input(GcnSiPadInput* out);

#endif /* GCN_SI_SI_H */
