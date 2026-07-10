/*
 * oracle/trace_format.h — gcnrecomp differential-oracle binary trace format
 * -------------------------------------------------------------------------
 * Part of gcnrecomp (GPL-3.0). This header is net-new (NOT copied from
 * Dolphin) and defines the shared on-disk record format emitted by BOTH:
 *
 *   (a) the LLE runtime under test  (in runtime/,  producer GCN_TRACE_PROD_RUNTIME)
 *   (b) the Dolphin trace-tap       (a LOCAL patch; see oracle/PATCH_PLAN.md,
 *                                    producer id GCN_TRACE_PROD_DOLPHIN)
 *
 * The two streams are compared by VALUE + ORDER (never by frame alignment) by
 * oracle/diff.py. This is the tiered design from docs/DESIGN.md §7:
 *   Tier 1  event ledger  : PC / low-mem writes / MMIO / interrupts / EXI / GX / DSP
 *   Tier 2  full register : gpr[0..31] + SPRs + CR/XER/MSR + optional FP hash
 *
 * ---------------------------------------------------------------------------
 * ENDIANNESS OF THE TRACE FILE
 *   The trace file is written in HOST byte order (little-endian on the x86
 *   hosts both producers run on). The *values* stored are already-decoded
 *   guest register/memory values as native integers — NOT raw big-endian
 *   Gekko memory bytes. A big-endian producer must byte-swap to LE on write;
 *   diff.py assumes little-endian ('<').
 *
 * FIXED-SIZE RECORDS
 *   Every record is exactly sizeof(gcn_trace_record) == GCN_TRACE_RECORD_SIZE
 *   bytes: a common 24-byte header followed by a tagged union whose size is
 *   the largest member (the retired-instruction payload). Unused union tail
 *   bytes MUST be zero-filled by the producer so CRCs/diffs are stable.
 *   Fixed records make the stream trivially seekable and parseable without a
 *   length prefix; variable-length payloads (EXI byte streams, GX FIFO spans)
 *   are captured as a small inline prefix + total length + CRC32 of the whole.
 * ---------------------------------------------------------------------------
 */
#ifndef GCN_ORACLE_TRACE_FORMAT_H
#define GCN_ORACLE_TRACE_FORMAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes on disk read 'G','C','N','T' on a little-endian host. */
#define GCN_TRACE_FILE_MAGIC   ((uint32_t)('G' | ('C' << 8) | ('N' << 16) | ((uint32_t)'T' << 24)))
#define GCN_TRACE_VERSION      1u

/* Producer identity (file header + per-record cpu high bits unused for now). */
#define GCN_TRACE_PROD_RUNTIME 0u   /* the recompiled-IPL LLE runtime under test */
#define GCN_TRACE_PROD_DOLPHIN 1u   /* the Dolphin trace-tap oracle              */

#if defined(_MSC_VER)
#  define GCN_PACK_PUSH __pragma(pack(push, 1))
#  define GCN_PACK_POP  __pragma(pack(pop))
#  define GCN_PACKED
#else
#  define GCN_PACK_PUSH
#  define GCN_PACK_POP
#  define GCN_PACKED __attribute__((packed))
#endif

/* ----- record type tags -------------------------------------------------- */
enum gcn_trace_rec_type {
    GCN_TR_RETIRED   = 1,  /* one retired guest instruction + full arch state */
    GCN_TR_MEM_WRITE = 2,  /* store to MEM1 (cached/uncached)                 */
    GCN_TR_MMIO      = 3,  /* Flipper MMIO access (read or write)             */
    GCN_TR_DMA       = 4,  /* block DMA (ARAM / DI / AI / ...)                */
    GCN_TR_INTERRUPT = 5,  /* PI/exception delivery                          */
    GCN_TR_EXI       = 6,  /* EXI transaction (RTC/SRAM/memcard/ROM)         */
    GCN_TR_GX        = 7,  /* GX FIFO write span / parsed packet / PE event  */
    GCN_TR_VI        = 8,  /* Video Interface (xfb / retrace)                */
    GCN_TR_DSP_AI    = 9   /* DSP mailbox / ARAM DMA / AI stream             */
};

