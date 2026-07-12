/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gcnrecomp runtime — EXI memory-card device model (ROADMAP M4).
 *
 * A GameCube memory card is an EXI device: slot A = channel 0, device index 0
 * (chip-select one-hot value 1); slot B = channel 1, device index 0. (The
 * IPL mask-ROM/RTC/SRAM device is channel 0, device index 1 = chip-select 2 —
 * a DIFFERENT device on the same channel; see exi.h.) The card presents a
 * NOR-flash-style command interface over the EXI serial bus.
 *
 * PROVENANCE
 *   Transcribed from Dolphin's CEXIMemoryCard (our independent oracle):
 *   oracle/dolphin/Source/Core/Core/HW/EXI/EXI_DeviceMemoryCard.cpp. The command
 *   state machine (gcn_memcard_transfer_byte) mirrors CEXIMemoryCard::TransferByte
 *   byte-for-byte; the deselect-commit (gcn_memcard_set_cs) mirrors SetCS; the
 *   bulk paths mirror the DMARead/DMAWrite overrides. See _work/M4_SPEC_PROTOCOL.md
 *   for the full citation-backed derivation. This is a faithful hardware model,
 *   NOT an HLE of the CARD library — the real IPL flash driver drives every byte.
 *
 * TIMING NOTE (synchronous completion)
 *   Dolphin defers transfer-complete (TCINT) and command-done interrupts through
 *   CoreTiming (UseDelayedTransferCompletion()==true; CmdDoneLater(5000) after an
 *   erase/program). Our EXI model completes transfers synchronously. Per the spec
 *   analysis (_work/M4_SPEC_PROTOCOL.md §5.5) this is observably identical for
 *   reads/status/id/DMA (Dolphin moves the data synchronously; only the interrupt
 *   is deferred), and the one place it is not obviously equivalent — the ~5000-
 *   cycle BUSY window after SectorErase/PageProgram — collapses to "the guest sees
 *   READY immediately", which is validated against captured erase/program traffic
 *   rather than assumed. gcn_memcard_set_cs applies the CmdDone() effect (READY set,
 *   BUSY cleared, interrupt latched) immediately on the deselect that commits an
 *   erase/program.
 */
#ifndef GCN_MEMCARD_MEMCARD_H
#define GCN_MEMCARD_MEMCARD_H

#include "cpu/cpu.h"   /* u8/u16/u32 fixed-width types */

