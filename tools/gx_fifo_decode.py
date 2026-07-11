#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# GX FIFO command-stream decoder for gcnrecomp.
#
# Reads a fifo_dump JSON (the always-on GX FIFO recorder ring: entries[].data =
# 64 hex chars = 32 bytes big-endian per gather-pipe burst, plus .pc/.wptr/.seq),
# concatenates the bursts in seq order into one contiguous GX command stream, and
# walks it exactly the way Dolphin's VideoCommon/OpcodeDecoding.{h,cpp} does.
#
# One line per command: byte offset, the guest pc of the burst the command STARTED
# in, opcode name, and decoded fields (BP/CP/XF register NAME + value; draws:
# primitive/vat/count/computed vertex size; CALL_DL: addr+size).
#
# Unknown/unexpected opcodes ERROR with offset+context; they are never skipped.
# A trailing partial command (payload runs past the captured stream) is reported,
# not silently dropped.
#
#   python tools/gx_fifo_decode.py <fifo_dump.json> [--summary] [--verbose-xf]
#
# stdlib only; mirrors the style of tools/gcn_debug_client.py.

import json
import struct
import sys

# ---------------------------------------------------------------------------
# Opcode values (OpcodeDecoding.h)
# ---------------------------------------------------------------------------
GX_NOP          = 0x00
GX_LOAD_CP_REG  = 0x08
GX_LOAD_XF_REG  = 0x10
GX_LOAD_INDX_A  = 0x20
GX_LOAD_INDX_B  = 0x28
GX_LOAD_INDX_C  = 0x30
GX_LOAD_INDX_D  = 0x38
GX_CMD_CALL_DL       = 0x40
GX_CMD_UNKNOWN_METRICS = 0x44
GX_CMD_INVL_VC  = 0x48
GX_LOAD_BP_REG  = 0x61
GX_PRIMITIVE_START = 0x80
GX_PRIMITIVE_END   = 0xBF

GX_PRIMITIVE_MASK  = 0x78
GX_PRIMITIVE_SHIFT = 3
GX_VAT_MASK        = 0x07

PRIMITIVE_NAMES = {
    0x0: "GX_DRAW_QUADS",
    0x1: "GX_DRAW_QUADS_2",
    0x2: "GX_DRAW_TRIANGLES",
    0x3: "GX_DRAW_TRIANGLE_STRIP",
    0x4: "GX_DRAW_TRIANGLE_FAN",
    0x5: "GX_DRAW_LINES",
    0x6: "GX_DRAW_LINE_STRIP",
    0x7: "GX_DRAW_POINTS",
}

INDX_NAMES = {
    GX_LOAD_INDX_A: "LOAD_INDX_A(XF_A/posmtx)",
    GX_LOAD_INDX_B: "LOAD_INDX_B(XF_B/nrmmtx)",
    GX_LOAD_INDX_C: "LOAD_INDX_C(XF_C/postmtx)",
    GX_LOAD_INDX_D: "LOAD_INDX_D(XF_D/lights)",
}

# ---------------------------------------------------------------------------
# BP register names (BPMemory.h)
# ---------------------------------------------------------------------------
BP_NAMES = {
    0x00: "GENMODE", 0x01: "DISPLAYCOPYFILTER",
    0x06: "IND_MTXA", 0x07: "IND_MTXB", 0x08: "IND_MTXC", 0x0F: "IND_IMASK",
    0x10: "IND_CMD",
    0x20: "SCISSORTL", 0x21: "SCISSORBR", 0x22: "LINEPTWIDTH",
    0x23: "PERF0_TRI", 0x24: "PERF0_QUAD", 0x25: "RAS1_SS0", 0x26: "RAS1_SS1",
    0x27: "IREF", 0x28: "TREF",
    0x30: "SU_SSIZE", 0x31: "SU_TSIZE",
    0x40: "ZMODE", 0x41: "BLENDMODE", 0x42: "CONSTANTALPHA", 0x43: "ZCOMPARE",
    0x44: "FIELDMASK", 0x45: "SETDRAWDONE", 0x46: "BUSCLOCK0",
    0x47: "PE_TOKEN_ID", 0x48: "PE_TOKEN_INT_ID",
    0x49: "EFB_TL", 0x4A: "EFB_WH", 0x4B: "EFB_ADDR", 0x4D: "EFB_STRIDE",
    0x4E: "COPYYSCALE", 0x4F: "CLEAR_AR", 0x50: "CLEAR_GB", 0x51: "CLEAR_Z",
    0x52: "TRIGGER_EFB_COPY", 0x53: "COPYFILTER0", 0x54: "COPYFILTER1",
    0x55: "CLEARBBOX1", 0x56: "CLEARBBOX2", 0x57: "CLEAR_PIXEL_PERF",
    0x58: "REVBITS", 0x59: "SCISSOROFFSET",
    0x60: "PRELOAD_ADDR", 0x61: "PRELOAD_TMEMEVEN", 0x62: "PRELOAD_TMEMODD",
    0x63: "PRELOAD_MODE", 0x64: "LOADTLUT0", 0x65: "LOADTLUT1",
    0x66: "TEXINVALIDATE", 0x67: "PERF1", 0x68: "FIELDMODE", 0x69: "BUSCLOCK1",
    0xF3: "ALPHACOMPARE", 0xF4: "BIAS", 0xF5: "ZTEX2", 0xFE: "BP_MASK",
}


