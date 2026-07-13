#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
oracle/scan_mmio_block.py — streaming MMIO-block scan for a trace_format.h
capture too large for inspect_trace.py's load-the-whole-file approach (a
raw, uncollapsed multi-hour boot/menu capture can run into the GB range —
see collapse_trace.py's docstring). Reads one record at a time; never holds
the whole trace in memory.

Usage:  python oracle/scan_mmio_block.py <trace> [BLOCK] [N]
  BLOCK: block name (CP/PE/VI/PI/MI/DSP/DI/SI/EXI/AI/OTHER) or a numeric id.
         Default DI (this tool's original use case: M5b disc-interaction
         captures, DI = 0xCC006000..0xCC0063FF).
  N:     how many matching events to print (default 40).

Prints the full record histogram, the total match count for BLOCK, and the
first N matches in seq order.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from diff import (FHDR_FMT, FHDR_SIZE, HDR_FMT, MAGIC, MMIO_BLOCK, RECORD_SIZE,
                  TR_MMIO, TYPE_NAME, Record, _decode_payload)

NAME_TO_BLOCK = {v: k for k, v in MMIO_BLOCK.items()}
MMIO_PAYLOAD_OFF = 24  # pc,addr,value,size,is_write,block — diff.py MMIO_FMT


def scan(path, block_sel, n):
    with open(path, "rb") as f:
        fhdr = f.read(FHDR_SIZE)
        if len(fhdr) < FHDR_SIZE:
            print(f"{path}: too small to hold a file header", file=sys.stderr)
            return 2
        magic, ver, hs, rs, fl, prod, iplh = struct.unpack_from(FHDR_FMT, fhdr)
        if magic != MAGIC or rs != RECORD_SIZE or hs != FHDR_SIZE:
            print(f"{path}: bad header (magic={magic:08X} hdr={hs} rec={rs})",
                  file=sys.stderr)
            return 2

        total = 0
        hist = {}
        matches = []
        match_count = 0
        first_seq = None
        while True:
            chunk = f.read(RECORD_SIZE)
            if len(chunk) < RECORD_SIZE:
                break
            total += 1
            t, size, cpu, seq, tb = struct.unpack_from(HDR_FMT, chunk)
            hist[t] = hist.get(t, 0) + 1
            if t == TR_MMIO:
                pc, addr, value, sz, is_write, block = struct.unpack_from(
                    "<IIIBBBx", chunk, MMIO_PAYLOAD_OFF)
                if block == block_sel:
                    match_count += 1
                    if first_seq is None:
                        first_seq = seq
                    if len(matches) < n:
                        fields = _decode_payload(t, chunk[24:])
                        matches.append((seq, Record(t, cpu, seq, tb, fields).one_line()))

    blkname = MMIO_BLOCK.get(block_sel, str(block_sel))
    print(f"file={path} records={total} producer={prod} ipl_hash={iplh:08X}")
    print("histogram:", {TYPE_NAME.get(t, t): c for t, c in sorted(hist.items())})
    print(f"{blkname}-block MMIO events: {match_count}")
    if matches:
        print(f"first match seq={first_seq}")
        print(f"first {len(matches)} {blkname} events:")
        for seq, line in matches:
            print(f"  seq={seq:<9d} {line}")
    else:
        print(f"NO {blkname}-block events found in this trace.")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    path = sys.argv[1]
    block_arg = sys.argv[2] if len(sys.argv) > 2 else "DI"
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    block_sel = NAME_TO_BLOCK.get(block_arg.upper(), None)
    if block_sel is None:
        try:
            block_sel = int(block_arg, 0)
        except ValueError:
            print(f"unknown block '{block_arg}' (want one of {list(NAME_TO_BLOCK)} or a number)",
                  file=sys.stderr)
            sys.exit(2)
    sys.exit(scan(path, block_sel, n))
