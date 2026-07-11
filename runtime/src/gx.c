/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GX FIFO consumer (impl). See include/gx/gx.h for scope and the exact Dolphin
 * files/lines every behavior is transcribed from. This is the GPU side of the
 * FIFO handshake: it drains command bytes gp.c parked in the guest-RAM FIFO
 * ring, decodes + executes whole commands, and advances the CP read pointer /
 * read-write distance exactly as Dolphin's single-core RunGpuLoop does.
 */
#include "gx/gx.h"
#include "debug/rings.h"

#include <stdio.h>
#include <string.h>

/* Staging ("video buffer") capacity. Fifo.cpp keeps a linear buffer that
 * accumulates 32-byte chunks and is consumed a whole-command-at-a-time; leftover
 * partial commands persist to the next chunk. 256 KiB comfortably holds any
 * single command the IPL emits (the largest is a bounded XF/BP/DL load — draws
 * are skipped, not buffered whole beyond their payload). Overflow is loud. */
#define GX_BUF_CAP  (256u * 1024u)

/* CP LoadCPReg sub-command classes (CPMemory.h:28-55). */
#define GX_CP_MATINDEX_A   0x30u
#define GX_CP_MATINDEX_B   0x40u
#define GX_CP_VCD_LO       0x50u
#define GX_CP_VCD_HI       0x60u
#define GX_CP_VAT_REG_A    0x70u
#define GX_CP_VAT_REG_B    0x80u
#define GX_CP_VAT_REG_C    0x90u
#define GX_CP_ARRAY_BASE   0xA0u
#define GX_CP_ARRAY_STRIDE 0xB0u
#define GX_CP_COMMAND_MASK 0xF0u

/* Opcodes (OpcodeDecoding.h:24-42). */
#define GX_OP_NOP              0x00u
#define GX_OP_LOAD_CP_REG      0x08u
#define GX_OP_LOAD_XF_REG      0x10u
#define GX_OP_LOAD_INDX_A      0x20u
#define GX_OP_LOAD_INDX_B      0x28u
#define GX_OP_LOAD_INDX_C      0x30u
#define GX_OP_LOAD_INDX_D      0x38u
#define GX_OP_CALL_DL          0x40u
#define GX_OP_UNKNOWN_METRICS  0x44u
#define GX_OP_INVL_VC          0x48u
#define GX_OP_LOAD_BP_REG      0x61u
#define GX_OP_PRIM_START       0x80u
#define GX_OP_PRIM_END         0xBFu

/* BP registers with side effects (BPMemory.h:52-64). */
#define GX_BP_SETDRAWDONE      0x45u
#define GX_BP_PE_TOKEN_ID      0x47u
#define GX_BP_PE_TOKEN_INT_ID  0x48u
#define GX_BP_EFB_TL           0x49u
#define GX_BP_EFB_WH           0x4Au
#define GX_BP_EFB_ADDR         0x4Bu
#define GX_BP_EFB_STRIDE       0x4Du
#define GX_BP_COPYYSCALE       0x4Eu
#define GX_BP_CLEAR_AR         0x4Fu
#define GX_BP_CLEAR_GB         0x50u
#define GX_BP_CLEAR_Z          0x51u
#define GX_BP_TRIGGER_EFB_COPY 0x52u

#define GX_XF_REGISTERS_START  0x1000u
#define GX_XF_REGISTERS_END    0x1058u   /* XFMemory.h:240 (register region end) */
#define GX_XF_MEM_WORDS        0x1058u   /* covers matrix/light mem + registers  */

/* CP-state mirror needed to size vertices (CPMemory.h CPState:744-761). Only the
 * fields that affect vertex size are consulted; all are stored faithfully. */
typedef struct {
    u32 vtx_desc_lo;         /* TVtxDesc::Low.Hex  (VCD_LO) */
    u32 vtx_desc_hi;         /* TVtxDesc::High.Hex (VCD_HI) */
    u32 vat_g0[8];           /* UVAT_group0 per format index */
    u32 vat_g1[8];           /* UVAT_group1 */
    u32 vat_g2[8];           /* UVAT_group2 */
    u32 array_bases[16];
    u32 array_strides[16];
    u32 matrix_index_a;
    u32 matrix_index_b;
} GxCpState;