def bp_name(addr):
    if addr in BP_NAMES:
        return BP_NAMES[addr]
    if 0x80 <= addr <= 0x9F:
        return "TX_SETMODE/IMAGE[0-3]+0x%02X" % addr
    if 0xA0 <= addr <= 0xBF:
        return "TX_SETMODE/IMAGE_4+0x%02X" % addr
    if 0xC0 <= addr <= 0xDF:
        return "TEV_COLOR/ALPHA_ENV+0x%02X" % addr
    if 0xE0 <= addr <= 0xE7:
        return "TEV_REGISTER+0x%02X" % addr
    if 0xE8 <= addr <= 0xF2:
        return "FOG+0x%02X" % addr
    if 0xF6 <= addr <= 0xFD:
        return "TEV_KSEL+0x%02X" % addr
    return "BP_0x%02X" % addr


# ---------------------------------------------------------------------------
# XF register names (XFMemory.h)
# ---------------------------------------------------------------------------
XF_NAMES = {
    0x1000: "ERROR", 0x1001: "DIAG", 0x1002: "STATE0", 0x1003: "STATE1",
    0x1004: "CLOCK", 0x1005: "CLIPDISABLE", 0x1006: "SETGPMETRIC",
    0x1007: "UNKNOWN_1007", 0x1008: "INVTXSPEC(VTXSPECS)", 0x1009: "SETNUMCHAN",
    0x100a: "SETCHAN0_AMBCOLOR", 0x100b: "SETCHAN1_AMBCOLOR",
    0x100c: "SETCHAN0_MATCOLOR", 0x100d: "SETCHAN1_MATCOLOR",
    0x100e: "SETCHAN0_COLOR", 0x100f: "SETCHAN1_COLOR",
    0x1010: "SETCHAN0_ALPHA", 0x1011: "SETCHAN1_ALPHA", 0x1012: "DUALTEX",
    0x1018: "SETMATRIXINDA", 0x1019: "SETMATRIXINDB", 0x101a: "SETVIEWPORT",
    0x1020: "SETPROJECTION", 0x103f: "SETNUMTEXGENS",
    0x1040: "SETTEXMTXINFO", 0x1050: "SETPOSTMTXINFO",
}


def xf_name(addr):
    if addr in XF_NAMES:
        return XF_NAMES[addr]
    if addr < 0x0100:
        return "POSMATRIX+0x%03X" % addr
    if 0x0400 <= addr < 0x0460:
        return "NORMALMATRIX+0x%03X" % addr
    if 0x0500 <= addr < 0x0600:
        return "POSTMATRIX+0x%03X" % addr
    if 0x0600 <= addr < 0x0680:
        return "LIGHT+0x%03X" % addr
    if 0x101a <= addr <= 0x101f:
        return "SETVIEWPORT+0x%03X" % addr
    if 0x1020 <= addr <= 0x1026:
        return "SETPROJECTION+0x%03X" % addr
    if 0x1040 <= addr < 0x1048:
        return "SETTEXMTXINFO+0x%03X" % addr
    if 0x1050 <= addr < 0x1058:
        return "SETPOSTMTXINFO+0x%03X" % addr
    return "XF_0x%04X" % addr


# ---------------------------------------------------------------------------
# CP register names (CPMemory.h)
# ---------------------------------------------------------------------------
def cp_name(cmd):
    hi = cmd & 0xF0
    lo = cmd & 0x0F
    if hi == 0x30:
        return "MATINDEX_A"
    if hi == 0x40:
        return "MATINDEX_B"
    if hi == 0x50:
        return "VCD_LO"
    if hi == 0x60:
        return "VCD_HI"
    if hi == 0x70:
        return "VAT_A[%d]" % lo
    if hi == 0x80:
        return "VAT_B[%d]" % lo
    if hi == 0x90:
        return "VAT_C[%d]" % lo
    if hi == 0xA0:
        return "ARRAY_BASE[%d]" % lo
    if hi == 0xB0:
        return "ARRAY_STRIDE[%d]" % lo
    if cmd in (0x00, 0x10, 0x20):
        return "CP_PERF_UNK_0x%02X" % cmd
    return "CP_0x%02X" % cmd


# ---------------------------------------------------------------------------
# Vertex-size tables (transcribed from VertexLoader_*.h) — indexed
#   type: VertexComponentFormat 0=NotPresent 1=Direct 2=Index8 3=Index16
#   fmt : ComponentFormat 0..7 (0=UByte 1=Byte 2=UShort 3=Short 4=Float 5-7=inv)
#   elem: 0/1
# ---------------------------------------------------------------------------
# Position: [type][fmt][elem]  (elem 0=XY,1=XYZ)
POS_SIZE = {
    1: {0: (2, 3), 1: (2, 3), 2: (4, 6), 3: (4, 6),
        4: (8, 12), 5: (8, 12), 6: (8, 12), 7: (8, 12)},
    2: {f: (1, 1) for f in range(8)},
    3: {f: (2, 2) for f in range(8)},
}
# TexCoord: [type][fmt][elem]  (elem 0=S,1=ST)
TC_SIZE = {
    1: {0: (1, 2), 1: (1, 2), 2: (2, 4), 3: (2, 4),
        4: (4, 8), 5: (4, 8), 6: (4, 8), 7: (4, 8)},
    2: {f: (1, 1) for f in range(8)},
    3: {f: (2, 2) for f in range(8)},
}
# Color: [type][fmt]  (fmt is ColorFormat 0..5)
COL_SIZE = {
    1: (2, 3, 4, 2, 3, 4),
    2: (1, 1, 1, 1, 1, 1),
    3: (2, 2, 2, 2, 2, 2),
}


def normal_size(type_, index3, elem, fmt):
    # elem 0=N,1=NTB
    if type_ == 0:
        return 0
    if type_ == 1:  # Direct (index3 irrelevant)
        base = {0: 3, 1: 3, 2: 6, 3: 6, 4: 12, 5: 12, 6: 12, 7: 12}[fmt]
        return base if elem == 0 else base * 3
    if type_ == 2:  # Index8
        if elem == 0:
            return 1
        return 3 if index3 else 1
    if type_ == 3:  # Index16
        if elem == 0:
            return 2
        return 6 if index3 else 2
    return 0


