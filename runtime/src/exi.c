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
#include "cpu/native_code.h"
#include "debug/rings.h"
#include "descramble_core.h"   /* tools/ipl_descramble — vendored segher descrambler,
                                  transcribed verbatim (see gcn_exi_set_rom_scrambled) */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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
    /* Reset CSR values, transcribed from Dolphin's CEXIChannel constructor
     * (our independent oracle): ch0/1 latch EXTINT (bit 11); ch1 additionally
     * powers up with CHIP_SELECT = 1 (bit 7). EXT (bit 12) is NOT latched here —
     * it is recomputed on every CSR read from dev_present[] (see gcn_exi_read),
     * exactly as Dolphin does (m_status.EXT = GetDevice(1)->IsPresent()). */
    exi->channels[0].csr = GCN_EXI_CSR_EXTINT;
    exi->channels[1].csr = GCN_EXI_CSR_EXTINT | GCN_EXI_CSR_CS_DEV0;
    /* Channel 2 is the AD16 dev-probe target: idle, no EXT. */

    /* M4: dev_present (which drives the EXT card-inserted bit on a CSR read) is
     * now LIVE — set by gcn_exi_set_memcard from card[ch]->present. All slots
     * start empty here; boot.c installs a card in slot A (matching the Dolphin
     * oracle's "card in slot A, slot B empty" config, EXT ch0=1 / ch1=0) via
     * gcn_exi_set_memcard. ch2 is always forced 0 on read. */
    exi->dev_present[0] = 0;
    exi->dev_present[1] = 0;
    exi->dev_present[2] = 0;
}

void gcn_exi_set_irq(GcnExi* exi, GcnExiIrqFn fn, void* user) {
    exi->irq = fn;
    exi->irq_user = user;
}

/* ---- interrupt line (EXI.cpp UpdateInterrupts:240-253 + EXI_Channel.cpp
 *      CEXIChannel::IsCausingInterrupt:251-267) ----
 * Per channel the line is (EXIINT & EXIINTMASK) || (TCINT & TCINTMASK) ||
 * (EXTINT & EXTINTMASK) from that channel's CSR; the PI line is the OR across
 * all channels. A device's own IsInterruptSet() (the memcards' command-done
 * latch) is first REGISTERED into that channel's EXIINT CSR bit, exactly as
 * Dolphin does — see the load-bearing comment inside the loop. Pushed to PI on
 * EVERY evaluation (PI INTSR is W1C, so a still-asserted level must re-appear). */
static void exi_update_interrupts(GcnExi* exi) {
    int level = 0;
    for (u32 ch = 0; ch < GCN_EXI_CHANNELS; ch++) {
        /* M4: on channels 0/1 the memory card's own command-done interrupt
         * (armed by SetInterrupt, latched by an erase/program) is REGISTERED
         * INTO THE CHANNEL'S EXIINT CSR BIT — EXI_Channel.cpp:253
         * IsCausingInterrupt, literally "interrupt of device 0 is registered
         * in EXIINT" — not merely ORed into the PI line. The distinction is
         * load-bearing: the IPL's EXI interrupt dispatcher reads each
         * channel's CSR to FIND the interrupt source and W1C-ack it there;
         * feeding the line without latching the CSR bit asserts PI INTSR's
         * EXI cause while every CSR reads clean, so the guest can neither
         * locate nor ack the cause and spins in its dispatcher forever. Hit
         * live by the card manager's copy flow: the erase-complete interrupt
         * on the card-B channel hung the copy at PC 0x81336BC8 (INTSR=0x10
         * pending, ch1 CSR EXIINT=0 — diagnosed from the always-on MMIO +
         * card-traffic rings). A W1C ack while the DEVICE latch is still up
         * re-registers on the next evaluation, exactly like Dolphin; the
         * device latch itself drops on the SDK's CLEAR_STATUS (0x89)
         * command (memcard.c). */
        if (ch < 2u && exi->card[ch] && gcn_memcard_interrupt_set(exi->card[ch]))
            exi->channels[ch].csr |= GCN_EXI_CSR_EXIINT;

        u32 csr = exi->channels[ch].csr;
        if (((csr & GCN_EXI_CSR_EXIINT) && (csr & GCN_EXI_CSR_EXIINTMASK)) ||
            ((csr & GCN_EXI_CSR_TCINT)  && (csr & GCN_EXI_CSR_TCINTMASK))  ||
            ((csr & GCN_EXI_CSR_EXTINT) && (csr & GCN_EXI_CSR_EXTINTMASK)))
            level = 1;
    }
    if (exi->irq)
        exi->irq(exi->irq_user, level);
    if (level != exi->irq_level) {
        gcn_ring_event(level ? GCN_EV_IRQ_RAISE : GCN_EV_IRQ_CLEAR,
                       /*source*/ 4u /* PI cause bit index of INT_CAUSE_EXI=0x10 */,
                       0u, 0u);
        exi->irq_level = level;
    }
}

