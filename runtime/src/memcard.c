/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gcnrecomp runtime — EXI memory-card device model (impl). See
 * include/memcard/memcard.h for the contract, provenance summary, and the
 * TIMING NOTE on synchronous-completion collapse.
 *
 * PROVENANCE
 *   Transcribed from Dolphin's CEXIMemoryCard (our independent oracle):
 *   oracle/dolphin/Source/Core/Core/HW/EXI/EXI_DeviceMemoryCard.cpp.
 *     gcn_memcard_init            <- ctor field inits, lines 115-131
 *     gcn_memcard_transfer_byte   <- TransferByte,      lines 339-496
 *     gcn_memcard_set_cs          <- SetCS,             lines 277-330
 *                                    (+ CmdDone effect,  lines 253-260, folded
 *                                     in synchronously per the TIMING NOTE)
 *     gcn_memcard_dma_read        <- DMARead,           lines 523-537
 *     gcn_memcard_dma_write       <- DMAWrite,           lines 541-555
 *     gcn_memcard_interrupt_set   <- IsInterruptSet,     lines 332-337
 *   Full citation-backed derivation: _work/M4_SPEC_PROTOCOL.md.
 */
#include "memcard/memcard.h"

#include <string.h>
#include <stdlib.h>

/* ---- lifecycle -------------------------------------------------------------*/

/* EXI_DeviceMemoryCard.cpp:115-131 (ctor): m_interrupt_switch=0, m_interrupt_set
 * =false, m_command=NintendoID(0), m_status=BUSY|UNLOCKED|READY(0xC1),
 * m_position=0, m_programming_buffer filled 0, m_card_id=0xc221 (fixed, NOT
 * size-derived — see ReadID vs NintendoID in transfer_byte). m_memory_card_size
 * / GetCardId() (size_mbits here) is size_bytes / SIZE_TO_Mb (EXI_DeviceMemoryCard
 * .cpp:142, GCMemcardRaw.cpp:45-46 for an existing-file-sized raw backing). */
void gcn_memcard_init(GcnMemcard* mc, u8* data, u32 size_bytes) {
    memset(mc, 0, sizeof(*mc));
    mc->data = data;
    mc->size_bytes = size_bytes;
    mc->size_mbits = (u16)(size_bytes / GCN_MC_SIZE_TO_MB);
    mc->card_id = GCN_MC_CARD_ID;
    mc->present = 1;
    mc->status = GCN_MC_STATUS_RESET;
    /* command/position/address/prog_buf/interrupt_switch/interrupt_set/dirty
     * are all zero from the memset above (command==0 == GCN_MC_CMD_NINTENDO_ID,
     * matching the ctor's m_command = Command::NintendoID). */
}

void gcn_memcard_free(GcnMemcard* mc) {
    free(mc->data);
    mc->data = NULL;
}

/* ---- chip-select edge (SetCS, EXI_DeviceMemoryCard.cpp:277-330) ------------*/