class CPState:
    """Minimal CP state needed to compute draw vertex sizes."""
    def __init__(self):
        self.vcd_lo = 0
        self.vcd_hi = 0
        self.vat_g0 = [0] * 8
        self.vat_g1 = [0] * 8
        self.vat_g2 = [0] * 8

    def copy(self):
        c = CPState()
        c.vcd_lo = self.vcd_lo
        c.vcd_hi = self.vcd_hi
        c.vat_g0 = list(self.vat_g0)
        c.vat_g1 = list(self.vat_g1)
        c.vat_g2 = list(self.vat_g2)
        return c

    def load(self, cmd, value):
        hi = cmd & 0xF0
        lo = cmd & 0x0F
        if hi == 0x50:
            self.vcd_lo = value
        elif hi == 0x60:
            self.vcd_hi = value
        elif hi == 0x70:
            self.vat_g0[lo & 7] = value
        elif hi == 0x80:
            self.vat_g1[lo & 7] = value
        elif hi == 0x90:
            self.vat_g2[lo & 7] = value

    def vertex_size(self, vat):
        low = self.vcd_lo
        high = self.vcd_hi
        g0 = self.vat_g0[vat]
        g1 = self.vat_g1[vat]
        g2 = self.vat_g2[vat]

        size = bin(low & 0x1FF).count("1")  # PosMatIdx + 8 TexMatIdx bytes

        pos_type = (low >> 9) & 3
        nrm_type = (low >> 11) & 3
        col_type = [(low >> 13) & 3, (low >> 15) & 3]
        tc_type = [(high >> (2 * i)) & 3 for i in range(8)]

        pos_elem = g0 & 1
        pos_fmt = (g0 >> 1) & 7
        nrm_elem = (g0 >> 9) & 1
        nrm_fmt = (g0 >> 10) & 7
        col_elem = [(g0 >> 13) & 1, (g0 >> 17) & 1]  # unused for size
        col_fmt = [(g0 >> 14) & 7, (g0 >> 18) & 7]
        nrm_index3 = (g0 >> 31) & 1

        # tex elements/format per index (from g0/g1/g2)
        tc_elem = [0] * 8
        tc_fmt = [0] * 8
        tc_elem[0] = (g0 >> 21) & 1
        tc_fmt[0] = (g0 >> 22) & 7
        tc_elem[1] = g1 & 1;          tc_fmt[1] = (g1 >> 1) & 7
        tc_elem[2] = (g1 >> 9) & 1;   tc_fmt[2] = (g1 >> 10) & 7
        tc_elem[3] = (g1 >> 18) & 1;  tc_fmt[3] = (g1 >> 19) & 7
        tc_elem[4] = (g1 >> 27) & 1;  tc_fmt[4] = (g1 >> 28) & 7
        tc_elem[5] = (g2 >> 5) & 1;   tc_fmt[5] = (g2 >> 6) & 7
        tc_elem[6] = (g2 >> 14) & 1;  tc_fmt[6] = (g2 >> 15) & 7
        tc_elem[7] = (g2 >> 23) & 1;  tc_fmt[7] = (g2 >> 24) & 7

        if pos_type != 0:
            size += POS_SIZE[pos_type][pos_fmt][pos_elem]
        size += normal_size(nrm_type, nrm_index3, nrm_elem, nrm_fmt)
        for i in range(2):
            if col_type[i] != 0:
                cf = col_fmt[i]
                if cf > 5:
                    raise DecodeError("invalid color format %d for color%d" % (cf, i))
                size += COL_SIZE[col_type[i]][cf]
        for i in range(8):
            if tc_type[i] != 0:
                size += TC_SIZE[tc_type[i]][tc_fmt[i]][tc_elem[i]]
        return size


class DecodeError(Exception):
    pass


def swap32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def swap24(b, o):
    return (b[o] << 16) | (b[o + 1] << 8) | b[o + 2]


def swap16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def f32(b, o):
    return struct.unpack_from(">f", b, o)[0]


class Agg:
    """A collection sink for one decoded byte range (the stream, or one DL).

    The same decode_one() writes into whichever Agg is current, so the DL
    walker reuses the stream walker's opcode logic verbatim; only the sink
    (and the buffer) differ."""
    def __init__(self):
        self.commands = []     # (offset, loc, opcode_name, detail, size)
        self.bp_writes = {}    # addr -> list of values
        self.cp_writes = {}
        self.xf_writes = {}    # addr -> list of value-tuples
        self.draws = []        # (prim, vat, count, vsize)
        self.draw_full = []    # (prim, vat, count, vsize, vcd_lo, vcd_hi, g0,g1,g2)
        self.calldls = []      # (offset, addr, size)
        self.nop_count = 0
        self.efb_copies = []   # (off, loc, latch snapshot)
        self.setdrawdone = []
        self.pe_token = []
        self.pe_token_int = []
        self._bp_latch = {}    # live BP cluster tracking for EFB copy snapshots
        self.truncated_at = None
        self.truncated_op = None


