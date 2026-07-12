/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Video Interface (VI) model (impl). See include/vi/vi.h for scope. Every
 * formula is transcribed from Dolphin VideoInterface.cpp (the authoritative
 * beam/timing oracle); all derived quantities are computed on demand from the
 * guest-written registers, so there is no cached state to go stale.
 */
#include "vi/vi.h"
#include "debug/rings.h"

#include <stdio.h>
#include <string.h>

/* The dispatch loop ticks one VI (like the one DSP core); registered at init. */
static GcnVi* s_vi = NULL;

void gcn_vi_set_irq(GcnVi* vi, GcnViIrqFn fn, void* user) {
    vi->irq = fn;
    vi->irq_user = user;
}

void gcn_vi_set_si_poll_hook(GcnVi* vi, GcnViSiPollFn fn, void* user) {
    vi->si_poll_hook = fn;
    vi->si_poll_user = user;
}

/* ---- derived timing (VideoInterface.cpp GetTicksPerSample /
 *      GetTicksPerHalfLine / GetHalfLinesPer{Odd,Even}Field) ---- */

static u32 vi_hlw(const GcnVi* vi) {
    return vi->reg[GCN_VI_HTR0_LO >> 1] & 0x3FFu;
}

/* Memoized on the two raw inputs (clock_bit, hlw): both are rarely-changing VI
 * register fields, but this is called once per executed block via vi_update,
 * so the u64 division below would otherwise be a per-block hot-path cost.
 * Recompute only when the (clock_bit,hlw) key changes; the returned value is
 * always identical to the pure formula since it is re-derived from the live
 * registers whenever they differ from the cached key — this cannot desync. */
static u64 vi_ticks_per_halfline(GcnVi* vi) {
    u32 clock_bit = vi->reg[GCN_VI_CLOCK >> 1] & 1u;
    u32 hlw = vi->reg[GCN_VI_HTR0_LO >> 1] & 0x3FFu;
    u32 key = (clock_bit << 10) | hlw;
    if (key != vi->tphl_key) {
        u32 clock_hz = clock_bit ? 54000000u : 27000000u;
        u64 ticks_per_sample = 2ull * GCN_VI_CORE_CLOCK / clock_hz;
        vi->tphl_cache = ticks_per_sample * hlw;
        vi->tphl_key = key;
    }
    return vi->tphl_cache;
}

static u32 vi_halflines_field(const GcnVi* vi, u32 psb_off, u32 prb_off) {
    u32 equ = vi->reg[GCN_VI_VTR >> 1] & 0xFu;
    u32 acv = (vi->reg[GCN_VI_VTR >> 1] >> 4) & 0x3FFu;
    u32 psb = vi->reg[psb_off >> 1] & 0x3FFu;
    u32 prb = vi->reg[prb_off >> 1] & 0x3FFu;
    return 3u * equ + prb + 2u * acv + psb;
}

static u32 vi_halflines_per_frame(const GcnVi* vi) {
    return vi_halflines_field(vi, GCN_VI_VTO_HI, GCN_VI_VTO_LO) +
           vi_halflines_field(vi, GCN_VI_VTE_HI, GCN_VI_VTE_LO);
}

/* ---- interrupt line (VideoInterface.cpp UpdateInterrupts) ----
 * Level = OR over the four DI comparators of (IR_INT && IR_MASK). Pushed into
 * the PI cause word on EVERY evaluation (not just edges): PI INTSR is W1C, so
 * an acked-but-still-asserted level must re-appear, exactly as in Dolphin. */
static void vi_update_interrupts(GcnVi* vi) {
    int level = 0;
    for (u32 i = 0; i < 4; i++) {
        u16 hi = vi->reg[(GCN_VI_DI0 + 4u * i) >> 1];
        if ((hi & GCN_VI_DI_IR_INT) && (hi & GCN_VI_DI_IR_MASK))
            level = 1;
    }
    if (vi->irq)
        vi->irq(vi->irq_user, level);
    if (level != vi->irq_level) {
        gcn_ring_event(level ? GCN_EV_IRQ_RAISE : GCN_EV_IRQ_CLEAR,
                       /*source*/ 8u /* PI cause bit: VI */, 0u, 0u);
        vi->irq_level = level;
    }
}

