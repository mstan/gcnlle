/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gcnrecomp runtime — M4 memcard device-model unit test.
 *
 * Drives GcnMemcard the way the EXI channel would: a command byte selects the
 * opcode, subsequent bytes carry address/data, and gcn_memcard_set_cs(0)
 * commits any pending flash-style erase/program on deselect (see memcard.h's
 * TIMING NOTE). Exercises every opcode with real state-machine logic
 * (NintendoID, ReadArray, ReadStatus, ReadID, SetInterrupt, ClearStatus,
 * SectorErase, PageProgram, ChipErase) plus the DMA bulk paths, against a
 * synthetic 0x200000-byte (16 Mbit / 251-block-class) card image.
 */
#include "memcard/memcard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); g_failures++; } \
    else { fprintf(stdout, "ok: %s\n", (msg)); } \
} while (0)

#define CARD_SIZE 0x200000u   /* 16 Mbit == GCN_MC_MBIT_251 */

/* Deterministic planted pattern: byte i == i & 0xFF. Simple and exactly
 * predictable so every readback assertion below is hand-verifiable. */
static void fill_pattern(u8* buf, u32 size) {
    for (u32 i = 0; i < size; i++)
        buf[i] = (u8)(i & 0xFFu);
}

/* ---- EXI-shaped drivers: select -> bytes -> deselect ----------------------*/

static u8 xfer(GcnMemcard* mc, u8 out) {
    u8 b = out;
    gcn_memcard_transfer_byte(mc, &b);
    return b;
}

/* ReadArray (0x52): 4 address bytes then the data byte — per the oracle, the
 * 4th address byte (BA) already returns the correctly-resolved data[addr] in
 * the SAME call, since m_address is fully OR'd together before the read runs
 * (EXI_DeviceMemoryCard.cpp:402-427). Self-contained select/deselect since
 * ReadArray is a no-op on CS deselect (spec §2.2). */
static u8 mc_read_byte_at(GcnMemcard* mc, u32 addr) {
    u8 ad1 = (u8)((addr >> 17) & 0xFFu);
    u8 ad2 = (u8)((addr >> 9) & 0xFFu);
    u8 ad3 = (u8)((addr >> 7) & 0x03u);
    u8 ba  = (u8)(addr & 0x7Fu);

    gcn_memcard_set_cs(mc, 1);
    (void)xfer(mc, GCN_MC_CMD_READ_ARRAY);
    (void)xfer(mc, ad1);
    (void)xfer(mc, ad2);
    (void)xfer(mc, ad3);
    u8 got = xfer(mc, ba);
    gcn_memcard_set_cs(mc, 0);
    return got;
}

/* PageProgram (0xF2): 4 address bytes then up to 128 data bytes, committed to
 * the backing image on CS deselect (EXI_DeviceMemoryCard.cpp:308-324). */
static void mc_page_program(GcnMemcard* mc, u32 addr, const u8* data, u32 n) {
    u8 ad1 = (u8)((addr >> 17) & 0xFFu);
    u8 ad2 = (u8)((addr >> 9) & 0xFFu);
    u8 ad3 = (u8)((addr >> 7) & 0x03u);
    u8 ba  = (u8)(addr & 0x7Fu);

    gcn_memcard_set_cs(mc, 1);
    (void)xfer(mc, GCN_MC_CMD_PAGE_PROGRAM);
    (void)xfer(mc, ad1);
    (void)xfer(mc, ad2);
    (void)xfer(mc, ad3);
    (void)xfer(mc, ba);
    for (u32 i = 0; i < n; i++)
        (void)xfer(mc, data[i]);
    gcn_memcard_set_cs(mc, 0);   /* commits: count = position-5 = n */
}

/* SectorErase (0xF1): 2 address bytes (block-granular AD1/AD2 only), the
 * 0x2000-byte block containing that address is erased on CS deselect
 * (EXI_DeviceMemoryCard.cpp:287-298). */
static void mc_sector_erase(GcnMemcard* mc, u32 addr) {
    u8 ad1 = (u8)((addr >> 17) & 0xFFu);
    u8 ad2 = (u8)((addr >> 9) & 0xFFu);

    gcn_memcard_set_cs(mc, 1);
    (void)xfer(mc, GCN_MC_CMD_SECTOR_ERASE);
    (void)xfer(mc, ad1);
    (void)xfer(mc, ad2);
    gcn_memcard_set_cs(mc, 0);   /* position==3 at deselect, >2: commits */
}