typedef struct {
    CPUState* cpu;
    GcnCp*    cp;
    GcnPe*    pe;

    GxCpState cpst;
    u32 bp[256];                     /* BPMemory register file */
    u32 xf[GX_XF_MEM_WORDS];         /* XF matrix/light memory + registers */

    u8  buf[GX_BUF_CAP];             /* linear staging (Fifo.cpp video buffer) */
    u32 buf_len;
    int dl_depth;                    /* display-list recursion guard */

    /* one-time-log bitsets (report which regs/opcodes the IPL exercised) */
    u8 seen_opcode[256];
    u8 seen_bp[256];
    u8 seen_cp[256];
    u8 seen_prim[8];
    u8 seen_xf_reg[0x60];            /* 0x1000..0x105F register loads */
    u8 seen_xf_mem;                  /* any matrix/light-memory load  */
    /* dedicated EFB-copy log flags (must NOT reuse seen_bp[] — TEV_KSEL occupies
     * BP 0xF6..0xFD and BP_MASK is 0xFE, so those slots are real guest regs) */
    u8 seen_efb_clear, seen_efb_content, seen_efb_tex, seen_efb_zero, seen_efb_oob;
} GcnGx;

static GcnGx s_gx;

/* Log a first-occurrence once; returns 1 the first time a flag is raised. */
static int note_once(u8* flag) {
    if (*flag) return 0;
    *flag = 1;
    return 1;
}

/* ---- big-endian readers (FIFO bytes are GameCube big-endian) ---- */
static u32 rd32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}
static u32 rd24(const u8* p) {
    return ((u32)p[0] << 16) | ((u32)p[1] << 8) | (u32)p[2];
}
static u16 rd16(const u8* p) {
    return (u16)(((u16)p[0] << 8) | (u16)p[1]);
}

/* ============================================================================
 * Vertex size computation — transcribed from VertexLoaderBase::GetVertexSize
 * (VertexLoaderBase.cpp:175-203) + the per-component size tables in
 * VertexLoader_{Position,Normal,Color,TextCoord}.h. Component/format enums are
 * CPMemory.h:100-224. A wrong size desyncs the FIFO, so this must be exact.
 * ==========================================================================*/

/* GetElementSize (CPMemory.h:142-161): 0/1 -> 1, 2/3 -> 2, 4..7 -> 4. */
static u32 elem_size(u32 fmt) {
    switch (fmt & 7u) {
    case 0: case 1: return 1;
    case 2: case 3: return 2;
    default:        return 4;   /* Float + InvalidFloat5..7 behave as float */
    }
}

/* VertexComponentFormat: 0 NotPresent, 1 Direct, 2 Index8, 3 Index16. */
enum { VCF_NONE = 0, VCF_DIRECT = 1, VCF_INDEX8 = 2, VCF_INDEX16 = 3 };

/* VertexLoader_Position.h s_table_size. */
static u32 pos_size(u32 type, u32 fmt, u32 elements /*0 XY,1 XYZ*/) {
    switch (type) {
    case VCF_NONE:    return 0;
    case VCF_INDEX8:  return 1;
    case VCF_INDEX16: return 2;
    default:          return (elements ? 3u : 2u) * elem_size(fmt);   /* Direct */
    }
}

/* VertexLoader_Normal.h s_table_size (elements: 0 N, 1 NTB; index3 bool). */
static u32 norm_size(u32 type, u32 fmt, u32 elements, u32 index3) {
    switch (type) {
    case VCF_NONE:    return 0;
    case VCF_DIRECT:  return (elements ? 9u : 3u) * elem_size(fmt);
    case VCF_INDEX8:  return elements ? (index3 ? 3u : 1u) : 1u;
    default:          return elements ? (index3 ? 6u : 2u) : 2u;      /* Index16 */
    }
}

/* VertexLoader_Color.h s_table_size (Direct sizes keyed by ColorFormat 0..5). */
static u32 color_size(u32 type, u32 cfmt) {
    static const u32 direct[6] = { 2, 3, 4, 2, 3, 4 };  /* 565,888,888x,4444,6666,8888 */
    switch (type) {
    case VCF_NONE:    return 0;
    case VCF_INDEX8:  return 1;
    case VCF_INDEX16: return 2;
    default:          return (cfmt <= 5u) ? direct[cfmt] : 0u;        /* Direct */
    }
}