/* ---- beam advance (VideoInterface.cpp Update, per halfline) ---- */

static void vi_advance_halfline(GcnVi* vi) {
    u32 total = vi_halflines_per_frame(vi);

    /* VideoInterface.cpp:911-912,950-968: the field-boundary check and the
     * SI-poll schedule both key off the CURRENT (pre-increment) half_line_count
     * — the increment/wrap below happens only afterward (VideoInterface.cpp:
     * 974-978). Fire the SI-poll hook here, before advancing, so si.c observes
     * the same halfline Dolphin's Update() would. even_field_begin is
     * GetHalfLinesPerOddField() — the same odd-field halfline count folded
     * into vi_halflines_per_frame below, just not summed with the even field. */
    u32 old_hl = vi->half_line_count;
    u32 even_field_begin = vi_halflines_field(vi, GCN_VI_VTO_HI, GCN_VI_VTO_LO);
    int is_at_field_boundary = (old_hl == 0u) || (old_hl == even_field_begin);
    if (vi->si_poll_hook)
        vi->si_poll_hook(vi->si_poll_user, old_hl, is_at_field_boundary);

    vi->half_line_count++;
    if (total == 0 || vi->half_line_count >= total)
        vi->half_line_count = 0;
    if (!(vi->half_line_count & 1u))
        vi->ticks_last_line_start = vi->last_cycles;

    /* Latch IR_INT on any DI comparator the beam just passed. The horizontal
     * granularity is halflines: HCT > HLW selects the second halfline. */
    u32 hlw = vi_hlw(vi);
    for (u32 i = 0; i < 4; i++) {
        u16* hi = &vi->reg[(GCN_VI_DI0 + 4u * i) >> 1];
        u16  lo = vi->reg[(GCN_VI_DI0 + 4u * i + 2u) >> 1];
        u32 vct = *hi & GCN_VI_DI_VCT_MASK;
        u32 hct = lo & GCN_VI_DI_HCT_MASK;
        u32 target_halfline = (hct > hlw) ? 1u : 0u;
        if (1u + vi->half_line_count / 2u == vct &&
            (vi->half_line_count & 1u) == target_halfline)
            *hi |= GCN_VI_DI_IR_INT;
    }
    vi_update_interrupts(vi);
}

/* Advance the beam state to `cycles_now` (the monotonic device clock). While
 * the IPL has not yet written a halfline width the beam is frozen (and time
 * must not accumulate, or the backlog would replay as a burst the moment HLW
 * lands). */
static void vi_update(GcnVi* vi, u64 cycles_now) {
    if (cycles_now < vi->last_cycles) {
        /* The device clock is monotonic by contract; a regression here means a
         * caller fed us the guest-writable TB again (the mttbl-underflow hang).
         * Rebase loudly rather than spinning for 2^64 cycles. */
        fprintf(stderr, "gcn vi: device clock went backwards (%llu < %llu) — rebasing\n",
                (unsigned long long)cycles_now, (unsigned long long)vi->last_cycles);
        vi->last_cycles = cycles_now;
        vi->ticks_last_line_start = cycles_now;
    }
    vi->now_cycles = cycles_now;
    u64 tphl = vi_ticks_per_halfline(vi);
    if (tphl == 0) {
        vi->last_cycles = cycles_now;
        vi->ticks_last_line_start = cycles_now;
        return;
    }
    while (cycles_now - vi->last_cycles >= tphl) {
        vi->last_cycles += tphl;
        vi_advance_halfline(vi);
    }
}

void gcn_vi_tick(u64 core_cycles) {
    if (s_vi)
        vi_update(s_vi, core_cycles);
}

