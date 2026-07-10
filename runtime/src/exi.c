/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gcnrecomp runtime — EXI device model (impl). See include/exi/exi.h.
 *
 * Register model adapted from GXRuntime (GPL-3.0); device command decode
 * (mask-ROM / RTC / SRAM) is net-new per YAGCD, validated against the Dolphin
 * oracle's EXI stream.
 */
#include "exi/exi.h"
#include "memory/memory.h"

#include <string.h>
#include <stdio.h>

/* ---- helpers ---------------------------------------------------------------*/

static u32 csr_chip_select(u32 csr) {
    return (csr & GCN_EXI_CSR_CS_MASK) >> 7u;   /* one-hot: 1=dev0 2=dev1 4=dev2 */
}

static bool decode(u32 addr, u32* ch, u32* off) {
    if (addr < GCN_EXI_BASE || addr >= GCN_EXI_BASE + GCN_EXI_REGISTER_BYTES)
        return false;
    u32 rel = addr - GCN_EXI_BASE;
    *ch  = rel / GCN_EXI_CHANNEL_STRIDE;
    *off = rel % GCN_EXI_CHANNEL_STRIDE;
    return true;
}

/* ---- lifecycle / fixtures --------------------------------------------------*/

void gcn_exi_init(GcnExi* exi) {
    memset(exi, 0, sizeof(*exi));
    /* Channels 0 and 1 present a device: EXT (bit 12) + EXTINT (bit 11) latched.
     * The Dolphin oracle reads ch0 CSR as 0x1950/0x1958/0x1808 — all require EXT
     * set, which GXRuntime's init (EXTINT only) does NOT produce. See exi.h. */
    exi->channels[0].csr = GCN_EXI_CSR_EXT | GCN_EXI_CSR_EXTINT;
    exi->channels[1].csr = GCN_EXI_CSR_EXT | GCN_EXI_CSR_EXTINT;
    /* Channel 2 is the AD16 dev-probe target: idle, no EXT. */
}

void gcn_exi_set_rom(GcnExi* exi, const u8* rom, u32 size, u32 base) {
    exi->rom = rom; exi->rom_size = size; exi->rom_base = base;
}

void gcn_exi_set_sram(GcnExi* exi, const u8 sram[GCN_SRAM_SIZE_BYTES]) {
    memcpy(exi->sram, sram, GCN_SRAM_SIZE_BYTES);
}

void gcn_exi_set_rtc(GcnExi* exi, u32 rtc_counter) {
    exi->rtc_counter = rtc_counter;
}

/* ---- CSR write (write-1-to-clear + chip-select edge) -----------------------*/

static void reset_transaction(GcnExi* exi, u32 ch) {
    exi->op[ch] = GCN_EXI_OP_NONE;
    exi->have_cmd[ch] = 0;
    exi->rom_offset[ch] = 0;
    exi->dev_pos[ch] = 0;
}

static void write_csr(GcnExi* exi, u32 ch, u32 value) {
    GcnExiChannel* c = &exi->channels[ch];
    u32 writable = GCN_EXI_CSR_EXIINTMASK | GCN_EXI_CSR_TCINTMASK |
                   GCN_EXI_CSR_CLK_MASK | GCN_EXI_CSR_CS_MASK;
    if (ch < 2u)
        writable |= GCN_EXI_CSR_EXTINTMASK;
    c->csr = (c->csr & ~writable) | (value & writable);
    if (value & GCN_EXI_CSR_EXIINT) c->csr &= ~GCN_EXI_CSR_EXIINT;   /* w1c */
    if (value & GCN_EXI_CSR_TCINT)  c->csr &= ~GCN_EXI_CSR_TCINT;    /* w1c */
    if (ch < 2u && (value & GCN_EXI_CSR_EXTINT))
        c->csr &= ~GCN_EXI_CSR_EXTINT;                              /* w1c */
    if (ch == 0u)
        c->csr = (c->csr & ~GCN_EXI_CSR_ROMDIS) | (value & GCN_EXI_CSR_ROMDIS);

    /* A change in chip-select (select / deselect / reselect) begins a fresh
     * device transaction. */
    u32 cs = csr_chip_select(c->csr);
    if (cs != exi->prev_cs[ch]) {
        reset_transaction(exi, ch);
        exi->prev_cs[ch] = (u8)cs;
    }
}

/* ---- device transfers ------------------------------------------------------*/