void gcn_exi_set_rom(GcnExi* exi, const u8* rom, u32 size, u32 base) {
    exi->rom = rom; exi->rom_size = size; exi->rom_base = base;
}

/* M1: see exi.h for the full contract/provenance comment. */
const u8* gcn_exi_set_rom_scrambled(GcnExi* exi, const u8* raw, u32 raw_size) {
    if (!raw || raw_size < IPL_SCRAMBLE_END)
        return NULL;
    u8* buf = (u8*)malloc(raw_size);
    if (!buf)
        return NULL;
    memcpy(buf, raw, raw_size);
    /* Verbatim segher descrambler (descramble_core.c), over exactly the
     * documented body range — everything outside it (the plaintext (C)
     * header, and any trailing unscrambled font data past IPL_SCRAMBLE_END)
     * is served as-is, faithfully mirroring what the real MX chip does. */
    ipl_descramble(buf + IPL_SCRAMBLE_START, IPL_SCRAMBLE_END - IPL_SCRAMBLE_START);

    free(exi->owned_rom);   /* replace any previously-owned buffer */
    exi->owned_rom = buf;
    gcn_exi_set_rom(exi, buf, raw_size, 0u);
    return buf;
}

void gcn_exi_free(GcnExi* exi) {
    free(exi->owned_rom);
    exi->owned_rom = NULL;
}

void gcn_exi_set_sram(GcnExi* exi, const u8 sram[GCN_SRAM_SIZE_BYTES]) {
    memcpy(exi->sram, sram, GCN_SRAM_SIZE_BYTES);
}

void gcn_exi_set_rtc(GcnExi* exi, u32 rtc_counter) {
    gcn_rtc_set_fixed(&exi->rtc, rtc_counter);
}

u32 gcn_exi_sync_rtc_from_host(GcnExi* exi, u64 core_cycles) {
    u32 sampled = gcn_rtc_sample_host_local();
    gcn_rtc_start(&exi->rtc, sampled, core_cycles);
    return sampled;
}

u32 gcn_exi_rtc_latch(GcnExi* exi, u64 core_cycles) {
    return gcn_rtc_read(&exi->rtc, core_cycles);
}

void gcn_exi_set_persist(GcnExi* exi, GcnExiPersistFn fn, void* user) {
    exi->persist = fn;
    exi->persist_user = user;
}

/* M4: install / remove a memory card on a channel, keeping dev_present (the EXT
 * card-inserted bit source) in sync with the card's presence. */
void gcn_exi_set_memcard(GcnExi* exi, u32 ch, GcnMemcard* card) {
    if (ch >= GCN_EXI_CHANNELS) return;
    exi->card[ch] = card;
    exi->dev_present[ch] = (ch < 2u && card && card->present) ? 1u : 0u;
}

void gcn_exi_set_card_persist(GcnExi* exi, GcnExiCardPersistFn fn, void* user) {
    exi->card_persist = fn;
    exi->card_persist_user = user;
}

bool gcn_exi_persist_load(const char* path, u32* out_rtc,
                           u8 out_sram[GCN_SRAM_SIZE_BYTES]) {
    if (!path || !*path || !out_rtc || !out_sram) return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;   /* no file yet: caller keeps the fixture */

    u8 buf[GCN_EXI_PERSIST_FILE_SIZE];
    size_t got = fread(buf, 1, sizeof(buf), f);
    /* Reject anything but an exact-size file (truncated/corrupt/foreign) so a
     * bad file falls back to the fixture instead of half-loading garbage. */
    if (got != sizeof(buf) || fgetc(f) != EOF) { fclose(f); return false; }
    fclose(f);

    *out_rtc = ((u32)buf[0] << 24) | ((u32)buf[1] << 16) |
               ((u32)buf[2] << 8)  |  (u32)buf[3];
    memcpy(out_sram, buf + 4, GCN_SRAM_SIZE_BYTES);
    return true;
}