void gcn_memcard_set_cs(GcnMemcard* mc, int selected) {
    if (selected) {                       /* not-selected -> selected: :281 */
        mc->position = 0;
        return;
    }

    switch (mc->command) {
    case GCN_MC_CMD_SECTOR_ERASE:          /* :287-298 */
        if (mc->position > 2) {
            /* ClearBlock(m_address & (m_memory_card_size-1)), :290. The real
             * backing requires a block-aligned address (GCMemcardRaw.cpp:198-
             * 211); the device itself never masks down to the block boundary
             * before calling it, relying on AD1/AD2 (bits 9+) supplying a
             * block-granular address. We replicate the *effective* behavior by
             * masking down to the GCN_MC_BLOCK_SIZE boundary ourselves so a
             * non-block-aligned address can never smear the fill across two
             * blocks or walk off the end of `data`. */
            u32 addr = mc->address & (mc->size_bytes - 1);
            u32 block_base = addr & ~(GCN_MC_BLOCK_SIZE - 1u);
            if (block_base + GCN_MC_BLOCK_SIZE <= mc->size_bytes) {
                memset(mc->data + block_base, 0xFF, GCN_MC_BLOCK_SIZE);
                mc->dirty = 1;
            }
            /* :291-296 set BUSY/clear READY then CmdDoneLater(5000). Per the
             * TIMING NOTE we collapse straight to the post-CmdDone effect
             * (:253-260): READY set, BUSY cleared, interrupt latched. */
            mc->status |= GCN_MC_STATUS_READY;
            mc->status &= ~GCN_MC_STATUS_BUSY;
            mc->interrupt_set = 1;
        }
        break;

    case GCN_MC_CMD_CHIP_ERASE:            /* :300-306 */
        if (mc->position > 2) {
            memset(mc->data, 0xFF, mc->size_bytes);   /* ClearAll(), :303 */
            mc->dirty = 1;
            mc->status &= ~GCN_MC_STATUS_BUSY;         /* :304 — synchronous in
                                                          * Dolphin itself: no
                                                          * CmdDoneLater, no
                                                          * READY touch, no
                                                          * interrupt. */
        }
        break;

    case GCN_MC_CMD_PAGE_PROGRAM:          /* :308-324 */
        if (mc->position >= 5) {
            u32 count = mc->position - 5;
            u32 i = 0;
            mc->status &= ~GCN_MC_STATUS_BUSY;         /* :313 */
            while (count--) {
                /* :317 m_memory_card->Write(m_address, 1, &buf[i]) does not
                 * itself mask m_address; the real raw backing bounds-checks
                 * and silently refuses an out-of-range Write. We mirror that
                 * containment by masking here so an out-of-range address can
                 * never write past `data`. */
                u32 off = mc->address & (mc->size_bytes - 1);
                mc->data[off] = mc->prog_buf[i++];
                i &= 127;                               /* :318 */
                mc->address = (mc->address & ~0x1FFu) | ((mc->address + 1) & 0x1FFu); /* :319 */
            }
            mc->dirty = 1;
            /* :322 CmdDoneLater(5000) — collapsed synchronously (TIMING NOTE). */
            mc->status |= GCN_MC_STATUS_READY;
            mc->interrupt_set = 1;
        }
        break;

    default:                               /* :326-327 — no-op on deselect */
        break;
    }
}

/* ---- per-byte command state machine (TransferByte, :339-496) --------------*/