int main(void) {
    u8* image = (u8*)malloc(CARD_SIZE);
    if (!image) { fprintf(stderr, "FAIL: malloc card image\n"); return 1; }
    fill_pattern(image, CARD_SIZE);

    GcnMemcard mc;
    gcn_memcard_init(&mc, image, CARD_SIZE);

    /* --- init contract (memcard.h gcn_memcard_init doc) --- */
    CHECK(mc.present == 1, "init: present == 1");
    CHECK(mc.card_id == GCN_MC_CARD_ID, "init: card_id == 0xc221");
    CHECK(mc.size_mbits == GCN_MC_MBIT_251, "init: size_mbits == 0x10 (16 Mbit)");
    CHECK(mc.status == GCN_MC_STATUS_RESET, "init: status == 0xC1");
    CHECK(mc.position == 0 && mc.address == 0 && mc.command == 0,
          "init: transaction state zeroed (command==NintendoID==0)");
    CHECK(mc.interrupt_switch == 0 && mc.interrupt_set == 0 && mc.dirty == 0,
          "init: interrupt/dirty state zeroed");
    {
        int all_zero = 1;
        for (u32 i = 0; i < GCN_MC_PAGE_BYTES; i++)
            if (mc.prog_buf[i] != 0) all_zero = 0;
        CHECK(all_zero, "init: prog_buf zeroed");
    }

    /* --- ReadStatus (0x83): baseline 0xC1 --- */
    {
        gcn_memcard_set_cs(&mc, 1);
        (void)xfer(&mc, GCN_MC_CMD_READ_STATUS);
        u8 st = xfer(&mc, 0);
        gcn_memcard_set_cs(&mc, 0);
        CHECK(st == GCN_MC_STATUS_RESET, "ReadStatus: 0xC1 at boot");
    }

    /* --- NintendoID (0x00): 80 00 00 00 10 00 00 00 (size id 0x10, BE, repeat/4) --- */
    {
        gcn_memcard_set_cs(&mc, 1);
        (void)xfer(&mc, GCN_MC_CMD_NINTENDO_ID);
        u8 b1 = xfer(&mc, 0);
        u8 b2 = xfer(&mc, 0);
        u8 b3 = xfer(&mc, 0);
        u8 b4 = xfer(&mc, 0);
        u8 b5 = xfer(&mc, 0);
        u8 b6 = xfer(&mc, 0);   /* repeats: position 6 -> (6-2)&3==0, same as position2 */
        gcn_memcard_set_cs(&mc, 0);
        CHECK(b1 == 0x80, "NintendoID: dummy cycle 0x80");
        CHECK(b2 == 0x00 && b3 == 0x00 && b4 == 0x00, "NintendoID: BE zero bytes");
        CHECK(b5 == 0x10, "NintendoID: size id 0x10 (16 Mbit)");
        CHECK(b6 == 0x00, "NintendoID: pattern repeats every 4 bytes");
    }

    /* --- ReadID (0x85): c2 c2 21 c2 21 c2 21 ... (position1 duplicates position2) --- */
    {
        gcn_memcard_set_cs(&mc, 1);
        (void)xfer(&mc, GCN_MC_CMD_READ_ID);
        u8 i1 = xfer(&mc, 0);
        u8 i2 = xfer(&mc, 0);
        u8 i3 = xfer(&mc, 0);
        u8 i4 = xfer(&mc, 0);
        u8 i5 = xfer(&mc, 0);
        gcn_memcard_set_cs(&mc, 0);
        CHECK(i1 == 0xc2, "ReadID: position1 == card_id>>8 == 0xc2");
        CHECK(i2 == 0xc2 && i3 == 0x21 && i4 == 0xc2 && i5 == 0x21,
              "ReadID: c2 c2 21 c2 21 parity pattern");
    }

    /* --- ReadArray (0x52) at a known planted address --- */
    {
        const u32 addr = 0x001234u;
        u8 got = mc_read_byte_at(&mc, addr);
        CHECK(got == (u8)(addr & 0xFFu), "ReadArray: returns planted byte at 0x001234");
    }

    /* --- PageProgram (0xF2) then set_cs(0) then ReadArray readback --- */
    {
        const u32 addr = 0x002100u;   /* block 1, distinct from other regions */
        const u8 payload[16] = { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
                                  0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF };
        mc_page_program(&mc, addr, payload, 16);
        int ok = 1;
        for (u32 i = 0; i < 16; i++) {
            u8 got = mc_read_byte_at(&mc, addr + i);
            if (got != payload[i]) ok = 0;
        }
        CHECK(ok, "PageProgram: 16-byte payload reads back exactly after commit");
        CHECK(mc.dirty != 0, "PageProgram: dirty flag set after commit");
    }

    /* --- SectorErase (0xF1) then set_cs(0): whole 0x2000 block -> 0xFF --- */
    {
        const u32 block_addr = 0x006000u;   /* block 3, exactly block-aligned */
        mc.dirty = 0;
        mc_sector_erase(&mc, block_addr);
        int all_ff = 1;
        for (u32 i = 0; i < GCN_MC_BLOCK_SIZE; i += 0x100u) {
            if (mc_read_byte_at(&mc, block_addr + i) != 0xFFu) all_ff = 0;
        }
        if (mc_read_byte_at(&mc, block_addr + GCN_MC_BLOCK_SIZE - 1) != 0xFFu) all_ff = 0;
        CHECK(all_ff, "SectorErase: entire 0x2000 block reads back 0xFF");
        CHECK(mc.dirty != 0, "SectorErase: dirty flag set after commit");

        /* Boundary check: neighbours of the erased block keep their original
         * planted pattern (the fill did not smear past the block). */
        u8 before = mc_read_byte_at(&mc, block_addr - 1);
        u8 after  = mc_read_byte_at(&mc, block_addr + GCN_MC_BLOCK_SIZE);
        CHECK(before == (u8)((block_addr - 1) & 0xFFu),
              "SectorErase: byte just before the block is untouched");
        CHECK(after == (u8)((block_addr + GCN_MC_BLOCK_SIZE) & 0xFFu),
              "SectorErase: byte just after the block is untouched");

        CHECK((mc.status & GCN_MC_STATUS_READY) != 0 &&
              (mc.status & GCN_MC_STATUS_BUSY) == 0,
              "SectorErase: status settles READY/~BUSY post-commit (collapsed CmdDone)");
    }

    /* --- SetInterrupt + program latches interrupt_set; gating; ClearStatus
     * clears it. Dismiss the SectorErase test's own already-latched
     * interrupt_set first (a real driver would ClearStatus after an erase
     * before moving on) so this section starts from a clean 0 latch. --- */
    {
        u8 b;
        gcn_memcard_set_cs(&mc, 1);
        b = (u8)GCN_MC_CMD_CLEAR_STATUS; gcn_memcard_transfer_byte(&mc, &b);
        gcn_memcard_set_cs(&mc, 0);
        CHECK(mc.interrupt_set == 0, "setup: ClearStatus dismisses SectorErase's latched interrupt");

        gcn_memcard_set_cs(&mc, 1);
        b = (u8)GCN_MC_CMD_SET_INTERRUPT; gcn_memcard_transfer_byte(&mc, &b);
        b = 1; gcn_memcard_transfer_byte(&mc, &b);   /* interrupt_switch = 1 */
        gcn_memcard_set_cs(&mc, 0);                  /* SetInterrupt: no-op on deselect */
        CHECK(mc.interrupt_switch == 1, "SetInterrupt: interrupt_switch latched");
        CHECK(gcn_memcard_interrupt_set(&mc) == 0,
              "SetInterrupt alone: no command-done yet, interrupt_set gated 0");

        /* A program's CmdDone effect latches interrupt_set. */
        const u8 tiny[4] = { 0x11, 0x22, 0x33, 0x44 };
        mc_page_program(&mc, 0x008010u, tiny, 4);
        CHECK(mc.interrupt_set == 1, "PageProgram commit: interrupt_set latched");
        CHECK(gcn_memcard_interrupt_set(&mc) == 1,
              "interrupt_switch=1 & interrupt_set=1 -> IsInterruptSet() true");

        /* Gating: switch off hides an otherwise-latched interrupt. */
        mc.interrupt_switch = 0;
        CHECK(gcn_memcard_interrupt_set(&mc) == 0,
              "interrupt_switch=0 gates IsInterruptSet() to 0 even though latched");
        mc.interrupt_switch = 1;

        /* ClearStatus (0x89): clears interrupt_set + error bits, sets READY. */
        gcn_memcard_set_cs(&mc, 1);
        b = (u8)GCN_MC_CMD_CLEAR_STATUS; gcn_memcard_transfer_byte(&mc, &b);
        gcn_memcard_set_cs(&mc, 0);   /* ClearStatus: no-op on deselect (handled at position 0) */
        CHECK(mc.interrupt_set == 0, "ClearStatus: interrupt_set cleared");
        CHECK(gcn_memcard_interrupt_set(&mc) == 0, "ClearStatus: IsInterruptSet() now 0");
        CHECK((mc.status & GCN_MC_STATUS_READY) != 0, "ClearStatus: READY set");
        CHECK((mc.status & (GCN_MC_STATUS_ERASEERROR | GCN_MC_STATUS_PROGRAMERROR)) == 0,
              "ClearStatus: error bits clear");
    }

    /* --- DMA read: flat copy at mc.address, no page wrap --- */
    {
        mc.address = 0x001234u;   /* same planted region as the ReadArray test */
        u8 dst[8];
        memset(dst, 0x55, sizeof dst);
        gcn_memcard_dma_read(&mc, dst, sizeof dst);
        int ok = 1;
        for (u32 i = 0; i < sizeof dst; i++)
            if (dst[i] != (u8)((0x001234u + i) & 0xFFu)) ok = 0;
        CHECK(ok, "DMARead: flat copy matches planted pattern, no wrap");
    }

    /* --- DMA write: flat copy into the card, sets dirty --- */
    {
        mc.address = 0x00C000u;   /* block 6, untouched elsewhere */
        u8 src[32];
        for (u32 i = 0; i < sizeof src; i++) src[i] = (u8)(0xE0u + i);
        mc.dirty = 0;
        gcn_memcard_dma_write(&mc, src, sizeof src);
        CHECK(mc.dirty != 0, "DMAWrite: dirty flag set");
        int ok = 1;
        for (u32 i = 0; i < sizeof src; i++)
            if (mc.data[0x00C000u + i] != src[i]) ok = 0;
        CHECK(ok, "DMAWrite: bytes land exactly at mc.address, flat, no wrap");
    }

    /* --- DMA bounds clamp: a request that would run past size_bytes copies
     * only what fits, never touching memory past the card image. --- */
    {
        mc.address = CARD_SIZE - 4u;
        u8 dst[16];
        memset(dst, 0x77, sizeof dst);
        gcn_memcard_dma_read(&mc, dst, sizeof dst);
        int head_ok = 1, tail_untouched = 1;
        for (u32 i = 0; i < 4; i++)
            if (dst[i] != mc.data[CARD_SIZE - 4u + i]) head_ok = 0;
        for (u32 i = 4; i < sizeof dst; i++)
            if (dst[i] != 0x77) tail_untouched = 0;
        CHECK(head_ok, "DMARead clamp: in-bounds head bytes copied correctly");
        CHECK(tail_untouched, "DMARead clamp: out-of-bounds tail left untouched (no overrun)");
    }

    /* --- ChipErase (0xF4): whole card -> 0xFF, no interrupt, run last --- */
    {
        u8 b;
        int prev_interrupt_set = mc.interrupt_set;
        gcn_memcard_set_cs(&mc, 1);
        b = (u8)GCN_MC_CMD_CHIP_ERASE; gcn_memcard_transfer_byte(&mc, &b);   /* position0->1 */
        b = 0; gcn_memcard_transfer_byte(&mc, &b);                          /* position1->2 */
        b = 0; gcn_memcard_transfer_byte(&mc, &b);                          /* position2->3 */
        gcn_memcard_set_cs(&mc, 0);   /* position==3 > 2: commits ClearAll() */

        int all_ff = 1;
        for (u32 i = 0; i < CARD_SIZE; i += 0x10000u)
            if (mc.data[i] != 0xFFu) all_ff = 0;
        if (mc.data[CARD_SIZE - 1] != 0xFFu) all_ff = 0;
        CHECK(all_ff, "ChipErase: entire card reads back 0xFF");
        CHECK((mc.status & GCN_MC_STATUS_BUSY) == 0, "ChipErase: BUSY cleared");
        CHECK(mc.interrupt_set == prev_interrupt_set,
              "ChipErase: no CmdDone effect -- interrupt_set unchanged");
    }

    gcn_memcard_free(&mc);
    CHECK(mc.data == NULL, "gcn_memcard_free: data pointer nulled");

    if (g_failures == 0)
        fprintf(stdout, "\nALL PASS\n");
    else
        fprintf(stderr, "\n%d FAILURE(S)\n", g_failures);
    return g_failures != 0;
}