/* VertexLoader_TextCoord.h s_table_size (elements: 0 S, 1 ST). */
static u32 tc_size(u32 type, u32 fmt, u32 elements) {
    switch (type) {
    case VCF_NONE:    return 0;
    case VCF_INDEX8:  return 1;
    case VCF_INDEX16: return 2;
    default:          return (elements ? 2u : 1u) * elem_size(fmt);   /* Direct */
    }
}

/* Per-tex-coord VAT field extraction (CPMemory.h VAT::GetTexFormat/Elements). */
static u32 tex_elements(const GxCpState* s, u32 vat, u32 idx) {
    u32 g0 = s->vat_g0[vat], g1 = s->vat_g1[vat], g2 = s->vat_g2[vat];
    switch (idx) {
    case 0: return (g0 >> 21) & 1u;
    case 1: return (g1 >> 0)  & 1u;
    case 2: return (g1 >> 9)  & 1u;
    case 3: return (g1 >> 18) & 1u;
    case 4: return (g1 >> 27) & 1u;
    case 5: return (g2 >> 5)  & 1u;
    case 6: return (g2 >> 14) & 1u;
    default:return (g2 >> 23) & 1u;
    }
}
static u32 tex_format(const GxCpState* s, u32 vat, u32 idx) {
    u32 g0 = s->vat_g0[vat], g1 = s->vat_g1[vat], g2 = s->vat_g2[vat];
    switch (idx) {
    case 0: return (g0 >> 22) & 7u;
    case 1: return (g1 >> 1)  & 7u;
    case 2: return (g1 >> 10) & 7u;
    case 3: return (g1 >> 19) & 7u;
    case 4: return (g1 >> 28) & 7u;
    case 5: return (g2 >> 6)  & 7u;
    case 6: return (g2 >> 15) & 7u;
    default:return (g2 >> 24) & 7u;
    }
}

static u32 popcount9(u32 v) {
    u32 n = 0;
    v &= 0x1FFu;
    while (v) { n += v & 1u; v >>= 1; }
    return n;
}

/* Full vertex size for a VAT index (VertexLoaderBase.cpp:175-203). */
static u32 gx_vertex_size(const GxCpState* s, u32 vat) {
    u32 low = s->vtx_desc_lo, high = s->vtx_desc_hi;
    u32 g0 = s->vat_g0[vat];
    u32 size = popcount9(low);   /* PosMatIdx + 8 TexMatIdx, one byte each */

    size += pos_size((low >> 9) & 3u, (g0 >> 1) & 7u, g0 & 1u);
    size += norm_size((low >> 11) & 3u, (g0 >> 10) & 7u, (g0 >> 9) & 1u, (g0 >> 31) & 1u);

    /* Two color channels (CPMemory.h Color0Comp bits14-16, Color1Comp bits18-20) */
    size += color_size((low >> 13) & 3u, (g0 >> 14) & 7u);
    size += color_size((low >> 15) & 3u, (g0 >> 18) & 7u);

    for (u32 i = 0; i < 8; i++) {
        u32 type = (high >> (2u * i)) & 3u;
        size += tc_size(type, tex_format(s, vat, i), tex_elements(s, vat, i));
    }
    return size;
}

/* ============================================================================
 * CP register load (CPMemory.cpp LoadCPReg:90-199). Only VCD/VAT affect vertex
 * size; everything is stored faithfully anyway.
 * ==========================================================================*/
static void gx_on_cp(GcnGx* gx, u8 cmd, u32 value) {
    if (note_once(&gx->seen_cp[cmd]))
        fprintf(stderr, "gx: CP reg 0x%02X first loaded (val 0x%08X)\n", cmd, value);

    GxCpState* s = &gx->cpst;
    switch (cmd & GX_CP_COMMAND_MASK) {
    case GX_CP_MATINDEX_A:   s->matrix_index_a = value; break;
    case GX_CP_MATINDEX_B:   s->matrix_index_b = value; break;
    case GX_CP_VCD_LO:       s->vtx_desc_lo = value; break;
    case GX_CP_VCD_HI:       s->vtx_desc_hi = value; break;
    case GX_CP_VAT_REG_A:    s->vat_g0[cmd & 7u] = value; break;
    case GX_CP_VAT_REG_B:    s->vat_g1[cmd & 7u] = value; break;
    case GX_CP_VAT_REG_C:    s->vat_g2[cmd & 7u] = value; break;
    case GX_CP_ARRAY_BASE:   s->array_bases[cmd & 0xFu]   = value & 0x1FFFFFFFu; break;
    case GX_CP_ARRAY_STRIDE: s->array_strides[cmd & 0xFu] = value & 0xFFu; break;
    default:
        /* 0x00/0x10/0x20 are perf-query commands (LoadCPReg:94-104, no state). */
        break;
    }
}