class Decoder:
    def __init__(self, data, offset_pc, dl_span=None, manifest=None):
        self.data = data
        self.n = len(data)
        self.offset_pc = offset_pc  # fn(offset) -> pc string
        self.cp = CPState()
        # DL mode: resolve CALL_DL contents from a raw MEM1 span + manifest.
        self.dl_span = dl_span            # bytes, or None
        self.manifest = manifest or {}    # phys addr -> (file_off, size)
        self.dl_mode = dl_span is not None
        self.dl_calls = []    # per-CALL_DL record (dict), in stream order
        self.dl_missing = []  # (addr, size) CALL_DLs not resolvable via manifest
        self._depth = 0       # nesting guard for DL-within-DL
        # active aggregation sink; the stream uses stream_agg, DLs use their own.
        self.stream_agg = Agg()
        self.cur = self.stream_agg
        # backward-compat aliases so main()/summary keep reading stream results.
        self.commands = self.stream_agg.commands
        self.bp_writes = self.stream_agg.bp_writes
        self.cp_writes = self.stream_agg.cp_writes
        self.xf_writes = self.stream_agg.xf_writes
        self.draws = self.stream_agg.draws
        self.calldls = self.stream_agg.calldls
        self.efb_copies = self.stream_agg.efb_copies
        self.setdrawdone = self.stream_agg.setdrawdone
        self.pe_token = self.stream_agg.pe_token
        self.pe_token_int = self.stream_agg.pe_token_int
        self.truncated_at = None
        self.truncated_op = None

    def pc_at(self, off):
        return self.offset_pc(off)

    def emit(self, off, loc, name, detail, size):
        self.cur.commands.append((off, loc, name, detail, size))

    def run(self):
        end = self._walk(self.data, self.offset_pc)
        self.truncated_at = self.stream_agg.truncated_at
        self.truncated_op = self.stream_agg.truncated_op
        return end

    def _walk(self, data, loc_fn):
        """Walk one contiguous GX byte range, dispatching each command through
        decode_one into self.cur. Returns the offset consumed."""
        off = 0
        n = len(data)
        while off < n:
            need, consumed = self.decode_one(data, loc_fn, off)
            if need is None:
                # truncated: not enough bytes for this command's payload
                self.cur.truncated_at = off
                self.cur.truncated_op = data[off]
                return off
            off += consumed
        return off

    def decode_one(self, data, loc_fn, off):
        """Returns (need, consumed). need=None => truncated. Else consumed>0.
        Writes decoded results into self.cur (the active Agg)."""
        n = len(data)
        op = data[off]

        def loc():
            return loc_fn(off)

        if op == GX_NOP:
            # merge run of NOPs like Dolphin's OnNop
            count = 1
            while off + count < n and data[off + count] == GX_NOP:
                count += 1
            self.cur.nop_count += count
            self.emit(off, loc(), "NOP", "x%d" % count, count)
            return count, count

        if op == GX_LOAD_CP_REG:
            if off + 6 > n:
                return None, 0
            cmd2 = data[off + 1]
            value = swap32(data, off + 2)
            self.cp.load(cmd2, value)
            self.cur.cp_writes.setdefault(cmd2, []).append(value)
            self.emit(off, loc(), "LOAD_CP_REG",
                      "%s = 0x%08X" % (cp_name(cmd2), value), 6)
            return 6, 6

        if op == GX_LOAD_XF_REG:
            if off + 5 > n:
                return None, 0
            cmd2 = swap32(data, off + 1)
            base_addr = cmd2 & 0xFFFF
            stream_size = ((cmd2 >> 16) & 0xF) + 1
            if (cmd2 >> 16) >= 16:
                raise DecodeError(
                    "XF stream_size field >=16 at offset %d (cmd2=0x%08X) loc=%s"
                    % (off, cmd2, loc()))
            total = 5 + stream_size * 4
            if off + total > n:
                return None, 0
            vals = [swap32(data, off + 5 + 4 * i) for i in range(stream_size)]
            self.cur.xf_writes.setdefault(base_addr, []).append(tuple(vals))
            detail = self._xf_detail(base_addr, stream_size, vals)
            self.emit(off, loc(), "LOAD_XF_REG", detail, total)
            return total, total

        if op in INDX_NAMES:
            if off + 5 > n:
                return None, 0
            value = swap32(data, off + 1)
            index = value >> 16
            address = value & 0xFFF
            size = ((value >> 12) & 0xF) + 1
            self.emit(off, loc(), "LOAD_INDX",
                      "%s idx=%d xfaddr=0x%03X size=%d"
                      % (INDX_NAMES[op], index, address, size), 5)
            return 5, 5

        if op == GX_CMD_CALL_DL:
            if off + 9 > n:
                return None, 0
            address = swap32(data, off + 1) & ~31
            size = swap32(data, off + 5) & ~31
            self.cur.calldls.append((off, address, size))
            note = ("contents in guest RAM, NOT in stream"
                    if not self.dl_mode else "resolved from span")
            self.emit(off, loc(), "CALL_DL",
                      "addr=0x%08X size=0x%X (%s)" % (address, size, note), 9)
            if self.dl_mode:
                self._decode_dl(address, size, off)
            return 9, 9

        if op == GX_CMD_UNKNOWN_METRICS:
            self.emit(off, loc(), "UNKNOWN_METRICS", "(1 byte)", 1)
            return 1, 1

        if op == GX_CMD_INVL_VC:
            self.emit(off, loc(), "INVL_VC", "(1 byte, vertex cache invalidate)", 1)
            return 1, 1

        if op == GX_LOAD_BP_REG:
            if off + 5 > n:
                return None, 0
            addr = data[off + 1]
            value = swap24(data, off + 2)
            self._on_bp(off, loc(), addr, value)
            self.emit(off, loc(), "LOAD_BP_REG",
                      "%s = 0x%06X" % (bp_name(addr), value), 5)
            return 5, 5

        if GX_PRIMITIVE_START <= op <= GX_PRIMITIVE_END:
            if off + 3 > n:
                return None, 0
            prim = (op & GX_PRIMITIVE_MASK) >> GX_PRIMITIVE_SHIFT
            vat = op & GX_VAT_MASK
            vsize = self.cp.vertex_size(vat)
            nverts = swap16(data, off + 1)
            total = 3 + nverts * vsize
            if off + total > n:
                return None, 0
            self.cur.draws.append((prim, vat, nverts, vsize))
            self.cur.draw_full.append((prim, vat, nverts, vsize, self.cp.vcd_lo,
                                       self.cp.vcd_hi, self.cp.vat_g0[vat],
                                       self.cp.vat_g1[vat], self.cp.vat_g2[vat]))
            self.emit(off, loc(), "DRAW",
                      "%s vat=%d nverts=%d vtxsize=%d payload=%d"
                      % (PRIMITIVE_NAMES[prim], vat, nverts, vsize,
                         nverts * vsize), total)
            return total, total

        # Unknown opcode — hard error, never skip.
        ctx = data[max(0, off - 4):off + 8].hex()
        raise DecodeError(
            "UNKNOWN opcode 0x%02X at offset %d (loc=%s) context=[..%s..]"
            % (op, off, loc(), ctx))

    # ---- DL resolution: decode a CALL_DL target from the MEM1 span ----
    def _decode_dl(self, address, size, call_off):
        """Decode a display list's bytes with the CURRENT (call-site) CP state.
        Records a per-call result; CP writes inside the DL persist (inline
        execution semantics), matching hardware."""
        phys = address & 0x03FFFFFF
        ent = self.manifest.get(phys) or self.manifest.get(address)
        if ent is None:
            self.dl_missing.append((address, size))
            return
        file_off, msize = ent
        dl_bytes = self.dl_span[file_off:file_off + msize]
        entry_cp = self.cp.copy()
        agg = Agg()
        prev = self.cur
        self.cur = agg
        self._depth += 1
        loc_fn = (lambda base: (lambda o: "DL_0x%08X+0x%X" % (base, o)))(address)
        try:
            self._walk(dl_bytes, loc_fn)
        finally:
            self._depth -= 1
            self.cur = prev
        self.dl_calls.append({
            "addr": address, "phys": phys, "size": size, "msize": msize,
            "call_off": call_off, "entry_cp": entry_cp, "agg": agg,
        })

    # ---- BP EFB-copy cluster tracking ----
    def _on_bp(self, off, loc, addr, value):
        self.cur.bp_writes.setdefault(addr, []).append(value)
        self.cur._bp_latch[addr] = value
        if addr == 0x45:  # SETDRAWDONE
            self.cur.setdrawdone.append((off, value))
        elif addr == 0x47:  # PE_TOKEN_ID
            self.cur.pe_token.append((off, value))
        elif addr == 0x48:  # PE_TOKEN_INT_ID
            self.cur.pe_token_int.append((off, value))
        elif addr == 0x52:  # TRIGGER_EFB_COPY
            self.cur.efb_copies.append((off, loc, dict(self.cur._bp_latch)))

    # ---- XF detail decoding ----
    def _xf_detail(self, base, count, vals):
        name = xf_name(base)
        head = "%s (base=0x%04X count=%d)" % (name, base, count)
        # matrices / viewport / projection: show floats too
        if base < 0x1000 or base in (0x101a,) or base == 0x1020 \
                or 0x101a <= base <= 0x1026:
            floats = []
            for v in vals:
                floats.append("%g" % struct.unpack(">f", struct.pack(">I", v))[0])
            return head + " = [" + ", ".join(
                "0x%08X(%s)" % (v, fl) for v, fl in zip(vals, floats)) + "]"
        return head + " = [" + ", ".join("0x%08X" % v for v in vals) + "]"


