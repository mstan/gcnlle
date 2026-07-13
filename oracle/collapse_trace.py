#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
oracle/collapse_trace.py — fold busy-wait MMIO poll runs out of a raw
trace_format.h capture, producing a smaller trace file in the SAME format.

Why: a poll loop reads the SAME status register value from the SAME PC an
amount of times set by hardware cycle timing, which we deliberately do not
model (PRINCIPLES: diff by value+order, never by timing — see mmio_diff.py's
module docstring, which documents this exact collapse rule for its in-memory
MMIO projection). A long boot/menu capture can be dominated by these spins
(the disc-boot trace this tool was written for: ~19.3M records, all but a
few hundred are the "DI:CC006004==2" cover/ready poll spinning at the null-
apploader wall). Collapsing them keeps the file tractable for diff.py/
mmio_diff.py without losing any information diff-by-value+order cares about.

Rule (mirrors mmio_diff.py's collapse_reads exactly, generalized from its
MMIO-only in-memory list to a full mixed-type trace FILE):
  - Only TR_MMIO records are eligible to fold.
  - A TR_MMIO READ record folds into (is dropped after) the most recent
    surviving TR_MMIO record IFF that record is ALSO a read with the
    IDENTICAL (addr, size, value). "Most recent surviving TR_MMIO record"
    skips over any interleaved non-MMIO records (EXI/DMA/INTR/GX/VI/DSP/
    RETIRED) exactly as mmio_diff.py's mmio()-then-collapse_reads pipeline
    does (it drops non-MMIO records before folding, then diffs them
    separately) — so those other record types are NEVER folded and never
    act as a "break" in an otherwise-identical MMIO read run.
  - TR_MMIO WRITE records are NEVER folded (guest-code deterministic; must
    match exactly, per mmio_diff.py's docstring).
  - All non-MMIO records pass through completely untouched, in original
    order, with original seq numbers (seq is informational only — diff.py
    orders by stream position, not seq value — so gaps left by folded
    records are fine and preserve provenance back to the raw capture).

Usage:  python oracle/collapse_trace.py <in.trace> <out.trace>
Exit codes: 0 ok, 2 file/format error.
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")
from diff import FHDR_FMT, FHDR_SIZE, HDR_FMT, MAGIC, RECORD_SIZE, TR_MMIO

MMIO_PAYLOAD_OFF = 24  # pc,addr,value,size,is_write,block — see diff.py MMIO_FMT


def collapse(in_path, out_path):
    with open(in_path, "rb") as fin, open(out_path, "wb") as fout:
        fhdr = fin.read(FHDR_SIZE)
        if len(fhdr) < FHDR_SIZE:
            print(f"{in_path}: too small to hold a file header", file=sys.stderr)
            return 2
        magic, ver, hs, rs, fl, prod, iplh = struct.unpack_from(FHDR_FMT, fhdr)
        if magic != MAGIC or rs != RECORD_SIZE or hs != FHDR_SIZE:
            print(f"{in_path}: bad header (magic={magic:08X} hdr={hs} rec={rs})",
                  file=sys.stderr)
            return 2
        fout.write(fhdr)

        total = 0
        kept = 0
        folded = 0
        last_mmio_read = None  # (addr, size, value) of the last SURVIVING MMIO read

        while True:
            chunk = fin.read(RECORD_SIZE)
            if len(chunk) < RECORD_SIZE:
                break  # tolerate a truncated tail record, same as inspect_trace.py
            total += 1
            t = struct.unpack_from("<H", chunk, 0)[0]
            if t == TR_MMIO:
                # MMIO_FMT = "<IIIBBBx" -> pc,addr,value,size,is_write,block
                pc, addr, value, size, is_write, block = struct.unpack_from(
                    "<IIIBBBx", chunk, MMIO_PAYLOAD_OFF)
                if is_write == 0:
                    key = (addr, size, value)
                    if last_mmio_read is not None and last_mmio_read == key:
                        folded += 1
                        continue  # fold: identical consecutive MMIO read
                    last_mmio_read = key
                else:
                    last_mmio_read = None  # a write breaks any read run
            fout.write(chunk)
            kept += 1

    print(f"{in_path}: {total} records -> {out_path}: {kept} records "
          f"({folded} MMIO reads folded, {100.0 * folded / max(total, 1):.1f}%)")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("usage: collapse_trace.py <in.trace> <out.trace>", file=sys.stderr)
        sys.exit(2)
    sys.exit(collapse(sys.argv[1], sys.argv[2]))