bool gcn_exi_persist_save(const char* path, u32 rtc,
                           const u8 sram[GCN_SRAM_SIZE_BYTES]) {
    if (!path || !*path) return false;
    FILE* f = fopen(path, "wb");
    if (!f) return false;

    u8 buf[GCN_EXI_PERSIST_FILE_SIZE];
    buf[0] = (u8)(rtc >> 24); buf[1] = (u8)(rtc >> 16);
    buf[2] = (u8)(rtc >> 8);  buf[3] = (u8)rtc;
    memcpy(buf + 4, sram, GCN_SRAM_SIZE_BYTES);

    size_t wrote = fwrite(buf, 1, sizeof(buf), f);
    int closed_ok = (fclose(f) == 0);
    return wrote == sizeof(buf) && closed_ok;
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
        /* M4: drive the memory card's SetCS across its own selection edge (the
         * card is device 0 = chip-select one-hot 1). Selecting resets the card's
         * byte position; DESELECTING commits any pending erase/program
         * (CEXIMemoryCard::SetCS) and, if that mutated the image, flushes it to
         * the host .raw. Mirrors EXI_Channel.cpp:91-94. */
        if (ch < 2u && exi->card[ch]) {
            int was_sel = (exi->prev_cs[ch] == 1u);
            int now_sel = (cs == 1u);
            if (now_sel != was_sel) {
                gcn_memcard_set_cs(exi->card[ch], now_sel);
                if (!now_sel && exi->card[ch]->dirty && exi->card_persist)
                    exi->card_persist(exi->card_persist_user, ch, exi->card[ch]);
            }
        }
        reset_transaction(exi, ch);
        exi->prev_cs[ch] = (u8)cs;
    }

    /* Mask changes and W1C acks of EXIINT/TCINT/EXTINT all move the line
     * (Dolphin's CSR write handler ends in UpdateInterrupts). */
    exi_update_interrupts(exi);
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
        /* Host time, when requested, was sampled once at boot. Reads advance
         * the device solely from monotonic emulated Gekko cycles. */
        c->data = gcn_rtc_read(&exi->rtc, cpu ? cpu->cycles : 0u);
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
                gcn_rtc_write(&exi->rtc, c->data, cpu ? cpu->cycles : 0u);
                if (exi->persist) exi->persist(exi->persist_user);
            } else if (exi->op[ch] == GCN_EXI_OP_SRAM_WRITE) {
                for (u32 i = 0; i < len && i < 4u; i++) {
                    u32 p = exi->dev_pos[ch] + i;
                    if (p < GCN_SRAM_SIZE_BYTES)
                        exi->sram[p] = (u8)(c->data >> (24u - 8u * i));
                }
                exi->dev_pos[ch] += len;
                if (exi->persist) exi->persist(exi->persist_user);
            }
        }
    } else {                    /* READ: device -> CPU */
        ipl_dev_read(exi, cpu, ch, dma, len);
    }
}

/* Memory card (channel 0 dev0 = slot A, channel 1 dev0 = slot B; chip-select
 * one-hot 1). Immediate transfers are byte-serial through the card's per-byte
 * command state machine, packed MSB-first into the immediate data register
 * exactly as IEXIDevice::ImmRead/ImmWrite decompose them (EXI_Device.cpp:33-54);
 * DMA transfers move the block in bulk at the card's current address
 * (CEXIMemoryCard::DMARead/DMAWrite). Every transaction is recorded into the
 * always-on memcard ring. Returns whether TCINT should latch (see start_transfer):
 * the memcard raises transfer-complete ONLY on DMA — its UseDelayedTransferCompletion
 * is true, so the channel never auto-completes an immediate transfer
 * (EXI_Channel.cpp:199) and immediate transfers schedule no completion event. */
