#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Poll-aware MMIO value+order diff of a runtime trace against the Dolphin oracle.
#
#   python oracle/mmio_diff.py [--resync] [--window N] [--confirm K] \
#          <runtime.trace> <dolphin.trace>
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
#
# ---------------------------------------------------------------------------
# STRICT MODE (default)
#   Positional value+order diff. Stops at the FIRST mismatch and reports it.
#   This is the load-bearing, no-mercy comparison: any divergence halts here.
#
# RESYNC MODE (--resync) — and WHY IT CANNOT LIE
# ---------------------------------------------------------------------------
#   Collapse folds *timing* (how many times a loop spun on one value), but it
#   cannot fold a subtler timing effect: an ASYNC hardware unit that advances
#   between the moment each side sampled it. Concretely, the runtime drives a
#   real DSP-LLE core that posts the boot mail a few polls LATER than Dolphin's
#   instant HLE-DSP. So at the DSP->CPU mailbox read (0xCC005004, pc 0x81333B2C)
#   the runtime's collapsed stream carries an EXTRA read (val=0x0000, "not yet")
#   before it converges to the oracle's 0x8054, and the adjacent DSP CSR read
#   (0xCC00500A) samples a transient HALT bit on one side but not the other.
#   These are not behavioral divergences — the guest issues the identical
#   sequence of accesses and only OBSERVES a different in-flight value.
#
#   Strict mode halts at the mailbox and can never validate the ~thousands of
#   downstream entries (VI/PI/DI traffic). RESYNC re-aligns across these
#   timing artifacts so the downstream stream is still checked — WITHOUT ever
#   masking a real divergence. The honesty guarantees are structural:
#
#     1. RESYNC IS ONLY EVER ATTEMPTED ON READ-vs-READ TIMING SHAPE.
#        The trigger gate requires: both sides READ, the SAME (addr,size,rw),
#        the SAME guest pc, and only the VALUE differs. A write on either side,
#        a different address, or a different access width is NOT this shape and
#        is an immediate HARD divergence. Writes are guest-deterministic; their
#        count, order, and values must match exactly and are never skipped.
#
#     2. ONLY COLLAPSED READ RUNS MAY BE SKIPPED. To re-align we may advance
#        one or both sides past extra reads, but every skipped entry must be a
#        READ. If a skip window would step over a WRITE, that candidate is
#        rejected. A genuine write-sequence divergence therefore cannot be
#        skated past — it always surfaces as a hard divergence.
#
#     3. THE WINDOW IS SMALL AND BOUNDED (--window, default 8). Re-alignment is
#        searched only a few collapsed entries ahead on either side, and the
#        MINIMAL skip is chosen. Two traces that genuinely take different device
#        paths (different addresses, different write order, a counter that
#        diverges and never reconverges — e.g. a VI beam-position register that
#        counts from a different origin) will NOT re-align inside the window and
#        are reported as a hard divergence. You cannot widen your way to a
#        false match without saying so on the command line.
#
#     4. RE-ALIGNMENT MUST BE CONFIRMED (--confirm, default 4). A candidate skip
#        is accepted only if the entry it lands on matches EXACTLY and the next
#        few pairs are each either an exact match or themselves the same benign
#        read-timing shape (same addr/size/rw/pc, value-only difference). Any
#        write mismatch or order/address mismatch inside the confirmation window
#        rejects the candidate. This prevents a coincidental single-entry match
#        from stitching two genuinely diverged streams together.
#
#     5. EVERY RESYNC IS REPORTED LOUDLY — the index, which side advanced, and
#        the full skipped entries (addr/val/pc). A resync is evidence for a
#        human, never a silent fixup. If the DSP model should stop lagging, or a
#        status bit should match, the resync log is exactly where you see it.
#
#   The reasoning behind #1/#2: a lone read whose value differs while the
#   surrounding ORDER of accesses (and every write) is identical is, by
#   construction, not a behavioral divergence — had the guest branched on that
#   value, its subsequent accesses would differ, and the bounded confirmation
#   window + the write rule would catch that. So tolerating an aligned poll
#   value difference (loudly) is faithful to "diff by value+order": order is
#   preserved, writes are exact, and the only thing forgiven is when an async
#   unit was sampled a beat apart.
#
#   Default behavior is unchanged: without --resync the strict diff runs and
#   halts at the first mismatch exactly as before.
#
# COUNTER-POLL COLLAPSE (--counter-polls) — for time-derived register VALUES
# ---------------------------------------------------------------------------
#   The identical-value collapse folds a loop that spins on one value; resync
#   tolerates a one-beat sampling skew. Neither can handle a poll of a COUNTER
#   (the VI vertical-beam position): the guest sits at ONE pc reading ONE
#   register whose value ascends with time, and exits when it hits a target.
#   The intermediate values are pure timing (how far the beam had swept when
#   the loop started — cycle accounting we deliberately do not model); the
#   value the guest ACTS ON is the run's exit value. So with --counter-polls a
#   maximal run of >=2 consecutive READS at the same (addr, size, pc) with
#   varying values collapses to its FINAL read, on both sides. Honesty
#   properties: the exit value still must match (a wrong beam formula that
#   exits the loop at a different value diverges); writes are untouched; a
#   counter whose wrong rate changes GUEST BEHAVIOR shows up immediately in
#   the following access order. Every fold is counted and reported.
import struct, sys