/* ----- per-record common header ------------------------------------------ */
GCN_PACK_PUSH
typedef struct GCN_PACKED gcn_trace_hdr {
    uint16_t type;   /* enum gcn_trace_rec_type                              */
    uint16_t size;   /* == GCN_TRACE_RECORD_SIZE (redundant guard)          */
    uint32_t cpu;    /* core id; 0 on GameCube (single Gekko)               */
    uint64_t seq;    /* global monotonic event/retire index — the ORDER key */
    uint64_t tb;     /* guest time-base snapshot; debug only, NOT for align */
} gcn_trace_hdr;
GCN_PACK_POP

/* ----- payloads ---------------------------------------------------------- */

/* retired-instruction: the tier-2 full architectural snapshot. */
#define GCN_TR_F_FP_VALID   0x00000001u  /* ps_hash / fpscr are meaningful   */
#define GCN_TR_F_BRANCH     0x00000002u  /* this insn changed control flow   */
#define GCN_TR_F_EXCEPTION  0x00000004u  /* npc redirected by an exception   */
GCN_PACK_PUSH
typedef struct GCN_PACKED gcn_tr_retired {
    uint32_t insn;      /* raw opcode word (decoded big-endian -> host u32)  */
    uint32_t pc;
    uint32_t npc;
    uint32_t lr;        /* SPR_LR   */
    uint32_t ctr;       /* SPR_CTR  */
    uint32_t cr;        /* condition register (packed 32-bit)                */
    uint32_t xer;       /* reconstructed XER                                 */
    uint32_t msr;
    uint32_t fpscr;     /* valid iff GCN_TR_F_FP_VALID                       */
    uint32_t gpr[32];
    uint64_t ps_hash;   /* hash of ps[0..31] both lanes; valid iff FP_VALID  */
    uint32_t flags;     /* GCN_TR_F_*                                        */
    uint32_t _pad0;
} gcn_tr_retired;
GCN_PACK_POP

/* memory write to MEM1. value_hi used only for size==8 (stfd/psq_st pair). */
GCN_PACK_PUSH
typedef struct GCN_PACKED gcn_tr_mem {
    uint32_t pc;
    uint32_t addr;
    uint32_t value;      /* low 32 bits (or whole value for size<=4)         */
    uint32_t value_hi;   /* high 32 bits for size==8                         */
    uint8_t  size;       /* 1,2,4,8                                          */
    uint8_t  _pad[3];
} gcn_tr_mem;
GCN_PACK_POP

/* MMIO block ids (Flipper register groups at 0xCC00xxxx). */
enum gcn_mmio_block {
    GCN_MMIO_CP = 0, GCN_MMIO_PE, GCN_MMIO_VI, GCN_MMIO_PI, GCN_MMIO_MI,
    GCN_MMIO_DSP, GCN_MMIO_DI, GCN_MMIO_SI, GCN_MMIO_EXI, GCN_MMIO_AI,
    GCN_MMIO_OTHER = 255
};
GCN_PACK_PUSH
typedef struct GCN_PACKED gcn_tr_mmio {
    uint32_t pc;
    uint32_t addr;
    uint32_t value;
    uint8_t  size;       /* 1,2,4                                            */
    uint8_t  is_write;   /* 1 = write, 0 = read                              */
    uint8_t  block;      /* enum gcn_mmio_block                              */
    uint8_t  _pad;
} gcn_tr_mmio;
GCN_PACK_POP

enum gcn_dma_device { GCN_DMA_ARAM = 0, GCN_DMA_DI, GCN_DMA_AI, GCN_DMA_SI, GCN_DMA_EXI, GCN_DMA_OTHER = 255 };
GCN_PACK_PUSH
typedef struct GCN_PACKED gcn_tr_dma {
    uint32_t pc;
    uint32_t src;
    uint32_t dst;
    uint32_t len;
    uint8_t  device;     /* enum gcn_dma_device                             */
    uint8_t  dir;        /* 0 = to main mem, 1 = from main mem              */
    uint8_t  _pad[2];
} gcn_tr_dma;
GCN_PACK_POP