/* ============================================================================
 * XF register load (OpcodeDecoding.h:157-175 -> LoadXFReg). Store `count` u32
 * words starting at `address` into the XF memory array. Matrix/light memory is
 * <0x1000; registers are 0x1000..0x1057.
 * ==========================================================================*/
static void gx_on_xf(GcnGx* gx, u16 address, u8 count, const u8* data) {
    if (address >= GX_XF_REGISTERS_START &&
        address < GX_XF_REGISTERS_START + 0x60u) {
        if (note_once(&gx->seen_xf_reg[address - GX_XF_REGISTERS_START]))
            fprintf(stderr, "gx: XF register 0x%04X first loaded (count %u)\n",
                    address, count);
    } else if (address < GX_XF_REGISTERS_START) {
        if (note_once(&gx->seen_xf_mem))
            fprintf(stderr, "gx: XF matrix/light memory first loaded "
                            "(addr 0x%04X count %u)\n", address, count);
    }

    for (u8 i = 0; i < count; i++) {
        u32 addr = (u32)address + i;
        if (addr < GX_XF_MEM_WORDS) {
            gx->xf[addr] = rd32(&data[i * 4u]);
        } else {
            static int warned = 0;
            if (!warned) {
                fprintf(stderr, "gx: XF load out of range (addr 0x%04X) — ignored\n",
                        addr);
                warned = 1;
            }
        }
    }
}

/* ============================================================================
 * EFB copy (BPStructs.cpp BPWritten BPMEM_TRIGGER_EFB_COPY:240-395). This
 * increment implements ONLY the copy-CLEAR to the XFB (fill the destination with
 * the clear color as YUY2); real EFB content copies are deferred loudly.
 * ==========================================================================*/

/* ConvertColorToYUV (SWEfbInterface.cpp:546-562), color = 0xRRGGBBAA. */
static void color_to_yuv(u32 color, u8* Y, u8* U, u8* V) {
    int r = (int)((color >> 24) & 0xFF);
    int g = (int)((color >> 16) & 0xFF);
    int b = (int)((color >> 8)  & 0xFF);
    int y =  66 * r + 129 * g +  25 * b;
    int u = -38 * r -  74 * g + 112 * b;
    int v = 112 * r -  94 * g -  18 * b;
    int y_round = (y >> 8) + ((y >> 7) & 1);
    int u_round = (u >> 8) + ((u >> 7) & 1);   /* signed */
    int v_round = (v >> 8) + ((v >> 7) & 1);   /* signed */
    /* EncodeXFB downsample (SWEfbInterface.cpp:643-652): Y += 16; a uniform field
     * makes the 1/4+1/2+1/4 U/V filter the identity, so UV = 128 + U (or + V). */
    int yb = y_round + 16;
    int ub = 128 + u_round;
    int vb = 128 + v_round;
    *Y = (u8)(yb < 0 ? 0 : yb > 255 ? 255 : yb);
    *U = (u8)(ub < 0 ? 0 : ub > 255 ? 255 : ub);
    *V = (u8)(vb < 0 ? 0 : vb > 255 ? 255 : vb);
}

