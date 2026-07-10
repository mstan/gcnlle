#!/usr/bin/env python3
"""
oracle/diff.py — value+order differential diff for gcnrecomp trace streams.

Compares two binary trace files written in the oracle/trace_format.h layout
(one from the LLE runtime under test, one from the Dolphin trace-tap) and
reports the FIRST divergence with a context window and a full register
snapshot at the divergence point.

Discipline (docs/DESIGN.md, PRINCIPLES.md "Using the Oracle When Boot
Diverges"): we diff by VALUE + ORDER, never by frame alignment. We do NOT
assume the two streams have equal length or are frame-aligned. The walk is
positional in retire/event order (the per-record `seq`), and the earliest
index at which the streams disagree is THE divergence.

Tiered strategy
---------------
  Tier 1  event ledger  : a canonical projection of each record (type + the
                          salient identity fields — PC, addr, value, event,
                          EXI/GX/DSP specifics). Catches structural drift:
                          a different PC, a missing/extra MMIO or EXI event,
                          an interrupt taken on one side and not the other.
  Tier 2  full register : for retired-instruction records whose Tier-1 ledger
                          still matches, compare the full architectural state
                          (gpr[0..31], LR/CTR/CR/XER/MSR, optional FP hash).
                          Catches a wrong *value* flowing under an identical
                          control-flow shape.

We report whichever tier fires first, at the earliest seq index.

Usage
-----
  diff.py RUNTIME.trace DOLPHIN.trace [--context N] [--max-report M]
          [--no-registers] [--summary]

Exit status: 0 if the streams match to the length of the shorter, 1 if a
divergence (or a length mismatch) is found, 2 on a file/format error.
"""
import argparse
import struct
import sys

MAGIC = 0x544E4347  # 'G','C','N','T' little-endian
RECORD_SIZE = 204
FHDR_SIZE = 32

# record type tags (mirror enum gcn_trace_rec_type)
TR_RETIRED, TR_MEM, TR_MMIO, TR_DMA, TR_INTR, TR_EXI, TR_GX, TR_VI, TR_DSP = range(1, 10)
TYPE_NAME = {
    TR_RETIRED: "RETIRED", TR_MEM: "MEM_WRITE", TR_MMIO: "MMIO", TR_DMA: "DMA",
    TR_INTR: "INTERRUPT", TR_EXI: "EXI", TR_GX: "GX", TR_VI: "VI", TR_DSP: "DSP_AI",
}
MMIO_BLOCK = {0: "CP", 1: "PE", 2: "VI", 3: "PI", 4: "MI", 5: "DSP",
              6: "DI", 7: "SI", 8: "EXI", 9: "AI", 255: "OTHER"}

# struct formats (little-endian, packed) — must match trace_format.h exactly.
FHDR_FMT = "<IHHIIII 8x"           # magic,ver,hdr_size,rec_size,flags,producer,ipl_hash, reserved[2]
HDR_FMT = "<HHIQQ"                  # type,size,cpu,seq,tb                            (24)
RETIRED_FMT = "<9I32IQII"          # insn,pc,npc,lr,ctr,cr,xer,msr,fpscr, gpr[32], ps_hash, flags, pad
MEM_FMT = "<IIIIB3x"               # pc,addr,value,value_hi,size
MMIO_FMT = "<IIIBBBx"              # pc,addr,value,size,is_write,block
DMA_FMT = "<IIIIBB2x"              # pc,src,dst,len,device,dir
INTR_FMT = "<IIIIB3x"              # taken_pc,cause,pending,intmask,source
EXI_FMT = "<I8B II16s16s"          # pc, 8 u8, cmd_crc,resp_crc, cmd[16], resp[16]
GX_FMT = "<IIIIHHI"                # pc,fifo_addr,fifo_len,fifo_crc,packet,event,token
VI_FMT = "<IIHHI"                  # pc,xfb_addr,event,field,line
DSP_FMT = "<IIIIIIIHHI"            # pc,mail_hi,mail_lo,control,aram_addr,aram_aram,aram_len,subsys,event,ai_rate

GPR_NAMES = [f"r{i}" for i in range(32)]


