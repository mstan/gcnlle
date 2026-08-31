#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Recompile captured guest pages into a content-keyed overlay module.

WHY
---
Relocatable overlay code (Wind Waker's REL modules) is loaded and relocated
into RAM at runtime, so it never appears in any static image and the ordinary
DOL/IPL codegen cannot see it. Every execution of it falls back to the
interpreter. On a Wind Waker title-screen run that was ~99% of all interpreter
fallbacks, concentrated in ~118 distinct 4 KiB pages.

The runtime already journals those misses and dumps the page bytes that were
resident at the time. This tool turns those captured pages into real
recompiled functions.

WHY IT IS KEYED BY CONTENT, NOT ADDRESS
---------------------------------------
One page hosts many different overlays over a run -- 0x80F00000 was seen with
204 distinct contents in a single session -- so an address does not identify
code here. Each captured page is compiled separately with
`--symbol-suffix _h<fnv1a64>`, and the emitted table lets the runtime pick the
variant whose hash matches the live bytes. A page whose content matches nothing
we compiled simply misses and falls back, exactly as before: this never guesses.

USAGE
-----
  gen_overlay_module.py --journal captures/native-misses.jsonl \\
                        --pages   captures/pages \\
                        --dolrecomp recompiler/build/dolrecomp.exe \\
                        --output  generated/overlay \\
                        --top-pages 80
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

PAGE_SIZE = 4096
BLOB_RE = re.compile(r"^([0-9A-Fa-f]{8})_([0-9A-Fa-f]{8})\.bin$")


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser()
    p.add_argument("--journal", type=Path, required=True)
    p.add_argument("--pages", type=Path, required=True)
    p.add_argument("--dolrecomp", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--top-pages", type=int, default=80,
                   help="recompile only the N hottest pages (0 = all captured)")
    p.add_argument("--tail-mb", type=int, default=60,
                   help="how much of the append-only journal to score, from "
                        "the END -- the journal spans many builds, so scoring "
                        "it from the start measures history, not this build")
    p.add_argument("--jobs", type=int, default=2)
    return p.parse_args()


def score_pages(journal: Path, tail_mb: int) -> collections.Counter:
    """Miss count per 4 KiB page, from the tail of the append-only journal."""
    counts: collections.Counter = collections.Counter()
    if not journal.is_file():
        return counts
    size = journal.stat().st_size
    with journal.open("rb") as f:
        f.seek(max(0, size - tail_mb * 1024 * 1024))
        f.readline()  # discard the partial line we most likely landed in
        for raw in f:
            try:
                pc = json.loads(raw).get("pc")
            except Exception:
                continue
            if isinstance(pc, int):
                counts[pc & ~(PAGE_SIZE - 1)] += 1
    return counts


def collect_blobs(pages_dir: Path) -> dict[int, list[Path]]:
    by_addr: dict[int, list[Path]] = collections.defaultdict(list)
    for entry in os.listdir(pages_dir):
        m = BLOB_RE.match(entry)
        if not m:
            continue
        by_addr[int(m.group(1), 16)].append(pages_dir / entry)
    return by_addr


def canonical(addr: int) -> int:
    """Overlay pages are recorded in the cached MEM1 view, matching the
    runtime's canonical_mem1()."""
    if addr < 0x01800000:
        return addr | 0x80000000
    return addr


def recompile(dolrecomp: Path, blob: Path, addr: int, suffix: str,
              workdir: Path) -> Path | None:
    """Recompile one captured page.

    `addr` MUST already be canonical (the cached 0x8xxxxxxx view). The runtime
    canonicalizes the dispatch address before lookup, and the generated body
    switches on ctx->pc against its own compile-time labels -- so if the page
    is compiled at the raw captured base (e.g. physical 0x00000000) while the
    table registers it canonically (0x80000000), the switch matches nothing,
    the function returns having done nothing, dispatch still reports success,
    and the block loop spins on an unchanging pc forever. That is not
    hypothetical: it happened, and presented as 5M blocks/s with zero frames.
    """
    out = workdir / suffix
    cmd = [str(dolrecomp), "--gamecube-ipl", str(blob), str(out),
           "--base", "0x%08X" % addr, "--entry", "0x%08X" % addr,
           "--chunk-instructions", "1024", "--symbol-suffix", suffix]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("  ! recompile failed for 0x%08X %s: %s"
              % (addr, suffix, (r.stderr or "").strip()[:200]))
        return None
    chunks = list((out / "generated" / "chunks").glob("*.c"))
    if len(chunks) != 1:
        print("  ! expected exactly 1 chunk for a 4KiB page, got %d (0x%08X)"
              % (len(chunks), addr))
        return None
    # The emitted switch must dispatch the address we are going to register.
    # Cheap here, and the alternative failure mode is an infinite spin at
    # runtime that looks like a speedup.
    want = "case 0x%08Xu:" % addr
    if want not in chunks[0].read_text(encoding="utf-8", errors="replace"):
        print("  ! label mismatch: %s not found in %s -- refusing to register"
              % (want, chunks[0].name))
        return None
    return chunks[0]


def main() -> int:
    args = parse_args()
    if not args.pages.is_dir():
        print("error: --pages is not a directory: %s" % args.pages)
        return 1

    blobs = collect_blobs(args.pages)
    if not blobs:
        print("error: no captured page blobs found in %s" % args.pages)
        return 1

    scores = score_pages(args.journal, args.tail_mb)
    if scores:
        ranked = [a for a, _ in scores.most_common()]
        if args.top_pages > 0:
            ranked = ranked[: args.top_pages]
        selected = [a for a in ranked if a in blobs]
        covered = sum(scores[a] for a in selected)
        total = sum(scores.values())
        print("journal: %d miss records over %d pages; selecting %d pages "
              "covering %.1f%% of misses"
              % (total, len(scores), len(selected),
                 100.0 * covered / max(total, 1)))
        missing = [a for a in ranked if a not in blobs]
        if missing:
            print("  note: %d hot pages have no captured blob and are skipped"
                  % len(missing))
    else:
        selected = sorted(blobs)
        print("no journal scores available; taking all %d captured addresses"
              % len(selected))

    # (address, hash) -> unique variant. Two files can carry the same bytes;
    # hashing the content rather than trusting the filename de-duplicates them.
    work: list[tuple[int, int, Path, str]] = []
    seen: set[tuple[int, int]] = set()
    for addr in selected:
        for blob in sorted(blobs[addr]):
            data = blob.read_bytes()
            if len(data) != PAGE_SIZE:
                continue
            h = fnv1a64(data)
            # Canonical from here on: compile base, symbol, and table entry all
            # agree, which is the invariant the label check above enforces.
            caddr = canonical(addr)
            if (caddr, h) in seen:
                continue
            seen.add((caddr, h))
            work.append((caddr, h, blob, "_h%016X" % h))

    print("recompiling %d page variants across %d addresses..."
          % (len(work), len(selected)))

    args.output.mkdir(parents=True, exist_ok=True)
    chunks_dir = args.output / "chunks"
    if chunks_dir.exists():
        shutil.rmtree(chunks_dir)
    chunks_dir.mkdir(parents=True)

    produced: list[tuple[int, int, str]] = []
    abi_copied = False
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)

        def run_one(item):
            addr, h, blob, suffix = item
            return item, recompile(args.dolrecomp, blob, addr, suffix, tmpdir)

        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            for (addr, h, _blob, suffix), chunk in pool.map(run_one, work):
                if chunk is None:
                    continue
                shutil.copyfile(
                    chunk, chunks_dir / ("overlay_%08X%s.c" % (addr, suffix)))
                if not abi_copied:
                    abi = chunk.parent.parent / "generated_abi.h"
                    if abi.is_file():
                        # Every variant emits an identical ABI header (all
                        # static inline), so one shared copy serves them all.
                        shutil.copyfile(abi, args.output / "generated_abi.h")
                        abi_copied = True
                produced.append((addr, h, "func_%08X%s" % (addr, suffix)))

    if not produced:
        print("error: no variants were produced")
        return 1

    produced.sort(key=lambda t: (t[0], t[1]))
    pages: list[tuple[int, int, int]] = []   # (start, first, count)
    for i, (addr, _h, _fn) in enumerate(produced):
        if pages and pages[-1][0] == addr:
            pages[-1] = (addr, pages[-1][1], pages[-1][2] + 1)
        else:
            pages.append((addr, i, 1))

    inc = args.output / "overlay_module_tables.inc"
    with inc.open("w", encoding="utf-8") as f:
        f.write("/* Generated by runtime/tools/gen_overlay_module.py -- do not edit.\n"
                " *\n"
                " * Content-keyed overlay variants recompiled from captured guest\n"
                " * pages. Each entry's hash is the FNV-1a64 of the exact 4 KiB the\n"
                " * function was compiled from, so the runtime can tell which overlay\n"
                " * is resident before letting native code run. */\n\n")
        for _addr, _h, fn in produced:
            f.write("void %s(CPUState* ctx);\n" % fn)
        f.write("\n#define GCN_OVERLAY_PAGE_COUNT %uu\n" % len(pages))
        f.write("#define GCN_OVERLAY_VARIANT_COUNT %uu\n\n" % len(produced))
        f.write("static const GcnOverlayVariant s_overlay_variants[] = {\n")
        for _addr, h, fn in produced:
            f.write("    { 0x%016XULL, %s },\n" % (h, fn))
        f.write("};\n\nstatic const GcnOverlayPage s_overlay_pages[] = {\n")
        for start, first, count in pages:
            f.write("    { 0x%08Xu, 0x%08Xu, %uu, %uu },\n"
                    % (start, start + PAGE_SIZE, first, count))
        f.write("};\n")

    print("wrote %d chunks, %d pages, %d variants -> %s"
          % (len(produced), len(pages), len(produced), args.output))
    return 0


if __name__ == "__main__":
    sys.exit(main())