static void gx_efb_copy(GcnGx* gx) {
    u32 copy = gx->bp[GX_BP_TRIGGER_EFB_COPY];
    int clear        = (copy >> 11) & 1;
    int copy_to_xfb  = (copy >> 14) & 1;
    int scale_invert = (copy >> 10) & 1;

    /* Source rect (X10Y10: x = bits0-9, y = bits10-19). BPStructs.cpp:250-256. */
    u32 srcXY = gx->bp[GX_BP_EFB_TL];
    u32 srcWH = gx->bp[GX_BP_EFB_WH];
    u32 wh_x = srcWH & 0x3FFu;
    u32 wh_y = (srcWH >> 10) & 0x3FFu;
    u32 copy_width = wh_x + 1u;                        /* srcRect.GetWidth() */
    (void)srcXY;

    u32 dest_addr   = gx->bp[GX_BP_EFB_ADDR] << 5;     /* copyTexDest << 5 */
    u32 dest_stride = gx->bp[GX_BP_EFB_STRIDE] << 5;   /* copyDestStride << 5 */
    u32 yscale_reg  = gx->bp[GX_BP_COPYYSCALE];        /* dispcopyyscale */

    if (!copy_to_xfb) {
        if (note_once(&gx->seen_efb_tex))
            fprintf(stderr, "gx: EFB->texture copy deferred (no rasterizer) "
                            "[dest 0x%08X w %u]\n", dest_addr, copy_width);
        return;
    }
    if (!clear) {
        if (note_once(&gx->seen_efb_content))
            fprintf(stderr, "gx: EFB content copy -> XFB deferred (no rasterizer) "
                            "[dest 0x%08X w %u] — destination left untouched\n",
                    dest_addr, copy_width);
        return;
    }

    /* yScale (BPStructs.cpp:322-330). num_xfb_lines = 1 + wh_y * yScale. */
    double yscale;
    if (scale_invert)
        yscale = (yscale_reg != 0) ? 256.0 / (double)yscale_reg : 1.0;
    else
        yscale = (double)yscale_reg / 256.0;
    u32 height = (u32)(1.0 + (double)wh_y * yscale);
    if (height < 1u) height = 1u;

    /* Clear color (EfbCopy.cpp:18-19): 0xRRGGBBAA from clearcolorAR/GB. */
    u32 ar = gx->bp[GX_BP_CLEAR_AR];
    u32 gb = gx->bp[GX_BP_CLEAR_GB];
    u32 clear_color = ((ar & 0xFFu) << 24) | (gb << 8) | ((ar & 0xFF00u) >> 8);

    u8 Y, U, V;
    color_to_yuv(clear_color, &Y, &U, &V);

    CPUState* cpu = gx->cpu;
    u32 phys_base = dest_addr & 0x1FFFFFFFu;
    if (dest_stride == 0u || !cpu || !cpu->ram) {
        if (note_once(&gx->seen_efb_zero))
            fprintf(stderr, "gx: EFB copy-clear with zero stride / no RAM — skipped\n");
        return;
    }
    /* Bounds: last byte touched is at phys_base + (height-1)*stride + width*2. */
    u64 last = (u64)phys_base + (u64)(height - 1u) * dest_stride + (u64)copy_width * 2u;
    if (last > (u64)cpu->ram_size) {
        if (note_once(&gx->seen_efb_oob))
            fprintf(stderr, "gx: EFB copy-clear XFB region out of MEM1 range "
                            "(dest 0x%08X stride %u h %u w %u) — skipped\n",
                    dest_addr, dest_stride, height, copy_width);
        return;
    }

    /* Fill the XFB region with the clear color as YUY2 (bytes Y0,U,Y1,V per 2px,
     * matching the debug-server screenshot decoder + Dolphin EncodeXFB layout). */
    for (u32 row = 0; row < height; row++) {
        u8* dst = cpu->ram + phys_base + (u64)row * dest_stride;
        for (u32 x = 0; x < copy_width; x++) {
            dst[x * 2u]      = Y;
            dst[x * 2u + 1u] = (x & 1u) ? V : U;
        }
    }

    if (note_once(&gx->seen_efb_clear))
        fprintf(stderr, "gx: EFB copy-CLEAR -> XFB: dest 0x%08X stride %u w %u h %u "
                        "clear 0x%08X (Y=%u U=%u V=%u)\n",
                dest_addr, dest_stride, copy_width, height, clear_color, Y, U, V);
}

/* ============================================================================
 * BP register load (BPStructs.cpp BPWritten:55-396). Store the value; run the
 * side effects for the registers that have them.
 * ==========================================================================*/