BLK = {0:"CP",1:"PE",2:"VI",3:"PI",4:"MI",5:"DSP",6:"DI",7:"SI",8:"EXI",9:"AI",255:"OTH"}
REC = 204  # sizeof(trace record)

def mmio(path):
    b = open(path, "rb").read()[32:]  # skip header (trailing partial record ignored)
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

def collapse_counter_polls(evs):
    """Collapse maximal runs of >=2 consecutive reads at the same (addr, size,
    pc) — a single-pc poll of a hardware counter — to the run's FINAL read (the
    exit value the guest acted on). Returns (collapsed, folds, folded_reads)."""
    out = []
    folds = 0
    folded = 0
    i = 0
    while i < len(evs):
        e = evs[i]
        j = i + 1
        if e[2] == 0:  # read: extend the same-(addr,size,pc) run
            while (j < len(evs) and evs[j][2] == 0 and
                   evs[j][0] == e[0] and evs[j][1] == e[1] and evs[j][5] == e[5]):
                j += 1
        if j - i >= 2:
            folds += 1
            folded += (j - i) - 1
            out.append(evs[j - 1])   # keep the exit value only
        else:
            out.append(e)
        i = j
    return out, folds, folded

def fmt(r):
    a, s, w, v, bl, pc = r
    return f"{'W' if w else 'R'}{s*8} {BLK[bl]} 0x{a:08X} val=0x{v:08X} pc=0x{pc:08X}"

def first_strict_diff(A, B):
    n = min(len(A), len(B))
    return next((i for i in range(n) if A[i][:4] != B[i][:4]), None)

# ----- strict report (unchanged behavior) --------------------------------
def report_strict(A, B):
    n = min(len(A), len(B))
    d = first_strict_diff(A, B)
    print(f"runtime MMIO(collapsed)={len(A)} dolphin MMIO(collapsed)={len(B)} "
          f"MATCHED={'ALL ' + str(n) if d is None else d}")
    if d is not None:
        for tag, r in (("runtime", A[d]), ("dolphin", B[d])):
            print(f"  {tag}: {fmt(r)}")
        print("  --- prev 3 (runtime) ---")
        for r in A[max(0, d - 3):d]:
            print(f"    {fmt(r)}")

# ----- resync pass -------------------------------------------------------
def benign_read_shape(ra, rb):
    """True iff ra/rb are the timing-artifact shape: both READS, same
    (addr,size,rw) at the same pc, differing only in value."""
    return (ra[2] == 0 and rb[2] == 0 and         # both reads
            ra[0] == rb[0] and ra[1] == rb[1] and  # same addr, size
            ra[5] == rb[5] and                     # same guest pc
            ra[3] != rb[3])                        # differing value

def _candidate_valid(A, B, ia, ib, sa, sb, confirm):
    """A (skipA=sa, skipB=sb) re-alignment is valid iff every skipped entry is
    a READ, the landed pair matches exactly, and the confirmation window holds
    (each pair an exact match or a benign read-timing shape; a write/order
    mismatch inside it rejects the candidate)."""
    for j in range(ia, ia + sa):                   # skipped runtime entries: reads only
        if j >= len(A) or A[j][2] != 0:
            return False
    for j in range(ib, ib + sb):                   # skipped dolphin entries: reads only
        if j >= len(B) or B[j][2] != 0:
            return False
    pa, pb = ia + sa, ib + sb
    if pa >= len(A) or pb >= len(B):
        return False
    if A[pa][:4] != B[pb][:4]:                     # must land on an exact match
        return False
    for t in range(1, confirm):                    # confirmation window
        qa, qb = pa + t, pb + t
        if qa >= len(A) or qb >= len(B):
            break                                  # exhaustion is acceptable
        if A[qa][:4] == B[qb][:4]:
            continue
        if benign_read_shape(A[qa], B[qb]):
            continue                               # a further poll-value artifact — allowed
        return False                               # write/order/addr mismatch — reject
    return True

def find_resync(A, B, ia, ib, window, confirm):
    """Search minimal-skip re-alignment. Returns (skipA, skipB) or None.
    Iterates by ascending total skip so the minimal is chosen first."""
    for total in range(1, 2 * window + 1):
        for sa in range(0, min(total, window) + 1):
            sb = total - sa
            if sb < 0 or sb > window:
                continue
            if sa == 0 and sb == 0:
                continue
            if _candidate_valid(A, B, ia, ib, sa, sb, confirm):
                return (sa, sb)
    return None