# ---------------------------------------------------------------------------
# XF field decoders used only for the human-readable inventory
# ---------------------------------------------------------------------------
def decode_vcd(low, high):
    def vc(x):
        return ("NotPresent", "Direct", "Index8", "Index16")[x]
    parts = []
    parts.append("PosMatIdx=%d" % (low & 1))
    tmi = (low >> 1) & 0xFF
    parts.append("TexMatIdx=0x%02X" % tmi)
    parts.append("Pos=%s" % vc((low >> 9) & 3))
    parts.append("Nrm=%s" % vc((low >> 11) & 3))
    parts.append("Col0=%s" % vc((low >> 13) & 3))
    parts.append("Col1=%s" % vc((low >> 15) & 3))
    tcs = [vc((high >> (2 * i)) & 3) for i in range(8)]
    parts.append("Tex=[" + ",".join(tcs) + "]")
    return "; ".join(parts)


def decode_vat(g0, g1, g2):
    cf = ("UByte", "Byte", "UShort", "Short", "Float", "inv5", "inv6", "inv7")
    colf = ("RGB565", "RGB888", "RGB888x", "RGBA4444", "RGBA6666", "RGBA8888",
            "inv6", "inv7")
    parts = []
    parts.append("PosElem=%s" % ("XYZ" if (g0 & 1) else "XY"))
    parts.append("PosFmt=%s" % cf[(g0 >> 1) & 7])
    parts.append("PosShift=%d" % ((g0 >> 4) & 0x1F))
    parts.append("NrmElem=%s" % ("NTB" if ((g0 >> 9) & 1) else "N"))
    parts.append("NrmFmt=%s" % cf[(g0 >> 10) & 7])
    parts.append("Col0Elem=%s" % ("RGBA" if ((g0 >> 13) & 1) else "RGB"))
    parts.append("Col0Fmt=%s" % colf[(g0 >> 14) & 7])
    parts.append("Col1Elem=%s" % ("RGBA" if ((g0 >> 17) & 1) else "RGB"))
    parts.append("Col1Fmt=%s" % colf[(g0 >> 18) & 7])
    parts.append("Tex0Elem=%s" % ("ST" if ((g0 >> 21) & 1) else "S"))
    parts.append("Tex0Fmt=%s" % cf[(g0 >> 22) & 7])
    parts.append("ByteDequant=%d" % ((g0 >> 30) & 1))
    parts.append("NormalIndex3=%d" % ((g0 >> 31) & 1))
    parts.append("VCacheEnhance=%d" % ((g1 >> 31) & 1))
    return "; ".join(parts)