class Record:
    __slots__ = ("type", "cpu", "seq", "tb", "f")

    def __init__(self, type_, cpu, seq, tb, fields):
        self.type = type_
        self.cpu = cpu
        self.seq = seq
        self.tb = tb
        self.f = fields  # dict of decoded payload fields

    # -- Tier 1: canonical event-ledger key --------------------------------
    def ledger(self):
        f = self.f
        t = self.type
        if t == TR_RETIRED:
            # control-flow identity only; register values are Tier 2.
            return (t, f["pc"], f["npc"], f["insn"])
        if t == TR_MEM:
            return (t, f["pc"], f["addr"], f["size"], f["value"], f["value_hi"])
        if t == TR_MMIO:
            return (t, f["addr"], f["size"], f["is_write"], f["value"])
        if t == TR_DMA:
            return (t, f["device"], f["dir"], f["src"], f["dst"], f["len"])
        if t == TR_INTR:
            return (t, f["source"], f["taken_pc"], f["pending"], f["intmask"])
        if t == TR_EXI:
            return (t, f["channel"], f["device"], f["select"], f["cmd_len"],
                    f["resp_len"], f["cmd_crc"], f["resp_crc"])
        if t == TR_GX:
            return (t, f["fifo_addr"], f["fifo_len"], f["fifo_crc"], f["packet"],
                    f["event"], f["token"])
        if t == TR_VI:
            return (t, f["xfb_addr"], f["event"], f["field"])
        if t == TR_DSP:
            return (t, f["subsys"], f["event"], f["mail_hi"], f["mail_lo"],
                    f["control"], f["aram_addr"], f["aram_aram"], f["aram_len"])
        return (t,)

    # -- Tier 2: full architectural state (retired only) -------------------
    def regstate(self):
        assert self.type == TR_RETIRED
        f = self.f
        st = {"pc": f["pc"], "npc": f["npc"], "lr": f["lr"], "ctr": f["ctr"],
              "cr": f["cr"], "xer": f["xer"], "msr": f["msr"]}
        for i in range(32):
            st[GPR_NAMES[i]] = f["gpr"][i]
        if f["flags"] & 0x1:  # GCN_TR_F_FP_VALID
            st["fpscr"] = f["fpscr"]
            st["ps_hash"] = f["ps_hash"]
        return st

    def one_line(self):
        f = self.f
        t = self.type
        n = TYPE_NAME.get(t, f"T{t}")
        if t == TR_RETIRED:
            return f"{n:9} pc={f['pc']:08X} npc={f['npc']:08X} insn={f['insn']:08X}"
        if t == TR_MEM:
            return f"{n:9} pc={f['pc']:08X} [{f['addr']:08X}] <= {f['value']:08X} (sz{f['size']})"
        if t == TR_MMIO:
            rw = "W" if f["is_write"] else "R"
            blk = MMIO_BLOCK.get(f["block"], "?")
            return f"{n:9} pc={f['pc']:08X} {blk}:{f['addr']:08X} {rw} {f['value']:08X} (sz{f['size']})"
        if t == TR_DMA:
            return f"{n:9} pc={f['pc']:08X} dev={f['device']} {f['src']:08X}->{f['dst']:08X} len={f['len']}"
        if t == TR_INTR:
            return f"{n:9} src={f['source']} pc={f['taken_pc']:08X} pend={f['pending']:08X} mask={f['intmask']:08X}"
        if t == TR_EXI:
            return (f"{n:9} pc={f['pc']:08X} ch{f['channel']} dev{f['device']} sel{f['select']} "
                    f"cmd{f['cmd_len']}:{f['cmd_crc']:08X} resp{f['resp_len']}:{f['resp_crc']:08X}")
        if t == TR_GX:
            return f"{n:9} pc={f['pc']:08X} fifo={f['fifo_addr']:08X}+{f['fifo_len']} crc={f['fifo_crc']:08X} pkt={f['packet']} ev={f['event']}"
        if t == TR_VI:
            return f"{n:9} pc={f['pc']:08X} xfb={f['xfb_addr']:08X} ev={f['event']} field={f['field']}"
        if t == TR_DSP:
            return f"{n:9} pc={f['pc']:08X} sub={f['subsys']} ev={f['event']} mail={f['mail_hi']:08X}:{f['mail_lo']:08X}"
        return n