static void gx_on_bp(GcnGx* gx, u8 cmd, u32 value) {
    if (note_once(&gx->seen_bp[cmd]))
        fprintf(stderr, "gx: BP reg 0x%02X first written (val 0x%06X)\n", cmd, value);

    gx->bp[cmd] = value;

    switch (cmd) {
    case GX_BP_SETDRAWDONE:            /* BPStructs.cpp:180-201 */
        if ((value & 0xFFu) == 0x02u) {
            gcn_pe_set_finish(gx->pe);
            fprintf(stderr, "gx: GXSetDrawDone -> PE finish (val 0x%04X)\n",
                    value & 0xFFFFu);
        } else {
            fprintf(stderr, "gx: GXSetDrawDone ??? (val 0x%04X)\n", value & 0xFFFFu);
        }
        break;
    case GX_BP_PE_TOKEN_ID:            /* BPStructs.cpp:202-217 (no interrupt) */
        gcn_pe_set_token(gx->pe, (u16)(value & 0xFFFFu), 0);
        break;
    case GX_BP_PE_TOKEN_INT_ID:        /* BPStructs.cpp:218-233 (interrupt) */
        gcn_pe_set_token(gx->pe, (u16)(value & 0xFFFFu), 1);
        break;
    case GX_BP_TRIGGER_EFB_COPY:       /* BPStructs.cpp:240-395 */
        gx_efb_copy(gx);
        break;
    default:
        /* All other BP regs: state storage only (that IS their hardware effect
         * until a rasterizer reads them). Already stored above. */
        break;
    }
}

/* ============================================================================
 * Opcode decode + execute — mirrors OpcodeDecoder::detail::RunCommand
 * (OpcodeDecoding.h:125-253). Returns the number of bytes consumed, or 0 if the
 * available bytes do not yet hold a whole command (NotEnoughData). A return of 0
 * makes the caller stop and wait for more FIFO data.
 * ==========================================================================*/
static u32 gx_run(GcnGx* gx, const u8* data, u32 available);   /* fwd (CALL_DL) */