def decode_efb_copy(latch):
    """Return dict of human-readable EFB-copy fields from the BP latch snapshot."""
    out = {}
    xy = latch.get(0x49)
    wh = latch.get(0x4A)
    dest = latch.get(0x4B)
    stride = latch.get(0x4D)
    yscale = latch.get(0x4E)
    ar = latch.get(0x4F)
    gb = latch.get(0x50)
    z = latch.get(0x51)
    trig = latch.get(0x52)
    if xy is not None:
        out["src_x"] = xy & 0x3FF
        out["src_y"] = (xy >> 10) & 0x3FF
    if wh is not None:
        out["width"] = (wh & 0x3FF) + 1
        out["height"] = ((wh >> 10) & 0x3FF) + 1
    if dest is not None:
        out["dest_addr"] = dest << 5
    if stride is not None:
        out["dest_stride"] = stride << 5
    if yscale is not None:
        out["yscale_reg"] = yscale
    if ar is not None:
        out["clear_A"] = (ar >> 8) & 0xFF
        out["clear_R"] = ar & 0xFF
    if gb is not None:
        out["clear_G"] = (gb >> 8) & 0xFF
        out["clear_B"] = gb & 0xFF
    if z is not None:
        out["clear_Z"] = z
    if trig is not None:
        out["trigger"] = trig
        out["clear"] = bool((trig >> 11) & 1)
        out["copy_to_xfb"] = bool((trig >> 14) & 1)
        out["intensity_fmt"] = bool((trig >> 15) & 1)
        out["auto_conv"] = bool((trig >> 16) & 1)
        out["half_scale"] = bool((trig >> 9) & 1)
        out["scale_invert"] = bool((trig >> 10) & 1)
        out["target_pixel_format"] = (trig >> 3) & 0xF
    return out


# ---------------------------------------------------------------------------
def build_stream(js):
    entries = js["entries"]
    entries = sorted(entries, key=lambda e: e["seq"])
    chunks = []
    burst_pc = []  # (start_offset, pc, seq)
    off = 0
    for e in entries:
        b = bytes.fromhex(e["data"])
        if len(b) != 32:
            raise DecodeError("entry seq=%s data is %d bytes, expected 32"
                              % (e.get("seq"), len(b)))
        burst_pc.append((off, e["pc"], e["seq"], e.get("wptr")))
        chunks.append(b)
        off += 32
    stream = b"".join(chunks)

    def offset_pc(o):
        idx = o // 32
        if idx >= len(burst_pc):
            idx = len(burst_pc) - 1
        return "0x%08X" % burst_pc[idx][1]

    return stream, offset_pc, burst_pc


def load_manifest(path):
    """Return (dl_span_path_hint, {phys_addr: (file_off, size)}, base)."""
    mj = json.load(open(path))
    base = mj.get("base")
    table = {}
    for d in mj["dls"]:
        phys = d["addr"] & 0x03FFFFFF
        table[phys] = (d["off"], d["size"])
        table[d["addr"]] = (d["off"], d["size"])
    return table, base


def main():
    argv = sys.argv[1:]
    summary = "--summary" in argv
    dl_span = None
    manifest = None
    dl_only = False
    span_path = manifest_path = None
    if "--dl" in argv:
        i = argv.index("--dl")
        span_path = argv[i + 1]
        manifest_path = argv[i + 2]
        dl_span = open(span_path, "rb").read()
        manifest, _base = load_manifest(manifest_path)
        # consume the two positionals so they aren't taken as the fifo json
        del argv[i:i + 3]
    dl_only = "--dl-only" in argv
    args = [a for a in argv if not a.startswith("--")]
    if not args:
        print("usage: gx_fifo_decode.py <fifo_dump.json> [--summary]")
        print("       gx_fifo_decode.py <fifo_dump.json> --dl <span.bin> "
              "<manifest.json> [--summary] [--dl-only]")
        return 2
    js = json.load(open(args[0]))
    stream, offset_pc, burst_pc = build_stream(js)
    dec = Decoder(stream, offset_pc, dl_span=dl_span, manifest=manifest)
    try:
        end = dec.run()
    except DecodeError as e:
        # print what we have, then the error, then fail loudly.
        for off, pc, name, detail, size in dec.commands:
            print("%6d  pc=%s  %-14s %s" % (off, pc, name, detail))
        print("\nDECODE ERROR: %s" % e, file=sys.stderr)
        return 1

    if not summary and not dl_only:
        for off, pc, name, detail, size in dec.commands:
            print("%6d  pc=%s  %-14s %s" % (off, pc, name, detail))

    print("\n==== SUMMARY ====")
    print("stream bytes: %d (%d bursts x 32)" % (len(stream), len(burst_pc)))
    print("commands decoded: %d" % len(dec.commands))
    if dec.truncated_at is not None:
        print("TRUNCATED tail: opcode 0x%02X at offset %d needs more bytes than "
              "captured (%d bytes remain)"
              % (dec.truncated_op, dec.truncated_at, len(stream) - dec.truncated_at))
    else:
        print("stream consumed end-to-end, no truncation")

    print("\n-- BP registers written (addr name: values) --")
    for addr in sorted(dec.bp_writes):
        vals = dec.bp_writes[addr]
        uniq = sorted(set(vals))
        show = ", ".join("0x%06X" % v for v in uniq[:8])
        if len(uniq) > 8:
            show += ", ... (%d unique)" % len(uniq)
        print("  0x%02X %-18s [n=%d] %s" % (addr, bp_name(addr), len(vals), show))

    print("\n-- CP registers written --")
    for cmd in sorted(dec.cp_writes):
        vals = dec.cp_writes[cmd]
        uniq = sorted(set(vals))
        show = ", ".join("0x%08X" % v for v in uniq[:6])
        print("  0x%02X %-16s [n=%d] %s" % (cmd, cp_name(cmd), len(vals), show))

    print("\n-- XF registers written --")
    for addr in sorted(dec.xf_writes):
        vals = dec.xf_writes[addr]
        print("  0x%04X %-22s [n=%d]" % (addr, xf_name(addr), len(vals)))

    print("\n-- Draws --")
    print("  total draw commands: %d" % len(dec.draws))
    for d in dec.draws[:20]:
        print("    %s vat=%d nverts=%d vtxsize=%d" % (PRIMITIVE_NAMES[d[0]], d[1], d[2], d[3]))

    print("\n-- CALL_DL --")
    print("  total: %d" % len(dec.calldls))
    for off, addr, size in dec.calldls[:20]:
        print("    offset=%d addr=0x%08X size=0x%X" % (off, addr, size))

    print("\n-- Sync / waits --")
    print("  SETDRAWDONE (BP 0x45): n=%d %s"
          % (len(dec.setdrawdone), [(o, "0x%X" % v) for o, v in dec.setdrawdone]))
    print("  PE_TOKEN_ID (BP 0x47): n=%d %s"
          % (len(dec.pe_token), [(o, "0x%X" % v) for o, v in dec.pe_token]))
    print("  PE_TOKEN_INT_ID (BP 0x48): n=%d %s"
          % (len(dec.pe_token_int), [(o, "0x%X" % v) for o, v in dec.pe_token_int]))

    print("\n-- EFB copies (BP 0x52 triggers) --")
    print("  total: %d" % len(dec.efb_copies))
    for off, pc, latch in dec.efb_copies:
        f = decode_efb_copy(latch)
        print("  offset=%d pc=%s" % (off, pc))
        print("    " + ", ".join("%s=%s" % (k, ("0x%X" % v if isinstance(v, int)
                                                 and k in ("dest_addr", "dest_stride",
                                                           "clear_Z", "trigger")
                                                 else v))
                                  for k, v in f.items()))

    if dec.dl_mode:
        report_dls(dec)
    return 0