def _decode_payload(type_, payload):
    f = {}
    if type_ == TR_RETIRED:
        v = struct.unpack_from(RETIRED_FMT, payload)
        (f["insn"], f["pc"], f["npc"], f["lr"], f["ctr"], f["cr"], f["xer"],
         f["msr"], f["fpscr"]) = v[0:9]
        f["gpr"] = list(v[9:41])
        f["ps_hash"], f["flags"], _pad = v[41], v[42], v[43]
    elif type_ == TR_MEM:
        f["pc"], f["addr"], f["value"], f["value_hi"], f["size"] = struct.unpack_from(MEM_FMT, payload)
    elif type_ == TR_MMIO:
        f["pc"], f["addr"], f["value"], f["size"], f["is_write"], f["block"] = struct.unpack_from(MMIO_FMT, payload)
    elif type_ == TR_DMA:
        f["pc"], f["src"], f["dst"], f["len"], f["device"], f["dir"] = struct.unpack_from(DMA_FMT, payload)
    elif type_ == TR_INTR:
        f["taken_pc"], f["cause"], f["pending"], f["intmask"], f["source"] = struct.unpack_from(INTR_FMT, payload)
    elif type_ == TR_EXI:
        v = struct.unpack_from(EXI_FMT, payload)
        f["pc"] = v[0]
        (f["channel"], f["device"], f["select"], f["cmd_len"], f["resp_len"],
         f["cmd_inline"], f["resp_inline"], _pad) = v[1:9]
        f["cmd_crc"], f["resp_crc"], f["cmd"], f["resp"] = v[9], v[10], v[11], v[12]
    elif type_ == TR_GX:
        (f["pc"], f["fifo_addr"], f["fifo_len"], f["fifo_crc"], f["packet"],
         f["event"], f["token"]) = struct.unpack_from(GX_FMT, payload)
    elif type_ == TR_VI:
        f["pc"], f["xfb_addr"], f["event"], f["field"], f["line"] = struct.unpack_from(VI_FMT, payload)
    elif type_ == TR_DSP:
        (f["pc"], f["mail_hi"], f["mail_lo"], f["control"], f["aram_addr"],
         f["aram_aram"], f["aram_len"], f["subsys"], f["event"], f["ai_rate"]) = struct.unpack_from(DSP_FMT, payload)
    else:
        raise ValueError(f"unknown record type {type_}")
    return f


def load_trace(path):
    with open(path, "rb") as fp:
        data = fp.read()
    if len(data) < FHDR_SIZE:
        raise ValueError(f"{path}: too small to hold a file header")
    magic, ver, hdr_size, rec_size, flags, producer, ipl_hash = struct.unpack_from(FHDR_FMT, data)
    if magic != MAGIC:
        raise ValueError(f"{path}: bad magic {magic:08X} (expected {MAGIC:08X})")
    if rec_size != RECORD_SIZE or hdr_size != FHDR_SIZE:
        raise ValueError(f"{path}: header/record size mismatch (hdr={hdr_size} rec={rec_size})")
    meta = {"version": ver, "producer": producer, "ipl_hash": ipl_hash}
    body = data[hdr_size:]
    if len(body) % RECORD_SIZE != 0:
        raise ValueError(f"{path}: body not a multiple of record size ({len(body)} bytes)")
    recs = []
    for off in range(0, len(body), RECORD_SIZE):
        chunk = body[off:off + RECORD_SIZE]
        type_, size, cpu, seq, tb = struct.unpack_from(HDR_FMT, chunk)
        if size != RECORD_SIZE:
            raise ValueError(f"{path}: record {len(recs)} declares size {size}")
        payload = chunk[24:]
        recs.append(Record(type_, cpu, seq, tb, _decode_payload(type_, payload)))
    return meta, recs


def diff_regstate(a, b):
    """Return list of (field, a_val, b_val) that differ between two retired snapshots."""
    sa, sb = a.regstate(), b.regstate()
    keys = list(sa.keys())
    for k in sb:
        if k not in sa:
            keys.append(k)
    out = []
    for k in keys:
        va, vb = sa.get(k), sb.get(k)
        if va != vb:
            out.append((k, va, vb))
    return out


def print_context(label, recs, idx, context):
    lo = max(0, idx - context)
    hi = min(len(recs), idx + context + 1)
    print(f"  {label} window [{lo}..{hi - 1}] of {len(recs)}:")
    for i in range(lo, hi):
        mark = ">>" if i == idx else "  "
        print(f"   {mark} [{i:>6}] seq={recs[i].seq:<8} {recs[i].one_line()}")
    if idx >= len(recs):
        print(f"   >> [{idx:>6}] <past end of stream>")


