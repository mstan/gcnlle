#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Poll-aware MMIO value+order diff of a runtime trace against the Dolphin oracle.
#
#   python oracle/mmio_diff.py <runtime.trace> <dolphin.trace>
#
# Both files are streams of trace_format.h records; the runtime emits MMIO-only
# while Dolphin interleaves RETIRED+MMIO+EXI, so we PROJECT each to its MMIO
# events and compare positionally by (addr, size, rw, value).
#
# Busy-wait handling: a poll loop reads the SAME status register value from the
# SAME PC an amount of times set by hardware cycle timing, which we deliberately
# do not model (PRINCIPLES: diff by value+order, never by timing). So before
# comparing we COLLAPSE each maximal run of consecutive identical READS (same
# addr/size/value) to a single event, on BOTH sides. Writes are guest-code
# deterministic and are never collapsed: their count must match exactly. This
# removes the timing dimension while preserving value+order — a wrong polled
# value still diverges loudly, and a missing/extra distinct access still shows.
import struct, sys

BLK = {0:"CP",1:"PE",2:"VI",3:"PI",4:"MI",5:"DSP",6:"DI",7:"SI",8:"EXI",9:"AI",255:"OTH"}
REC = 204  # sizeof(trace record)

def mmio(path):
    b = open(path, "rb").read()[32:]  # skip header
    out = []
    for k in range(0, len(b) // REC * REC, REC):
        if struct.unpack_from("<H", b, k)[0] == 3:  # GCN_TR_MMIO
            pc, a, v, s, w, bl = struct.unpack_from("<IIIBBBx", b, k + 24)
            out.append((a, s, w, v, bl, pc))
    return out

def collapse_reads(evs):
    """Collapse maximal runs of consecutive identical reads (a,s,v). Writes pass
    through untouched."""
    out = []
    for e in evs:
        a, s, w, v, bl, pc = e
        if w == 0 and out:
            pa, ps, pw, pv, _, _ = out[-1]
            if pw == 0 and (pa, ps, pv) == (a, s, v):
                continue  # same polled value from the loop — fold in
        out.append(e)
    return out

def main():
    A = collapse_reads(mmio(sys.argv[1]))
    B = collapse_reads(mmio(sys.argv[2]))
    n = min(len(A), len(B))
    d = next((i for i in range(n) if A[i][:4] != B[i][:4]), None)
    print(f"runtime MMIO(collapsed)={len(A)} dolphin MMIO(collapsed)={len(B)} "
          f"MATCHED={'ALL ' + str(n) if d is None else d}")
    if d is not None:
        for tag, r in (("runtime", A[d]), ("dolphin", B[d])):
            a, s, w, v, bl, pc = r
            print(f"  {tag}: {'W' if w else 'R'}{s*8} {BLK[bl]} "
                  f"0x{a:08X} val=0x{v:08X} pc=0x{pc:08X}")
        print("  --- prev 3 (runtime) ---")
        for r in A[max(0, d - 3):d]:
            a, s, w, v, bl, pc = r
            print(f"    {'W' if w else 'R'}{s*8} {BLK[bl]} "
                  f"0x{a:08X} val=0x{v:08X} pc=0x{pc:08X}")

if __name__ == "__main__":
    main()