/* PI interrupt sources (subset the IPL menu exercises). */
enum gcn_intr_source {
    GCN_INT_ERROR = 0, GCN_INT_RSW, GCN_INT_DI, GCN_INT_SI, GCN_INT_EXI,
    GCN_INT_AI, GCN_INT_DSP, GCN_INT_MEMINT, GCN_INT_VI, GCN_INT_PE_TOKEN,
    GCN_INT_PE_FINISH, GCN_INT_CP, GCN_INT_DEBUG, GCN_INT_HSP,
    GCN_INT_EXCEPTION = 200 /* non-PI CPU exception (DSI/ISI/dec/...)        */
};
GCN_PACK_PUSH
typedef struct GCN_PACKED gcn_tr_intr {
    uint32_t taken_pc;      /* handler entry PC (exception vector)           */
    uint32_t cause;         /* source-specific cause / exception SRR1 bits   */
    uint32_t pending_mask;  /* PI INTSR pending bits                         */
    uint32_t intmask;       /* PI INTMR mask bits                            */
    uint8_t  source;        /* enum gcn_intr_source                          */
    uint8_t  _pad[3];
} gcn_tr_intr;
GCN_PACK_POP

/* EXI transaction. cmd/resp streams are captured as an inline prefix plus a
 * CRC32 over the entire stream so full fidelity is checkable without bloating
 * the record. */
#define GCN_EXI_INLINE 16
GCN_PACK_PUSH
typedef struct GCN_PACKED gcn_tr_exi {
    uint32_t pc;
    uint8_t  channel;      /* 0..2                                          */
    uint8_t  device;       /* selected device index on the channel          */
    uint8_t  select;       /* 1 = CS asserted, 0 = deasserted (end of xfer) */
    uint8_t  cmd_len;      /* total command bytes in the transaction        */
    uint8_t  resp_len;     /* total response bytes                          */
    uint8_t  cmd_inline;   /* bytes stored in cmd[]  (<= GCN_EXI_INLINE)    */
    uint8_t  resp_inline;  /* bytes stored in resp[] (<= GCN_EXI_INLINE)    */
    uint8_t  _pad;
    uint32_t cmd_crc;      /* CRC32 over the full command stream            */
    uint32_t resp_crc;     /* CRC32 over the full response stream           */
    uint8_t  cmd[GCN_EXI_INLINE];
    uint8_t  resp[GCN_EXI_INLINE];
} gcn_tr_exi;
GCN_PACK_POP

/* GX FIFO span + parsed packet + PE completion events. */
enum gcn_gx_event { GCN_GX_NONE = 0, GCN_GX_DRAWDONE, GCN_GX_TOKEN, GCN_GX_FINISH };
GCN_PACK_PUSH
typedef struct GCN_PACKED gcn_tr_gx {
    uint32_t pc;
    uint32_t fifo_addr;    /* start of the written FIFO span in MEM1        */
    uint32_t fifo_len;     /* span length in bytes                          */
    uint32_t fifo_crc;     /* CRC32 over the span bytes                     */
    uint16_t packet_type;  /* parsed CP/BP/XF opcode or primitive type      */
    uint16_t event;        /* enum gcn_gx_event                             */
    uint32_t token;        /* PE token value (for GCN_GX_TOKEN)             */
} gcn_tr_gx;
GCN_PACK_POP

enum gcn_vi_event { GCN_VI_NONE = 0, GCN_VI_RETRACE, GCN_VI_XFB_SET };
GCN_PACK_PUSH
typedef struct GCN_PACKED gcn_tr_vi {
    uint32_t pc;
    uint32_t xfb_addr;     /* external framebuffer base scanned out         */
    uint16_t event;        /* enum gcn_vi_event                             */
    uint16_t field;        /* 0/1 for interlaced field                      */
    uint32_t line;         /* current vertical position / vcount            */
} gcn_tr_vi;
GCN_PACK_POP

enum gcn_audio_subsys { GCN_AUD_DSP = 0, GCN_AUD_AI, GCN_AUD_ARAM };
enum gcn_audio_event  {
    GCN_AUD_NONE = 0, GCN_AUD_MAIL_TO_DSP, GCN_AUD_MAIL_FROM_DSP,
    GCN_AUD_ARAM_DMA_START, GCN_AUD_ARAM_DMA_DONE, GCN_AUD_AI_START, GCN_AUD_AI_STOP
};
GCN_PACK_PUSH
typedef struct GCN_PACKED gcn_tr_dsp {
    uint32_t pc;
    uint32_t mail_hi;      /* CPU<->DSP mailbox high half                   */
    uint32_t mail_lo;      /* CPU<->DSP mailbox low half                    */
    uint32_t control;      /* DSP control/status (CSR)                      */
    uint32_t aram_addr;    /* ARAM DMA main-memory address                  */
    uint32_t aram_aram;    /* ARAM DMA ARAM-side address                    */
    uint32_t aram_len;     /* ARAM DMA length                               */
    uint16_t subsys;       /* enum gcn_audio_subsys                         */
    uint16_t event;        /* enum gcn_audio_event                          */
    uint32_t ai_rate;      /* AI sample-rate / control bits                 */
} gcn_tr_dsp;
GCN_PACK_POP