static u32 gx_run_command(GcnGx* gx, const u8* data, u32 available) {
    if (available < 1u)
        return 0;

    const u8 op = data[0];

    switch (op) {
    case GX_OP_NOP: {
        u32 count = 1;
        while (count < available && data[count] == GX_OP_NOP)
            count++;
        if (note_once(&gx->seen_opcode[GX_OP_NOP]))
            fprintf(stderr, "gx: opcode NOP first seen\n");
        return count;
    }

    case GX_OP_LOAD_CP_REG: {
        if (available < 6u) return 0;
        if (note_once(&gx->seen_opcode[op]))
            fprintf(stderr, "gx: opcode LOAD_CP_REG first seen\n");
        gx_on_cp(gx, data[1], rd32(&data[2]));
        return 6;
    }

    case GX_OP_LOAD_XF_REG: {
        if (available < 5u) return 0;
        u32 cmd2 = rd32(&data[1]);
        u32 stream_size_temp = cmd2 >> 16;
        if (stream_size_temp >= 16u) {
            static int warned = 0;
            if (!warned) {
                fprintf(stderr, "gx: LOAD_XF_REG stream size field 0x%X >= 16 "
                                "(cmd2 0x%08X) — masking to 4 bits\n",
                        stream_size_temp, cmd2);
                warned = 1;
            }
        }
        u32 stream_size = (stream_size_temp & 0xFu) + 1u;
        if (available < 5u + stream_size * 4u) return 0;
        if (note_once(&gx->seen_opcode[op]))
            fprintf(stderr, "gx: opcode LOAD_XF_REG first seen\n");
        gx_on_xf(gx, (u16)(cmd2 & 0xFFFFu), (u8)stream_size, &data[5]);
        return 5u + stream_size * 4u;
    }

    case GX_OP_LOAD_INDX_A:
    case GX_OP_LOAD_INDX_B:
    case GX_OP_LOAD_INDX_C:
    case GX_OP_LOAD_INDX_D: {
        if (available < 5u) return 0;
        if (note_once(&gx->seen_opcode[op]))
            fprintf(stderr, "gx: opcode LOAD_INDX_%c UNIMPLEMENTED (indexed XF "
                            "load) — payload consumed, load skipped\n",
                    'A' + (int)((op - GX_OP_LOAD_INDX_A) / 8));
        return 5;
    }

    case GX_OP_CALL_DL: {
        if (available < 9u) return 0;
        if (note_once(&gx->seen_opcode[op]))
            fprintf(stderr, "gx: opcode CALL_DL first seen\n");
        u32 addr = rd32(&data[1]) & ~31u;
        u32 size = rd32(&data[5]) & ~31u;

        /* OnDisplayList (OpcodeDecoding.cpp:143-200): recursion is not allowed —
         * Dolphin warns and skips a nested DL. Run the DL bytes from guest RAM. */
        if (gx->dl_depth > 0) {
            static int warned = 0;
            if (!warned) {
                fprintf(stderr, "gx: recursive display list detected — skipped\n");
                warned = 1;
            }
            return 9;
        }
        CPUState* cpu = gx->cpu;
        u32 phys = addr & 0x1FFFFFFFu;
        if (cpu && cpu->ram && size > 0u &&
            (u64)phys + (u64)size <= (u64)cpu->ram_size) {
            gx->dl_depth++;
            gx_run(gx, cpu->ram + phys, size);
            gx->dl_depth--;
        } else if (size > 0u) {
            static int warned = 0;
            if (!warned) {
                fprintf(stderr, "gx: display list 0x%08X size %u out of MEM1 range "
                                "— skipped\n", addr, size);
                warned = 1;
            }
        }
        return 9;
    }

    case GX_OP_UNKNOWN_METRICS:   /* 0x44: OnUnknown (OpcodeDecoding.cpp:207-212) */
        if (note_once(&gx->seen_opcode[op]))
            fprintf(stderr, "gx: opcode 0x44 (unknown metrics) — no-op\n");
        return 1;

    case GX_OP_INVL_VC:           /* 0x48: invalidate vertex cache — no-op */
        if (note_once(&gx->seen_opcode[op]))
            fprintf(stderr, "gx: opcode INVL_VC (vertex-cache invalidate) — no-op\n");
        return 1;

    case GX_OP_LOAD_BP_REG: {
        if (available < 5u) return 0;
        if (note_once(&gx->seen_opcode[op]))
            fprintf(stderr, "gx: opcode LOAD_BP_REG first seen\n");
        gx_on_bp(gx, data[1], rd24(&data[2]));
        return 5;
    }

    default:
        if (op >= GX_OP_PRIM_START && op <= GX_OP_PRIM_END) {
            if (available < 3u) return 0;
            u32 prim = (op >> 3) & 7u;       /* GX_PRIMITIVE_MASK 0x78 >> 3 */
            u32 vat  = op & 7u;              /* GX_VAT_MASK */
            u32 vsize = gx_vertex_size(&gx->cpst, vat);
            u32 nverts = rd16(&data[1]);

            /* A wrong vertex size desyncs the stream; a nonzero primitive with a
             * zero computed size means VCD/VAT were never set (or are corrupt).
             * ERROR loudly and stop the drain rather than guess (GX_PLAN). */
            if (nverts > 0u && vsize == 0u) {
                static int errored = 0;
                if (!errored) {
                    fprintf(stderr, "gx: PRIMITIVE 0x%02X (prim %u vat %u) has %u "
                                    "vertices but computed vertex size 0 "
                                    "(VCD lo 0x%08X hi 0x%08X) — STOPPING DRAIN "
                                    "to avoid FIFO desync\n",
                            op, prim, vat, nverts,
                            gx->cpst.vtx_desc_lo, gx->cpst.vtx_desc_hi);
                    errored = 1;
                }
                return 0;   /* stop: do not advance past an unknowable payload */
            }

            u32 total = 3u + nverts * vsize;
            if (available < total) return 0;

            if (note_once(&gx->seen_prim[prim]))
                fprintf(stderr, "gx: primitive type %u (opcode 0x%02X) — geometry "
                                "rasterization deferred; %u-byte vertex payload "
                                "skipped (%u verts x %u bytes)\n",
                        prim, op, nverts * vsize, nverts, vsize);
            return total;   /* skip the whole primitive (header + vertex payload) */
        }

        /* Unknown opcode. HandleUnknownOpcode advances 1 byte (OpcodeDecoding.cpp
         * :219-224). Loud once — an unknown here usually means desync. Dump the
         * DL depth + the next bytes so the culprit command is identifiable. */
        if (note_once(&gx->seen_opcode[op])) {
            u32 ctx = available < 16u ? available : 16u;
            fprintf(stderr, "gx: UNKNOWN opcode 0x%02X (dl_depth=%d, avail=%u) — "
                            "advancing 1 byte (possible FIFO desync); next:",
                    op, gx->dl_depth, available);
            for (u32 i = 0; i < ctx; i++) fprintf(stderr, " %02X", data[i]);
            if (gx->dl_depth == 0 && data >= gx->buf && data < gx->buf + GX_BUF_CAP) {
                u32 back = (u32)(data - gx->buf);
                if (back > 24u) back = 24u;
                fprintf(stderr, " | prev:");
                for (u32 i = back; i > 0; i--) fprintf(stderr, " %02X", data[-(int)i]);
            }
            fprintf(stderr, "\n");
        }
        return 1;
    }
}