int gcn_vi_xfb_info(u32* addr, u32* width, u32* height, u32* stride) {
    *addr = *width = *height = *stride = 0;
    GcnVi* vi = s_vi;
    if (!vi)
        return 0;
    /* XFB top-field base (VideoInterface.cpp GetXFBAddressTop:450-456): the
     * FB_LEFT_TOP u32 holds FBB[23:0] + XOFF + POFF (bit 28); POFF means the
     * stored address is >>5. */
    u32 fb_hi = vi->reg[GCN_VI_FB_LEFT_TOP_HI >> 1];
    u32 fb_lo = vi->reg[(GCN_VI_FB_LEFT_TOP_HI + 2u) >> 1];
    u32 fb    = ((fb_hi << 16) | fb_lo);
    u32 fbb   = fb & 0x00FFFFFFu;
    u32 poff  = (fb >> 28) & 1u;
    *addr = poff ? (fbb << 5) : fbb;
    /* Geometry (OutputField:814-816): width = WPL*16, height = ACV,
     * stride = STD*16*2 bytes (picture-config reg 0x48: STD[7:0], WPL[14:8]). */
    u32 pcfg = vi->reg[0x48 >> 1];
    *width  = ((pcfg >> 8) & 0x7Fu) * 16u;
    *height = (vi->reg[GCN_VI_VTR >> 1] >> 4) & 0x3FFu;
    *stride = (pcfg & 0xFFu) * 16u * 2u;
    return (*addr != 0 && *width != 0 && *height != 0) ? 1 : 0;
}

void gcn_vi_init(GcnVi* vi) {
    memset(vi, 0, sizeof *vi);

    /* Power-on register state: Dolphin VideoInterfaceManager::Init() calls
     * Preset(true) UNCONDITIONALLY (VideoInterface.cpp:96-186), so the VI
     * comes up with full NTSC timing and the beam counting from power-on —
     * the IPL's first beam poll observes a mid-frame value, not a frozen 0.
     * (The oracle confirms the layout: the IPL's own later mode-set writes
     * these very values back — 0x4769, 0x2EA/0x5140, 0x1F6/5, 0x410C,
     * 0x1107/0x1AE, 0x2828.) Values below are Preset(true) verbatim. */
    vi->reg[GCN_VI_VTR     >> 1] = 0x0006;  /* EQU=6, ACV=0                  */
    vi->reg[GCN_VI_DCR     >> 1] = GCN_VI_DCR_ENB;
    vi->reg[GCN_VI_HTR0_HI >> 1] = 0x4769;  /* HCE=105, HCS=71               */
    vi->reg[GCN_VI_HTR0_LO >> 1] = 0x01AD;  /* HLW=429                       */
    vi->reg[GCN_VI_HTR1_HI >> 1] = 0x02EA;  /* HBS640=373 (upper bits)       */
    vi->reg[GCN_VI_HTR1_LO >> 1] = 0x5140;  /* HSY=64, HBE640=162            */
    vi->reg[GCN_VI_VTO_HI  >> 1] = 0x0005;  /* odd PSB=5                     */
    vi->reg[GCN_VI_VTO_LO  >> 1] = 0x01F6;  /* odd PRB=502                   */
    vi->reg[GCN_VI_VTE_HI  >> 1] = 0x0004;  /* even PSB=4                    */
    vi->reg[GCN_VI_VTE_LO  >> 1] = 0x01F7;  /* even PRB=503                  */
    vi->reg[GCN_VI_BBOI_HI >> 1] = 0x410C;  /* BS=12, BE=520 (both halves)   */
    vi->reg[(GCN_VI_BBOI_HI + 2u) >> 1] = 0x410C;
    vi->reg[GCN_VI_BBEI_HI >> 1] = 0x40ED;  /* BS=13, BE=519 (both halves)   */
    vi->reg[(GCN_VI_BBEI_HI + 2u) >> 1] = 0x40ED;
    vi->reg[GCN_VI_DI0     >> 1] = 0x1107;  /* pre-retrace:  VCT=263, MASK=1 */
    vi->reg[(GCN_VI_DI0 + 2u) >> 1] = 0x01AE;  /* HCT=430                    */
    vi->reg[GCN_VI_DI1     >> 1] = 0x1001;  /* post-retrace: VCT=1, MASK=1   */
    vi->reg[(GCN_VI_DI1 + 2u) >> 1] = 0x0001;  /* HCT=1                      */
    vi->reg[0x48 >> 1]           = 0x2828;  /* picture config: STD=40, WPL=40 */
    vi->reg[GCN_VI_CLOCK   >> 1] = 0x0001;  /* 54 MHz (Preset: IsNTSC(region)) */

    /* Force the first vi_ticks_per_halfline() call to compute: 0xFFFFFFFF
     * can never equal a real (clock_bit<<10)|hlw key (max 0x7FF), so it
     * doesn't alias with the legitimate all-zero key. */
    vi->tphl_key = 0xFFFFFFFFu;

    s_vi = vi;
}