/* Copy `len` bytes from the mask ROM (at exi->rom_offset[ch]) into MEM1 at the
 * channel's DMA main-memory address; or, for an immediate read, pack up to 4
 * bytes MSB-first into the immediate-data register. Advances the ROM cursor. */
static void ipl_rom_read(GcnExi* exi, CPUState* cpu, u32 ch, bool dma, u32 len) {
    GcnExiChannel* c = &exi->channels[ch];
    if (dma) {
        u32 guest = 0x80000000u | (c->mar & 0x03FFFFFFu);   /* physical -> cached */
        u32 avail = 0;
        u8* dst = gcn_mem_resolve(cpu, guest, &avail);
        for (u32 i = 0; i < len; i++) {
            u8 b = 0;
            u32 off = exi->rom_offset[ch] + i;
            if (exi->rom && off >= exi->rom_base &&
                (off - exi->rom_base) < exi->rom_size)
                b = exi->rom[off - exi->rom_base];
            if (dst && i < avail) dst[i] = b;
        }
    } else {
        u32 v = 0;
        for (u32 i = 0; i < len && i < 4u; i++) {
            u8 b = 0;
            u32 off = exi->rom_offset[ch] + i;
            if (exi->rom && off >= exi->rom_base &&
                (off - exi->rom_base) < exi->rom_size)
                b = exi->rom[off - exi->rom_base];
            v |= (u32)b << (24u - 8u * i);
        }
        c->data = v;
    }
    exi->rom_offset[ch] += len;
}

/* Serve the RTC counter / SRAM image on a read. RTC is a single 4-byte value;
 * SRAM is the 0x40-byte image, DMA'd or streamed 4 bytes per immediate read. */
static void ipl_dev_read(GcnExi* exi, CPUState* cpu, u32 ch, bool dma, u32 len) {
    GcnExiChannel* c = &exi->channels[ch];
    switch (exi->op[ch]) {
    case GCN_EXI_OP_ROM_READ:
        ipl_rom_read(exi, cpu, ch, dma, len);
        break;
    case GCN_EXI_OP_RTC_READ:
        /* Immediate 4-byte read returns the seconds-since-2000 counter. */
        c->data = exi->rtc_counter;
        break;
    case GCN_EXI_OP_SRAM_READ:
        if (dma) {
            u32 guest = 0x80000000u | (c->mar & 0x03FFFFFFu);
            u32 avail = 0;
            u8* dst = gcn_mem_resolve(cpu, guest, &avail);
            for (u32 i = 0; i < len; i++) {
                u32 p = exi->dev_pos[ch] + i;
                u8 b = (p < GCN_SRAM_SIZE_BYTES) ? exi->sram[p] : 0;
                if (dst && i < avail) dst[i] = b;
            }
        } else {
            u32 v = 0;
            for (u32 i = 0; i < len && i < 4u; i++) {
                u32 p = exi->dev_pos[ch] + i;
                u8 b = (p < GCN_SRAM_SIZE_BYTES) ? exi->sram[p] : 0;
                v |= (u32)b << (24u - 8u * i);
            }
            c->data = v;
        }
        exi->dev_pos[ch] += len;
        break;
    default:
        if (!dma) c->data = 0;   /* absent/unknown op drives 0 */
        break;
    }
}

/* IPL device (channel 0, chip-select bit 1 = device index 1): mask ROM + RTC +
 * SRAM, distinguished by the leading command word. */