#ifdef __cplusplus
extern "C" {
#endif

/* On-card geometry (Dolphin GCMemcard.h). */
#define GCN_MC_BLOCK_SIZE     0x2000u   /* 8192 — erase/sector block            */
#define GCN_MC_SYSTEM_BLOCKS  5u        /* header/dir/dir-bak/bat/bat-bak        */
#define GCN_MC_PAGE_BYTES     128u      /* programming buffer size              */
#define GCN_MC_SIZE_TO_MB     0x20000u  /* bytes per Mbit-id unit (1024*8*16)   */

/* Card size id = GetCardId() = size in "Mbit-id" units (the value NintendoID
 * reports and that byte size divides down to). 251-block retail card = 16 Mbit
 * = 0x10 → 2 MiB. (Dolphin's own default is 0x80 / 128 Mbit; we use the classic
 * 251-block card, which is what the IPL and the vast majority of GC saves target.) */
#define GCN_MC_MBIT_59    0x04u  /*  4 Mbit,   64 blocks,  59 usable, 0x080000 B */
#define GCN_MC_MBIT_123   0x08u  /*  8 Mbit,  128 blocks, 123 usable, 0x100000 B */
#define GCN_MC_MBIT_251   0x10u  /* 16 Mbit,  256 blocks, 251 usable, 0x200000 B */
#define GCN_MC_MBIT_507   0x20u  /* 32 Mbit,  512 blocks, 507 usable, 0x400000 B */
#define GCN_MC_MBIT_1019  0x40u  /* 64 Mbit, 1024 blocks,1019 usable, 0x800000 B */
#define GCN_MC_MBIT_2043  0x80u  /*128 Mbit, 2048 blocks,2043 usable,0x1000000 B */

/* Fixed "Nintendo brand" 16-bit id returned by ReadID (0x85). Not size-derived
 * (EXI_DeviceMemoryCard.cpp:131). */
#define GCN_MC_CARD_ID    0xc221u

/* m_status bits (EXI_DeviceMemoryCard.cpp:40-45). Reset value = BUSY|UNLOCKED|
 * READY = 0xC1 (line 118). ERASEERROR/PROGRAMERROR/SLEEP are never set by the
 * model (no failure path) but ClearStatus clears the two error bits. */
#define GCN_MC_STATUS_BUSY          0x80
#define GCN_MC_STATUS_UNLOCKED      0x40
#define GCN_MC_STATUS_SLEEP         0x20
#define GCN_MC_STATUS_ERASEERROR    0x10
#define GCN_MC_STATUS_PROGRAMERROR  0x08
#define GCN_MC_STATUS_READY         0x01
#define GCN_MC_STATUS_RESET \
    (GCN_MC_STATUS_BUSY | GCN_MC_STATUS_UNLOCKED | GCN_MC_STATUS_READY)  /* 0xC1 */

/* EXI memory-card command opcodes (Command enum, EXI_DeviceMemoryCard.h:88-105).
 * Only the nine with real state-machine logic are handled; the rest are no-ops
 * that echo 0xFF, exactly as Dolphin (see spec §2.1). */
enum {
    GCN_MC_CMD_NINTENDO_ID      = 0x00,
    GCN_MC_CMD_READ_ARRAY       = 0x52,
    GCN_MC_CMD_ARRAY_TO_BUFFER  = 0x53,
    GCN_MC_CMD_SET_INTERRUPT    = 0x81,
    GCN_MC_CMD_WRITE_BUFFER     = 0x82,
    GCN_MC_CMD_READ_STATUS      = 0x83,
    GCN_MC_CMD_READ_ID          = 0x85,
    GCN_MC_CMD_READ_ERROR_BUF   = 0x86,
    GCN_MC_CMD_WAKE_UP          = 0x87,
    GCN_MC_CMD_SLEEP            = 0x88,
    GCN_MC_CMD_CLEAR_STATUS     = 0x89,
    GCN_MC_CMD_SECTOR_ERASE     = 0xF1,
    GCN_MC_CMD_PAGE_PROGRAM     = 0xF2,
    GCN_MC_CMD_EXTRA_BYTE_PROG  = 0xF3,
    GCN_MC_CMD_CHIP_ERASE       = 0xF4,
};

typedef struct GcnMemcard {
    /* Backing image (owned; freed by gcn_memcard_free). `size_bytes` is a power
     * of two; `data` is the flat card memory the .raw file mirrors 1:1. */
    u8*  data;
    u32  size_bytes;
    u16  size_mbits;   /* GetCardId(): size_bytes / GCN_MC_SIZE_TO_MB */
    u16  card_id;      /* GCN_MC_CARD_ID */
    int  present;      /* IsPresent(): 1 once a card image is installed */

    /* Per-transaction state (position resets on chip-select assert). */
    int  command;                    /* m_command: opcode from byte 0          */
    u32  position;                   /* m_position: byte index within CS assert */
    u32  address;                    /* m_address: card byte offset             */
    int  status;                     /* m_status                                */
    u8   prog_buf[GCN_MC_PAGE_BYTES];/* m_programming_buffer                    */
    int  interrupt_switch;           /* SetInterrupt(0x81) enable gate          */
    int  interrupt_set;              /* latched command-done interrupt          */

    /* Set whenever the backing image is mutated (Write/ClearBlock/ClearAll) so
     * the host layer knows to flush it to the .raw file. */
    int  dirty;
} GcnMemcard;

/* Install a card image. Takes ownership of `data` (size_bytes bytes, a valid
 * card size — see GCN_MC_MBIT_*). Sets present=1, card_id, status=0xC1, and
 * derives size_mbits from size_bytes. `data` may be freshly formatted (see the
 * memcard image module) or loaded from a host .raw. */
void gcn_memcard_init(GcnMemcard* mc, u8* data, u32 size_bytes);

/* Release the owned image (safe to call unconditionally). */
void gcn_memcard_free(GcnMemcard* mc);

/* Chip-select edge (CEXIMemoryCard::SetCS). `selected` != 0 → newly selected:
 * reset position. `selected` == 0 → deselected: commit any pending SectorErase/
 * ChipErase/PageProgram against the backing image (setting `dirty`) and, for
 * SectorErase/PageProgram, apply the deferred CmdDone effect immediately
 * (status READY set, BUSY cleared, interrupt latched — see the TIMING NOTE). */
void gcn_memcard_set_cs(GcnMemcard* mc, int selected);

/* Process one bus byte through the command state machine
 * (CEXIMemoryCard::TransferByte). `*byte` is the byte written by the guest on a
 * write; on a read it is ignored on input and set to the device response. Every
 * call advances `position`. */
void gcn_memcard_transfer_byte(GcnMemcard* mc, u8* byte);

/* Bulk DMA at the current `address` (CEXIMemoryCard::DMARead/DMAWrite): read
 * `size` bytes from the card into `dst`, or write `size` bytes from `src` into
 * the card (setting `dirty`). No 512-byte page wrap is applied on the DMA path
 * (that only exists in the per-byte immediate path); `address` is masked to the
 * card size. `dst`/`src` are host pointers into guest RAM resolved by the caller. */
void gcn_memcard_dma_read(GcnMemcard* mc, u8* dst, u32 size);
void gcn_memcard_dma_write(GcnMemcard* mc, const u8* src, u32 size);

/* IsInterruptSet(): interrupt_set gated by interrupt_switch (0 unless the guest
 * armed interrupts via SetInterrupt and a command-done has latched). The EXI
 * channel ORs this into EXIINT for channels 0/1 unconditionally ("always check
 * memcard slots", EXI_Channel.cpp:253). */
int gcn_memcard_interrupt_set(const GcnMemcard* mc);

#ifdef __cplusplus
}
#endif

#endif /* GCN_MEMCARD_MEMCARD_H */