def main(argv=None):
    ap = argparse.ArgumentParser(description="value+order diff of two gcnrecomp trace streams")
    ap.add_argument("runtime", help="trace from the LLE runtime under test")
    ap.add_argument("oracle", help="trace from the Dolphin oracle tap")
    ap.add_argument("--context", type=int, default=4, help="records of context on each side (default 4)")
    ap.add_argument("--no-registers", action="store_true", help="skip Tier-2 full-register comparison")
    ap.add_argument("--summary", action="store_true", help="print stream summaries and exit")
    args = ap.parse_args(argv)

    try:
        meta_a, a = load_trace(args.runtime)
        meta_b, b = load_trace(args.oracle)
    except (OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    print(f"runtime : {args.runtime}  ({len(a)} records, producer={meta_a['producer']})")
    print(f"oracle  : {args.oracle}  ({len(b)} records, producer={meta_b['producer']})")

    if args.summary:
        for label, recs in (("runtime", a), ("oracle", b)):
            counts = {}
            for r in recs:
                counts[r.type] = counts.get(r.type, 0) + 1
            desc = ", ".join(f"{TYPE_NAME.get(t, t)}={n}" for t, n in sorted(counts.items()))
            print(f"  {label}: {desc}")
        return 0

    n = min(len(a), len(b))
    for i in range(n):
        ra, rb = a[i], b[i]
        # Tier 1: event-ledger match.
        if ra.ledger() != rb.ledger():
            print(f"\nDIVERGENCE at index {i} (Tier 1: event ledger)")
            print(f"  runtime seq={ra.seq}: {ra.one_line()}")
            print(f"  oracle  seq={rb.seq}: {rb.one_line()}")
            if ra.type != rb.type:
                print(f"  record TYPE differs: runtime={TYPE_NAME.get(ra.type, ra.type)} "
                      f"oracle={TYPE_NAME.get(rb.type, rb.type)}")
            print()
            print_context("runtime", a, i, args.context)
            print_context("oracle", b, i, args.context)
            if ra.type == TR_RETIRED and rb.type == TR_RETIRED:
                _print_regdiff(ra, rb)
            return 1
        # Tier 2: full-register comparison inside the (so-far matching) window.
        if not args.no_registers and ra.type == TR_RETIRED and rb.type == TR_RETIRED:
            rd = diff_regstate(ra, rb)
            if rd:
                print(f"\nDIVERGENCE at index {i} (Tier 2: register value)")
                print(f"  pc={ra.f['pc']:08X} insn={ra.f['insn']:08X}  "
                      f"(control flow matches; a register VALUE differs)")
                print()
                print_context("runtime", a, i, args.context)
                print_context("oracle", b, i, args.context)
                _print_regdiff(ra, rb)
                return 1

    # No divergence within the common prefix.
    if len(a) != len(b):
        longer, label, idx = (a, "runtime", len(b)) if len(a) > len(b) else (b, "oracle", len(a))
        print(f"\nDIVERGENCE at index {idx} (length: streams match for the first {n} "
              f"records, then '{label}' has {abs(len(a) - len(b))} extra)")
        print()
        print_context(label, longer, idx if idx < len(longer) else len(longer) - 1, args.context)
        return 1

    print(f"\nMATCH: {n} records identical by value + order (Tier 1 ledger + Tier 2 registers).")
    return 0


def _print_regdiff(ra, rb):
    rd = diff_regstate(ra, rb)
    if not rd:
        print("  (register state identical at this point)")
        return
    print("  register snapshot at divergence (only differing fields shown):")
    print(f"    {'field':>8}  {'runtime':>16}  {'oracle':>16}")
    for k, va, vb in rd:
        sa = f"{va:016X}" if isinstance(va, int) and va > 0xFFFFFFFF else (f"{va:08X}" if va is not None else "----")
        sb = f"{vb:016X}" if isinstance(vb, int) and vb > 0xFFFFFFFF else (f"{vb:08X}" if vb is not None else "----")
        print(f"    {k:>8}  {sa:>16}  {sb:>16}")


if __name__ == "__main__":
    sys.exit(main())