CPARRAY_NAMES = {
    0: "Position", 1: "Normal", 2: "Color0", 3: "Color1",
    4: "TexCoord0", 5: "TexCoord1", 6: "TexCoord2", 7: "TexCoord3",
    8: "TexCoord4", 9: "TexCoord5", 10: "TexCoord6", 11: "TexCoord7",
    12: "XF_A(posmtx)", 13: "XF_B(nrmmtx)", 14: "XF_C(texmtx)", 15: "XF_D(light)",
}


def tris_of(prim, nverts):
    """Triangle count contributed by a primitive with nverts vertices."""
    if prim in (0x2,):            # TRIANGLES
        return nverts // 3
    if prim in (0x3, 0x4):        # TRIANGLE_STRIP / FAN
        return max(0, nverts - 2)
    if prim in (0x0, 0x1):        # QUADS / QUADS_2  -> 2 tris per 4 verts
        return (nverts // 4) * 2
    return 0                       # lines / points contribute no triangles


def indexed_arrays(vcd_lo, vcd_hi):
    """Return list of (attr, array_index) for attributes that are indexed
    (Index8/Index16) in this VCD, per Dolphin CPArray numbering
    (Position=0, Normal=1, Color0=2, Color1=3, TexCoord0..7=4..11)."""
    out = []
    if ((vcd_lo >> 9) & 3) >= 2:
        out.append(("Position", 0))
    if ((vcd_lo >> 11) & 3) >= 2:
        out.append(("Normal", 1))
    if ((vcd_lo >> 13) & 3) >= 2:
        out.append(("Color0", 2))
    if ((vcd_lo >> 15) & 3) >= 2:
        out.append(("Color1", 3))
    for i in range(8):
        if ((vcd_hi >> (2 * i)) & 3) >= 2:
            out.append(("TexCoord%d" % i, 4 + i))
    return out


def report_dls(dec):
    from collections import Counter, OrderedDict
    print("\n\n==== DISPLAY-LIST DRAW INVENTORY ====")
    print("CALL_DL sites walked: %d   resolved: %d   unresolved: %d"
          % (len(dec.calldls), len(dec.dl_calls), len(dec.dl_missing)))
    if dec.dl_missing:
        print("  !! UNRESOLVED CALL_DLs (not in manifest):")
        for addr, size in dec.dl_missing:
            print("     addr=0x%08X size=0x%X" % (addr, size))

    # group per-call records by physical DL address
    by_addr = OrderedDict()
    for rec in dec.dl_calls:
        by_addr.setdefault(rec["phys"], []).append(rec)

    anomalies = []
    total_verts = 0
    total_tris = 0
    union_prims = Counter()          # prim_name -> count of draw cmds
    union_vat = set()
    union_vcount = Counter()
    union_fmts = OrderedDict()       # (vcd_lo,vcd_hi,g0,g1,g2) -> count
    union_indexed = set()            # (attr, array_idx)
    union_dl_bp = {}                 # bp addr -> set(values) seen inside DLs
    union_dl_xf = {}                 # xf addr -> count seen inside DLs
    frame_verts = 0                  # one logical frame = each unique DL once
    frame_tris = 0
    seen_addr = set()

    print("\n-- Per unique display list --")
    for phys, recs in by_addr.items():
        addr = recs[0]["addr"]
        msize = recs[0]["msize"]
        agg = recs[0]["agg"]
        # command breakdown
        opc = Counter(c[2] for c in agg.commands)
        opc_str = ", ".join("%s×%d" % (k, v) for k, v in sorted(opc.items()))
        # consistency across the (usually 2) calls to this DL
        sigs = set()
        for r in recs:
            sig = tuple((c[2], c[4]) for c in r["agg"].commands)
            sigs.add(sig)
        consistent = len(sigs) == 1
        if not consistent:
            anomalies.append("DL 0x%08X: %d calls decode to DIFFERENT command "
                             "signatures (call-site CP state differs)"
                             % (addr, len(recs)))
        # truncation / trailing bytes
        consumed = sum(c[4] for c in agg.commands)
        if agg.truncated_at is not None:
            anomalies.append("DL 0x%08X: TRUNCATED at off %d (opcode 0x%02X)"
                             % (addr, agg.truncated_at, agg.truncated_op))
        elif consumed != msize:
            anomalies.append("DL 0x%08X: consumed %d of %d bytes (%d trailing)"
                             % (addr, consumed, msize, msize - consumed))

        dl_verts = sum(d[2] for d in agg.draw_full)
        dl_tris = sum(tris_of(d[0], d[2]) for d in agg.draw_full)
        print("  DL 0x%08X  size=0x%X  calls=%d  consumed=%d  %s"
              % (addr, msize, len(recs), consumed,
                 "OK" if consistent else "**INCONSISTENT**"))
        print("     ops: %s" % opc_str)
        if agg.draw_full:
            for d in agg.draw_full:
                print("     DRAW %s vat=%d nverts=%d vtxsize=%d tris=%d"
                      % (PRIMITIVE_NAMES[d[0]], d[1], d[2], d[3],
                         tris_of(d[0], d[2])))
        if agg.bp_writes:
            print("     BP inside: " + ", ".join(
                "%s(0x%02X)" % (bp_name(a), a) for a in sorted(agg.bp_writes)))
        if agg.xf_writes:
            print("     XF inside: " + ", ".join(
                "%s(0x%04X)" % (xf_name(a), a) for a in sorted(agg.xf_writes)))
        if agg.cp_writes:
            print("     CP inside: " + ", ".join(
                "%s(0x%02X)" % (cp_name(a), a) for a in sorted(agg.cp_writes)))

        # accumulate union (across ALL invocations)
        for r in recs:
            a = r["agg"]
            for d in a.draw_full:
                total_verts += d[2]
                total_tris += tris_of(d[0], d[2])
                union_prims[PRIMITIVE_NAMES[d[0]]] += 1
                union_vat.add(d[1])
                union_vcount[d[2]] += 1
                key = (d[4], d[5], d[6], d[7], d[8])
                union_fmts[key] = union_fmts.get(key, 0) + 1
                for pair in indexed_arrays(d[4], d[5]):
                    union_indexed.add(pair)
            for aaddr, vals in a.bp_writes.items():
                union_dl_bp.setdefault(aaddr, set()).update(vals)
            for xaddr, vals in a.xf_writes.items():
                union_dl_xf[xaddr] = union_dl_xf.get(xaddr, 0) + len(vals)
        # one-frame totals: count each unique DL once
        if phys not in seen_addr:
            seen_addr.add(phys)
            frame_verts += dl_verts
            frame_tris += dl_tris

    print("\n-- UNION across all display lists --")
    print("  draw primitives used: " + ", ".join(
        "%s×%d" % (k, v) for k, v in sorted(union_prims.items())))
    print("  VAT indices used: %s" % sorted(union_vat))
    print("  vertex counts (nverts×occurrences): "
          + ", ".join("%d×%d" % (k, v) for k, v in sorted(union_vcount.items())))
    print("\n  vertex formats actually used at DRAW time (VCD/VAT decoded):")
    for i, (key, cnt) in enumerate(union_fmts.items()):
        vcd_lo, vcd_hi, g0, g1, g2 = key
        print("   [fmt %d]  used by %d draws" % (i, cnt))
        print("     VCD_LO=0x%08X VCD_HI=0x%08X" % (vcd_lo, vcd_hi))
        print("       %s" % decode_vcd(vcd_lo, vcd_hi))
        print("     VAT g0=0x%08X g1=0x%08X g2=0x%08X" % (g0, g1, g2))
        print("       %s" % decode_vat(g0, g1, g2))
    print("\n  indexed attributes -> vertex arrays (ARRAY_BASE index) referenced:")
    if union_indexed:
        for attr, idx in sorted(union_indexed, key=lambda x: x[1]):
            print("     %-9s -> ARRAY_BASE[%d] (%s)"
                  % (attr, idx, CPARRAY_NAMES[idx]))
    else:
        print("     (none — all attributes Direct/inline)")
    print("\n  BP registers written INSIDE display lists:")
    if union_dl_bp:
        for a in sorted(union_dl_bp):
            vals = sorted(union_dl_bp[a])
            show = ", ".join("0x%06X" % v for v in vals[:6])
            print("     0x%02X %-18s %s" % (a, bp_name(a), show))
    else:
        print("     (none)")
    print("\n  XF registers written INSIDE display lists:")
    if union_dl_xf:
        for a in sorted(union_dl_xf):
            print("     0x%04X %-20s [n=%d]" % (a, xf_name(a), union_dl_xf[a]))
    else:
        print("     (none)")

    print("\n-- Geometry totals --")
    print("  ALL %d invocations: vertices=%d  triangles=%d"
          % (len(dec.dl_calls), total_verts, total_tris))
    print("  one frame (each of %d unique DLs once): vertices=%d  triangles=%d"
          % (len(by_addr), frame_verts, frame_tris))

    print("\n-- DL decode sanity --")
    if anomalies:
        print("  !! ANOMALIES:")
        for a in anomalies:
            print("     " + a)
    else:
        print("  all display lists decoded end-to-end, no unknown opcodes, "
              "no truncation, consistent across calls")


if __name__ == "__main__":
    sys.exit(main())