/* OpcodeDecoder::Run (OpcodeDecoding.h:267-279): consume whole commands until a
 * partial one (return 0). Returns total bytes consumed. */
static u32 gx_run(GcnGx* gx, const u8* data, u32 available) {
    u32 off = 0;
    while (off < available) {
        u32 sz = gx_run_command(gx, &data[off], available - off);
        if (sz == 0u) break;
        off += sz;
    }
    return off;
}

/* ============================================================================
 * Public API
 * ==========================================================================*/
void gcn_gx_init(CPUState* cpu, GcnCp* cp, GcnPe* pe) {
    memset(&s_gx, 0, sizeof s_gx);
    s_gx.cpu = cpu;
    s_gx.cp  = cp;
    s_gx.pe  = pe;
}

void gcn_gx_tick(u32 cycles) {
    (void)cycles;
    GcnGx* gx = &s_gx;
    if (!gx->cp || !gx->cpu)
        return;
    /* Fifo.cpp RunGpuLoop:317-320 gate: GPReadEnable && distance && !breakpoint. */
    if (!gx->cp->gp_read_enable)
        return;

    u32 drained = 0;
    while (drained < GCN_GX_DRAIN_BYTES_PER_TICK &&
           gcn_cp_fifo_rw_distance(gx->cp) >= GCN_CP_GATHER_PIPE_SIZE &&
           !gcn_cp_at_breakpoint(gx->cp)) {

        /* Read the 32 bytes at the current read pointer (Fifo.cpp ReadDataFromFifo
         * :215-236 copies a GATHER_PIPE_SIZE chunk into the video buffer). */
        u32 rptr = gcn_cp_fifo_read_pointer(gx->cp);
        u32 phys = rptr & 0x1FFFFFFFu;
        if ((u64)phys + GCN_CP_GATHER_PIPE_SIZE > (u64)gx->cpu->ram_size) {
            static int warned = 0;
            if (!warned) {
                fprintf(stderr, "gx: FIFO read pointer 0x%08X out of MEM1 range — "
                                "drain halted\n", rptr);
                warned = 1;
            }
            break;
        }
        if (gx->buf_len + GCN_CP_GATHER_PIPE_SIZE > GX_BUF_CAP) {
            static int warned = 0;
            if (!warned) {
                fprintf(stderr, "gx: staging buffer overflow (%u bytes buffered, a "
                                "single command exceeds %u) — drain halted\n",
                        gx->buf_len, GX_BUF_CAP);
                warned = 1;
            }
            break;
        }
        memcpy(gx->buf + gx->buf_len, gx->cpu->ram + phys, GCN_CP_GATHER_PIPE_SIZE);
        gx->buf_len += GCN_CP_GATHER_PIPE_SIZE;

        /* Advance the read side (wrap + distance-32 + status/interrupt eval). */
        gcn_cp_gpu_consume_chunk(gx->cp);
        drained += GCN_CP_GATHER_PIPE_SIZE;

        /* Run whole commands out of the staging buffer; keep the leftover partial
         * command for the next chunk (Fifo.cpp:342-352 advances the read ptr past
         * consumed commands only). */
        u32 consumed = gx_run(gx, gx->buf, gx->buf_len);
        if (consumed > 0u) {
            if (consumed < gx->buf_len)
                memmove(gx->buf, gx->buf + consumed, gx->buf_len - consumed);
            gx->buf_len -= consumed;
        }
    }
}