/* ----- the tagged record ------------------------------------------------- */
GCN_PACK_PUSH
typedef struct GCN_PACKED gcn_trace_record {
    gcn_trace_hdr hdr;
    union {
        gcn_tr_retired retired;
        gcn_tr_mem     mem;
        gcn_tr_mmio    mmio;
        gcn_tr_dma     dma;
        gcn_tr_intr    intr;
        gcn_tr_exi     exi;
        gcn_tr_gx      gx;
        gcn_tr_vi      vi;
        gcn_tr_dsp     dsp;
        uint8_t        raw[180]; /* fixes union size; keep >= largest member */
    } u;
} gcn_trace_record;
GCN_PACK_POP

/* ----- file header (32 bytes, once at the top of every trace file) ------- */
GCN_PACK_PUSH
typedef struct GCN_PACKED gcn_trace_file_header {
    uint32_t magic;         /* GCN_TRACE_FILE_MAGIC                          */
    uint16_t version;       /* GCN_TRACE_VERSION                            */
    uint16_t hdr_size;      /* == sizeof(gcn_trace_file_header) (32)        */
    uint32_t record_size;   /* == GCN_TRACE_RECORD_SIZE (204)               */
    uint32_t flags;         /* reserved (0)                                 */
    uint32_t producer;      /* GCN_TRACE_PROD_*                             */
    uint32_t ipl_hash;      /* CRC32 of the descrambled IPL image, 0 if n/a */
    uint32_t reserved[2];
} gcn_trace_file_header;
GCN_PACK_POP

#define GCN_TRACE_RECORD_SIZE  204
#define GCN_TRACE_FHDR_SIZE     32

/* ----- compile-time layout guards ---------------------------------------- */
_Static_assert(sizeof(gcn_trace_hdr) == 24, "trace header must be 24 bytes");
_Static_assert(sizeof(gcn_tr_retired) == 180, "retired payload must be 180 bytes");
_Static_assert(sizeof(gcn_tr_mem) == 20, "mem payload size");
_Static_assert(sizeof(gcn_tr_mmio) == 16, "mmio payload size");
_Static_assert(sizeof(gcn_tr_dma) == 20, "dma payload size");
_Static_assert(sizeof(gcn_tr_intr) == 20, "intr payload size");
_Static_assert(sizeof(gcn_tr_exi) == 52, "exi payload size");
_Static_assert(sizeof(gcn_tr_gx) == 24, "gx payload size");
_Static_assert(sizeof(gcn_tr_vi) == 16, "vi payload size");
_Static_assert(sizeof(gcn_tr_dsp) == 36, "dsp payload size");
_Static_assert(sizeof(gcn_trace_record) == GCN_TRACE_RECORD_SIZE, "record must be 204 bytes");
_Static_assert(sizeof(gcn_trace_file_header) == GCN_TRACE_FHDR_SIZE, "file header must be 32 bytes");

/* ----- tiny inline helper (optional; no libc dependency) ----------------- */
static inline void gcn_trace_file_header_init(gcn_trace_file_header *h, uint32_t producer, uint32_t ipl_hash) {
    h->magic = GCN_TRACE_FILE_MAGIC;
    h->version = (uint16_t)GCN_TRACE_VERSION;
    h->hdr_size = (uint16_t)GCN_TRACE_FHDR_SIZE;
    h->record_size = (uint32_t)GCN_TRACE_RECORD_SIZE;
    h->flags = 0u;
    h->producer = producer;
    h->ipl_hash = ipl_hash;
    h->reserved[0] = 0u;
    h->reserved[1] = 0u;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GCN_ORACLE_TRACE_FORMAT_H */