static int memcard_transfer(GcnExi* exi, CPUState* cpu, u32 ch, u32 rw, bool dma, u32 len) {
    GcnMemcard* mc = exi->card[ch];
    GcnExiChannel* c = &exi->channels[ch];
    if (dma) {
        u32 guest = 0x80000000u | (c->mar & 0x03FFFFFFu);   /* physical -> cached */
        u32 avail = 0;
        u8* p = gcn_mem_resolve(cpu, guest, &avail);
        u32 n = (len < avail) ? len : avail;
        if (p) {
            if (rw == 1u) {
                gcn_memcard_dma_write(mc, p, n);
            } else {
                gcn_ring_watch_check_span(guest, n, 0xEC1EC100u); /* [gcn-watch] */
                gcn_memcard_dma_read(mc, p, n);
                /* Device write to RAM: dirty the miss-CRC identity only
                 * (icbi convention -- see gcn_native_code_content_dirty). */
                gcn_native_code_content_dirty(guest, n);
            }
        }
    } else if (rw == 1u) {                 /* immediate WRITE: CPU -> card */
        u32 v = c->data;
        for (u32 i = 0; i < len && i < 4u; i++) {
            u8 b = (u8)(v >> (24u - 8u * i));
            gcn_memcard_transfer_byte(mc, &b);   /* response byte discarded */
        }
    } else if (rw == 0u) {                 /* immediate READ: card -> CPU */
        u32 v = 0;
        for (u32 i = 0; i < len && i < 4u; i++) {
            u8 b = 0;
            gcn_memcard_transfer_byte(mc, &b);
            v |= (u32)b << (24u - 8u * i);
        }
        c->data = v;
    }
    /* rw==2 (read/write) is a no-op device-side: IEXIDevice::ImmReadWrite is
     * empty (EXI_Device.cpp:56-58) and the card never relies on it. */

    gcn_ring_memcard(cpu->pc, (u8)ch, /*cs*/1u, (u8)mc->command, (u8)rw,
                     dma ? 1u : 0u, mc->address, len, dma ? c->mar : c->data);
    return dma ? 1 : 0;
}

/* Route a started transfer to the selected device. Returns 1 if the transfer
 * raises TCINT (a non-delayed device completes immediately; the memcard only on
 * DMA — see memcard_transfer). Chip-select is one-hot: 1=dev0, 2=dev1, 4=dev2. */
static int device_transfer(GcnExi* exi, CPUState* cpu, u32 ch, u32 cs,
                           u32 rw, bool dma, u32 len) {
    if (ch == 0u && cs == 2u) {                        /* mask ROM / RTC / SRAM */
        ipl_transfer(exi, cpu, ch, rw, dma, len);
        return 1;
    } else if (ch < 2u && cs == 1u && exi->card[ch]) { /* memory card slot A/B */
        return memcard_transfer(exi, cpu, ch, rw, dma, len);
    } else if (ch == 2u && cs == 1u) {                 /* AD16 dev-hardware probe */
        /* Retail console has no AD16: reads drive 0, writes are ignored. */
        if (rw == 0u && !dma) exi->channels[ch].data = 0;
        return 1;
    } else {
        /* No device at this (channel, chip-select) — e.g. an empty card slot or
         * SP1/SP2: reads drive 0. The "None" device still completes (TCINT). */
        if (rw == 0u && !dma) exi->channels[ch].data = 0;
        return 1;
    }
}

static void start_transfer(GcnExi* exi, CPUState* cpu, u32 ch) {
    GcnExiChannel* c = &exi->channels[ch];
    bool dma = (c->cr & GCN_EXI_CR_DMA) != 0u;
    u32 rw  = (c->cr & GCN_EXI_CR_RW_MASK) >> 2u;      /* 0=read 1=write */
    u32 len = dma ? c->len : (((c->cr & GCN_EXI_CR_TLEN_MASK) >> 4u) + 1u);
    u32 cs  = csr_chip_select(c->csr);

    int set_tcint = device_transfer(exi, cpu, ch, cs, rw, dma, len);

    /* Clear TSTART, then latch transfer-complete (EXI_Channel.cpp:210) for any
     * device that completes synchronously, and re-evaluate the PI line. */
    c->cr &= ~GCN_EXI_CR_TSTART;
    if (set_tcint)
        c->csr |= GCN_EXI_CSR_TCINT;
    exi_update_interrupts(exi);
}

/* ---- MMIO dispatch entry points -------------------------------------------*/

u32 gcn_exi_read(void* user, CPUState* cpu, u32 addr, u8 size) {
    (void)cpu; (void)size;
    GcnExi* exi = (GcnExi*)user;
    u32 ch = 0, off = 0;
    if (!decode(addr, &ch, &off)) return 0;
    GcnExiChannel* c = &exi->channels[ch];
    switch (off) {
    case GCN_EXI_CSR_OFF:
        /* EXT reflects device presence at chip-select 1, computed on read (never
         * stored); ch2 is always 0 (Dolphin forces it). */
        if (ch < 2u && exi->dev_present[ch])
            return c->csr | GCN_EXI_CSR_EXT;
        return c->csr & ~GCN_EXI_CSR_EXT;
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