static void ipl_transfer(GcnExi* exi, CPUState* cpu, u32 ch, u32 rw, bool dma, u32 len) {
    GcnExiChannel* c = &exi->channels[ch];
    if (rw == 1u) {              /* WRITE: CPU -> device */
        if (!exi->have_cmd[ch]) {
            u32 cmd = c->data;  /* IPL always issues a 4-byte command word */
            exi->have_cmd[ch] = 1;
            exi->dev_pos[ch] = 0;
            if (cmd == 0x20000000u)      exi->op[ch] = GCN_EXI_OP_RTC_READ;
            else if (cmd == 0x20000100u) exi->op[ch] = GCN_EXI_OP_SRAM_READ;
            else if (cmd == 0xA0000000u) exi->op[ch] = GCN_EXI_OP_RTC_WRITE;
            else if (cmd == 0xA0000100u) exi->op[ch] = GCN_EXI_OP_SRAM_WRITE;
            else if (!(cmd & 0x80000000u)) {
                exi->op[ch] = GCN_EXI_OP_ROM_READ;
                exi->rom_offset[ch] = cmd >> 6u;   /* YAGCD: ROM addr = cmd>>6 */
            } else {
                exi->op[ch] = GCN_EXI_OP_NONE;
            }
        } else {                /* subsequent write = data payload */
            if (exi->op[ch] == GCN_EXI_OP_RTC_WRITE) {
                exi->rtc_counter = c->data;
            } else if (exi->op[ch] == GCN_EXI_OP_SRAM_WRITE) {
                for (u32 i = 0; i < len && i < 4u; i++) {
                    u32 p = exi->dev_pos[ch] + i;
                    if (p < GCN_SRAM_SIZE_BYTES)
                        exi->sram[p] = (u8)(c->data >> (24u - 8u * i));
                }
                exi->dev_pos[ch] += len;
            }
        }
    } else {                    /* READ: device -> CPU */
        ipl_dev_read(exi, cpu, ch, dma, len);
    }
}

static void device_transfer(GcnExi* exi, CPUState* cpu, u32 ch, u32 cs,
                            u32 rw, bool dma, u32 len) {
    /* Chip-select is one-hot: 1=dev0, 2=dev1, 4=dev2. */
    if (ch == 0u && cs == 2u) {            /* mask ROM / RTC / SRAM */
        ipl_transfer(exi, cpu, ch, rw, dma, len);
    } else if (ch == 2u && cs == 1u) {     /* AD16 dev-hardware probe */
        /* Retail console has no AD16: reads drive 0, writes are ignored. */
        if (rw == 0u && !dma) exi->channels[ch].data = 0;
    } else {
        /* Memory cards (ch0 cs1 slot A, ch1 cs1 slot B) and SP1/SP2 are not
         * modeled yet (M4). Present-but-empty: reads drive 0. FLAGGED. */
        if (rw == 0u && !dma) exi->channels[ch].data = 0;
    }
}

static void start_transfer(GcnExi* exi, CPUState* cpu, u32 ch) {
    GcnExiChannel* c = &exi->channels[ch];
    bool dma = (c->cr & GCN_EXI_CR_DMA) != 0u;
    u32 rw  = (c->cr & GCN_EXI_CR_RW_MASK) >> 2u;      /* 0=read 1=write */
    u32 len = dma ? c->len : (((c->cr & GCN_EXI_CR_TLEN_MASK) >> 4u) + 1u);
    u32 cs  = csr_chip_select(c->csr);

    device_transfer(exi, cpu, ch, cs, rw, dma, len);

    /* Complete synchronously: clear TSTART, latch transfer-complete interrupt. */
    c->cr  &= ~GCN_EXI_CR_TSTART;
    c->csr |= GCN_EXI_CSR_TCINT;
}

/* ---- MMIO dispatch entry points -------------------------------------------*/

u32 gcn_exi_read(void* user, CPUState* cpu, u32 addr, u8 size) {
    (void)cpu; (void)size;
    GcnExi* exi = (GcnExi*)user;
    u32 ch = 0, off = 0;
    if (!decode(addr, &ch, &off)) return 0;
    GcnExiChannel* c = &exi->channels[ch];
    switch (off) {
    case GCN_EXI_CSR_OFF:  return c->csr;
    case GCN_EXI_MAR_OFF:  return c->mar;
    case GCN_EXI_LEN_OFF:  return c->len;
    case GCN_EXI_CR_OFF:   return c->cr;
    case GCN_EXI_DATA_OFF: return c->data;
    default:               return 0;
    }
}

void gcn_exi_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size) {
    (void)size;
    GcnExi* exi = (GcnExi*)user;
    u32 ch = 0, off = 0;
    if (!decode(addr, &ch, &off)) return;
    GcnExiChannel* c = &exi->channels[ch];
    switch (off) {
    case GCN_EXI_CSR_OFF:  write_csr(exi, ch, value); break;
    case GCN_EXI_MAR_OFF:  c->mar = value; break;
    case GCN_EXI_LEN_OFF:  c->len = value; break;
    case GCN_EXI_CR_OFF:
        c->cr = value;
        if (value & GCN_EXI_CR_TSTART)
            start_transfer(exi, cpu, ch);
        break;
    case GCN_EXI_DATA_OFF: c->data = value; break;
    default: break;
    }
}