def report_resync(A, B, window, confirm):
    d_strict = first_strict_diff(A, B)
    print(f"runtime MMIO(collapsed)={len(A)} dolphin MMIO(collapsed)={len(B)}")
    print(f"RESYNC mode: window={window} confirm={confirm}  "
          f"(strict would stop at {d_strict})")
    print("-" * 72)

    ia = ib = 0
    matched = 0
    resyncs = 0
    skipped_rt = 0
    skipped_do = 0
    hard = None  # ("gate"|"nowindow"|"exhaust", ia, ib)

    while ia < len(A) and ib < len(B):
        if A[ia][:4] == B[ib][:4]:
            matched += 1
            ia += 1
            ib += 1
            continue
        ra, rb = A[ia], B[ib]
        if not benign_read_shape(ra, rb):
            hard = ("gate", ia, ib)         # write / addr / width mismatch
            break
        cand = find_resync(A, B, ia, ib, window, confirm)
        if cand is None:
            hard = ("nowindow", ia, ib)     # read values won't re-align in window
            break
        sa, sb = cand
        resyncs += 1
        skipped_rt += sa
        skipped_do += sb
        # loud per-resync report
        which = []
        if sa: which.append(f"runtime +{sa}")
        if sb: which.append(f"dolphin +{sb}")
        print(f"RESYNC #{resyncs} at runtime[{ia}]/dolphin[{ib}]: advanced "
              f"{', '.join(which)}  (re-align on {fmt(A[ia+sa])})")
        print(f"    gate  runtime: {fmt(ra)}")
        print(f"    gate  dolphin: {fmt(rb)}")
        for j in range(ia, ia + sa):
            print(f"    skip  runtime[{j}]: {fmt(A[j])}")
        for j in range(ib, ib + sb):
            print(f"    skip  dolphin[{j}]: {fmt(B[j])}")
        ia += sa
        ib += sb

    if hard is None and (ia >= len(A) or ib >= len(B)):
        # ran a side to the end without a hard divergence
        if ia < len(A) or ib < len(B):
            hard = ("exhaust", ia, ib)

    print("-" * 72)
    print(f"matched            = {matched}")
    print(f"resyncs            = {resyncs}")
    print(f"skipped reads rt   = {skipped_rt}")
    print(f"skipped reads dol  = {skipped_do}")
    if d_strict is not None:
        gained = matched - d_strict
        print(f"newly validated    = {gained}  (matched pairs beyond the strict "
              f"stop at {d_strict})")
    else:
        print("newly validated    = 0  (strict already matched all)")

    if hard is None:
        print("HARD divergence    = NONE (consumed the shorter stream cleanly)")
        return
    kind, ia, ib = hard
    label = {"gate":  "gate mismatch (write / addr / width — not a read-timing shape)",
             "nowindow": f"read values do not re-align within window={window}",
             "exhaust": "one side exhausted with entries remaining on the other"}[kind]
    print(f"HARD divergence    = FIRST at runtime[{ia}]/dolphin[{ib}]: {label}")
    if ia < len(A):
        print(f"  runtime: {fmt(A[ia])}")
    if ib < len(B):
        print(f"  dolphin: {fmt(B[ib])}")
    print("  --- prev 3 (runtime) ---")
    for r in A[max(0, ia - 3):ia]:
        print(f"    {fmt(r)}")
    print("  --- next 3 (runtime) ---")
    for r in A[ia + 1:ia + 4]:
        print(f"    {fmt(r)}")
    print("  --- next 3 (dolphin) ---")
    for r in B[ib + 1:ib + 4]:
        print(f"    {fmt(r)}")

def main():
    args = sys.argv[1:]
    resync = False
    counter_polls = False
    window = 8
    confirm = 4
    pos = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--resync":
            resync = True
        elif a == "--counter-polls":
            counter_polls = True
        elif a == "--window":
            i += 1; window = int(args[i])
        elif a == "--confirm":
            i += 1; confirm = int(args[i])
        elif a.startswith("--window="):
            window = int(a.split("=", 1)[1])
        elif a.startswith("--confirm="):
            confirm = int(a.split("=", 1)[1])
        else:
            pos.append(a)
        i += 1
    if len(pos) != 2:
        sys.exit("usage: mmio_diff.py [--resync] [--counter-polls] [--window N] "
                 "[--confirm K] <runtime.trace> <dolphin.trace>")
    A = collapse_reads(mmio(pos[0]))
    B = collapse_reads(mmio(pos[1]))
    if counter_polls:
        A, fa, ra = collapse_counter_polls(A)
        B, fb, rb = collapse_counter_polls(B)
        print(f"counter-poll collapse: runtime {fa} runs/{ra} reads folded, "
              f"dolphin {fb} runs/{rb} reads folded")
    if resync:
        report_resync(A, B, window, confirm)
    else:
        report_strict(A, B)

if __name__ == "__main__":
    main()
