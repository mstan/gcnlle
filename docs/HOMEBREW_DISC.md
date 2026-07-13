# Homebrew disc — design (stretch item, not yet implemented)

Goal: give the IPL's disc-boot path something real to chew on WITHOUT
recompiling a commercial game: a tiny self-authored PPC program on a disc
image, carried through the REAL boot chain (DI reads → apploader → DOL entry),
executing under the runtime. This is deliberately also the recon for the next
phase (game loading), scoped down to code we own.

## What exists today (M5 baseline)

- `tools/make_dummy_disc.py` builds an oracle-symmetric ISO whose apploader is
  null — the IPL's disc-check accepts it, and boot ends at the faithful loud
  boundary (jump to 0x80000000). The scope boundary is exactly "no code to
  run."
- The recompiler ingests ONE image (the IPL) and emits one dispatch table of
  chunk functions; the runtime's `dolrecomp_call` resolves PC → chunk through
  that single table.

## The two missing pieces

### 1. Authoring the guest payload (no toolchain dependency)

No devkitPPC on this machine, and vendoring one contradicts the "tiny" goal.
Two viable routes, in preference order:

a. **Hand-assembled flat PPC** — the payload is a few dozen instructions
   (set up a stack, write a test pattern / magic values to a known MEM1
   region, spin). Emit the word list from a python script
   (`tools/make_homebrew_disc.py`) with an inline mini-assembler (only the
   opcodes we use: lis/addi/stw/b/mtspr…). The apploader too: the IPL calls
   the apploader's three entry points (init/main/close, BE pointers at disc
   offset 0x2440 header) with a documented ABI — main() returns
   (dst, size, disc-offset) triplets telling BS2 what to copy where, then the
   entry PC. A minimal apploader = "copy N bytes from disc offset X to
   0x80003100, entry there" is ~30 instructions.
b. clang bare-metal cross (`--target=powerpc-none-eabi -mcpu=750`) IF a
   mingw clang with the PPC backend is already installed — check first;
   do not install toolchains for this.

### 2. Executing it (the real architecture decision — needs the user)

The payload lands in MEM1 at runtime; there is no recompiled bank for it.
Options, in increasing faithfulness/cost:

- **Static second bank**: `make_homebrew_disc.py` writes the payload binary;
  the recompiler ingests it at its load address as a second image
  (`--extra-image payload.bin@0x80003100`) into the SAME dispatch table
  (chunks don't overlap the IPL ranges). Boot flow: IPL apploader path copies
  bytes into MEM1 (data path is already real), and when the PC enters
  0x80003100 the dispatch table already has code for it. EXACTNESS CAVEAT:
  the recompiler must verify at runtime that the bytes the apploader actually
  copied match the bytes it compiled (hash check at first dispatch into the
  bank — a mismatch is a loud failure, never silent divergence).
- **Dynamic recompile-at-boot**: run the recompiler as a library/subprocess
  when DI reads complete. This is the real game-loading architecture
  (per-game recompilation) — out of scope for the stretch, needs its own
  design round with the user.

The static-second-bank route exercises: DI streaming of a real apploader,
BS2's apploader-calling convention, DOL copy, entry jump — everything the
game path needs except on-demand recompilation. That makes it the right
stretch scope.

## Verification plan (when implemented)

- Golden gates untouched (no disc in gate runs).
- New pinned artifact: boot with `GCN_DISC=homebrew.iso`, run to a fixed
  block count, `GCN_MEM_DUMP` the payload's output region → new golden hash.
- Oracle: capture Dolphin with the same ISO (`--ipl-disc` patch from M5) and
  diff MMIO value+order through the apploader path — this is the interesting
  part: it validates BS2's DI usage under a REAL (non-null) apploader against
  hardware-model truth.
- Screenshot the IPL's transition out of the menu (disc boot skips menu when
  inserted at power-on — M5 already validated the discless/dummy halves).
