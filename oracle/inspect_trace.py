#!/usr/bin/env python3
"""Quick inspector for a gcn trace_format.h stream (either producer).

Usage:  python inspect_trace.py <trace> [N_MMIO_EXI]

Prints a record histogram, the first N MMIO/EXI events in seq order, the first
`sc` (if any), and the first/last retired instruction. Tolerant of a truncated
final record (e.g. a hard-killed producer): only whole records are parsed.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from diff import (FHDR_FMT, FHDR_SIZE, HDR_FMT, MAGIC, RECORD_SIZE, TR_EXI,
                  TR_MMIO, TR_RETIRED, TYPE_NAME, Record, _decode_payload)


def load_whole(path):
    data = open(path, "rb").read()
    magic, ver, hs, rs, fl, prod, iplh = struct.unpack_from(FHDR_FMT, data)
    body = data[FHDR_SIZE:]
    n = len(body) // RECORD_SIZE
    recs = []
    for off in range(0, n * RECORD_SIZE, RECORD_SIZE):
        ch = body[off:off + RECORD_SIZE]
        t, size, cpu, seq, tb = struct.unpack_from(HDR_FMT, ch)
        recs.append(Record(t, cpu, seq, tb, _decode_payload(t, ch[24:])))
    return dict(magic_ok=(magic == MAGIC), producer=prod, ipl_hash=iplh,
                truncated=(len(body) % RECORD_SIZE != 0)), recs


def main():
    path = sys.argv[1]
    N = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    meta, recs = load_whole(path)
    print(f"file={path} size={os.path.getsize(path)} records={len(recs)} "
          f"producer={meta['producer']} ipl_hash={meta['ipl_hash']:08X} "
          f"magic_ok={meta['magic_ok']} truncated_tail={meta['truncated']}")
    hist = {}
    for r in recs:
        hist[r.type] = hist.get(r.type, 0) + 1
    print("histogram:", {TYPE_NAME.get(t, t): c for t, c in sorted(hist.items())})

    print(f"\nfirst {N} MMIO/EXI events (seq order):")
    c = 0
    for r in recs:
        if r.type in (TR_MMIO, TR_EXI):
            print(f"  seq={r.seq:<7d} {r.one_line()}")
            c += 1
            if c >= N:
                break

    sc = next((r for r in recs if r.type == TR_RETIRED and (r.f["insn"] >> 26) == 17), None)
    print(f"\nfirst sc: {('seq=%d pc=%08X' % (sc.seq, sc.f['pc'])) if sc else 'none in trace'}")
    rets = [r for r in recs if r.type == TR_RETIRED]
    if rets:
        print(f"first retired: seq={rets[0].seq} pc={rets[0].f['pc']:08X} insn={rets[0].f['insn']:08X}")
        print(f"last  retired: seq={rets[-1].seq} pc={rets[-1].f['pc']:08X}")


if __name__ == "__main__":
    main()