void gcn_memcard_transfer_byte(GcnMemcard* mc, u8* byte) {
    if (mc->position == 0) {
        mc->command = (int)*byte;   /* :344 m_command = static_cast<Command>(byte) */
        *byte = 0xFF;               /* :345 */

        /* :347-371 is a logging-only switch on every known command value —
         * no side effects; deliberately not transcribed (see spec §2.0). */

        if (mc->command == GCN_MC_CMD_CLEAR_STATUS) {   /* :372-383 */
            mc->status &= ~GCN_MC_STATUS_PROGRAMERROR;
            mc->status &= ~GCN_MC_STATUS_ERASEERROR;
            mc->status |= GCN_MC_STATUS_READY;
            mc->interrupt_set = 0;
            *byte = 0xFF;
            mc->position = 0;   /* :382 — a no-op; the trailing position++
                                  * below sets it right back to 1 (spec §2.0). */
        }
    } else {
        switch (mc->command) {
        case GCN_MC_CMD_NINTENDO_ID:        /* :389-400 */
            if (mc->position == 1)
                *byte = 0x80;                /* dummy cycle, :397 */
            else
                *byte = (u8)((u32)mc->size_mbits >>
                             (24 - (((mc->position - 2) & 3) * 8)));   /* :399,
                                              GetCardId() == size_mbits here */
            break;

        case GCN_MC_CMD_READ_ARRAY:          /* :402-427 */
            switch (mc->position) {
            case 1: mc->address = (u32)(*byte) << 17; *byte = 0xFF; break; /* AD1 */
            case 2: mc->address |= (u32)(*byte) << 9;               break; /* AD2 */
            case 3: mc->address |= (u32)(*byte & 3u) << 7;          break; /* AD3 */
            case 4: mc->address |= (u32)(*byte & 0x7Fu);            break; /* BA  */
            default: break;
            }
            if (mc->position > 1) {         /* :419 */
                u32 off = mc->address & (mc->size_bytes - 1);   /* :421 */
                *byte = mc->data[off];
                if (mc->position >= 9)      /* :424-425 */
                    mc->address = (mc->address & ~0x1FFu) | ((mc->address + 1) & 0x1FFu);
            }
            break;

        case GCN_MC_CMD_READ_STATUS:         /* :429-432 */
            *byte = (u8)mc->status;
            break;

        case GCN_MC_CMD_READ_ID:             /* :434-439 */
            if (mc->position == 1)
                *byte = (u8)(mc->card_id >> 8);
            else
                *byte = (u8)((mc->position & 1u) ? mc->card_id : (mc->card_id >> 8));
            break;

        case GCN_MC_CMD_SECTOR_ERASE:        /* :441-452 */
            switch (mc->position) {
            case 1: mc->address = (u32)(*byte) << 17; break;   /* AD1 */
            case 2: mc->address |= (u32)(*byte) << 9;  break;   /* AD2 */
            default: break;
            }
            *byte = 0xFF;
            break;

        case GCN_MC_CMD_SET_INTERRUPT:       /* :454-460 */
            if (mc->position == 1)
                mc->interrupt_switch = (int)*byte;
            *byte = 0xFF;
            break;

        case GCN_MC_CMD_CHIP_ERASE:          /* :462-464 */
            *byte = 0xFF;
            break;

        case GCN_MC_CMD_PAGE_PROGRAM:        /* :466-487 */
            switch (mc->position) {
            case 1: mc->address = (u32)(*byte) << 17; break;   /* AD1 */
            case 2: mc->address |= (u32)(*byte) << 9;  break;   /* AD2 */
            case 3: mc->address |= (u32)(*byte & 3u) << 7; break; /* AD3 */
            case 4: mc->address |= (u32)(*byte & 0x7Fu); break;  /* BA  */
            default: break;
            }
            if (mc->position >= 5)
                mc->prog_buf[(mc->position - 5) & 0x7Fu] = *byte;   /* :484 */
            *byte = 0xFF;
            break;

        /* ArrayToBuffer(0x53), WriteBuffer(0x82), ReadErrorBuffer(0x86),
         * WakeUp(0x87), Sleep(0x88), ExtraByteProgram(0xF3), and any unknown
         * byte: no case in Dolphin's position>0 switch (:489-491) — fall to
         * default, echo 0xFF, no state change. */
        default:
            *byte = 0xFF;
            break;
        }
    }
    mc->position++;   /* :494 — unconditional */
}

/* ---- bulk DMA (DMARead/DMAWrite, :523-555) ---------------------------------
 * Flat copy at the current m_address, no per-byte page wrap (that only exists
 * in the immediate path above). Dolphin itself does not mask m_address here
 * (§6.3 of the spec) and relies on the raw backing's own bounds check to
 * refuse an out-of-range access outright; we replicate the *effective*
 * containment by masking address and clamping the copy length to what
 * actually fits in `data`, rather than ever indexing past size_bytes. */
void gcn_memcard_dma_read(GcnMemcard* mc, u8* dst, u32 size) {
    u32 addr = mc->address & (mc->size_bytes - 1);
    u32 remain = mc->size_bytes - addr;
    u32 n = (size < remain) ? size : remain;
    if (n)
        memcpy(dst, mc->data + addr, n);
}

void gcn_memcard_dma_write(GcnMemcard* mc, const u8* src, u32 size) {
    u32 addr = mc->address & (mc->size_bytes - 1);
    u32 remain = mc->size_bytes - addr;
    u32 n = (size < remain) ? size : remain;
    if (n) {
        memcpy(mc->data + addr, src, n);
        mc->dirty = 1;
    }
}

/* ---- interrupt line (IsInterruptSet, :332-337) -----------------------------*/

int gcn_memcard_interrupt_set(const GcnMemcard* mc) {
    return mc->interrupt_switch ? mc->interrupt_set : 0;
}