/* ---- 16-bit register access (specials transcribed from RegisterMMIO) ---- */

static u32 vi_read16(GcnVi* vi, u32 off) {
    switch (off) {
    case GCN_VI_VBEAM:
        return 1u + vi->half_line_count / 2u;
    case GCN_VI_HBEAM: {
        u64 tphl = vi_ticks_per_halfline(vi);
        u32 hlw = vi_hlw(vi);
        if (tphl == 0 || hlw == 0)
            return 0;
        u32 v = 1u + (u32)(hlw * (vi->now_cycles - vi->ticks_last_line_start) / tphl);
        if (v < 1u) v = 1u;
        if (v > 2u * hlw) v = 2u * hlw;
        return v;
    }
    default:
        return vi->reg[off >> 1];
    }
}

static void vi_write16(GcnVi* vi, u32 off, u16 value) {
    switch (off) {
    case GCN_VI_DCR:
        vi->reg[off >> 1] = value & GCN_VI_DCR_STORE_MASK;
        if (value & GCN_VI_DCR_RST) {
            /* Reset: clear all four DI registers, drop pending interrupts. */
            for (u32 i = 0; i < 8; i++)
                vi->reg[(GCN_VI_DI0 >> 1) + i] = 0;
            vi_update_interrupts(vi);
        }
        return;
    case GCN_VI_FB_LEFT_TOP_HI:
    case GCN_VI_FB_RIGHT_TOP_HI:
    case GCN_VI_FB_LEFT_BOTTOM_HI:
    case GCN_VI_FB_RIGHT_BOTTOM_HI:
        /* Setting any CLRPOFF bit (15:13) clears POFF (bit 12). */
        vi->reg[off >> 1] = (value & 0xE000u) ? (u16)(value & ~0x1000u) : value;
        return;
    case GCN_VI_DI0:
    case GCN_VI_DI1:
    case GCN_VI_DI2:
    case GCN_VI_DI3:
        /* HI half: VCT + IR_MASK + IR_INT. Writing IR_INT=0 is the ack. */
        vi->reg[off >> 1] = value;
        vi_update_interrupts(vi);
        return;
    case GCN_VI_VBEAM:
    case GCN_VI_HBEAM: {
        /* Beam positions are computed, not stored. Dolphin warns and ignores
         * writes ("not documented or implemented"); mirror that, once. */
        static int warned = 0;
        if (!warned) {
            fprintf(stderr, "gcn vi: write to beam position 0x%02X ignored\n", off);
            warned = 1;
        }
        return;
    }
    default:
        vi->reg[off >> 1] = value;
        return;
    }
}

u32 gcn_vi_read(void* user, CPUState* cpu, u32 addr, u8 size) {
    GcnVi* vi = (GcnVi*)user;
    u32 off = addr - GCN_VI_BASE;
    /* Beam state was advanced by gcn_vi_tick at most one block (~96 cycles)
     * ago — far below halfline resolution, so no lazy catch-up here. */
    (void)cpu;
    switch (size) {
    case 1: {
        /* Dolphin maps 8-bit reads onto the containing 16-bit register. */
        u32 v = vi_read16(vi, off & ~1u);
        return (addr & 1u) ? (v & 0xFFu) : (v >> 8);
    }
    case 2:
        return vi_read16(vi, off);
    default: /* 4: HI halfword at off, LO at off+2 */
        return (vi_read16(vi, off) << 16) | vi_read16(vi, off + 2u);
    }
}

void gcn_vi_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size) {
    GcnVi* vi = (GcnVi*)user;
    u32 off = addr - GCN_VI_BASE;
    (void)cpu;
    switch (size) {
    case 1: {
        /* Dolphin registers 8-bit VI writes as invalid; stay loud, once. */
        static int warned = 0;
        if (!warned) {
            fprintf(stderr, "gcn vi: 8-bit write 0x%08X ignored (invalid on hw)\n", addr);
            warned = 1;
        }
        return;
    }
    case 2:
        vi_write16(vi, off, (u16)value);
        return;
    default: /* 4 */
        vi_write16(vi, off, (u16)(value >> 16));
        vi_write16(vi, off + 2u, (u16)value);
        return;
    }
}
