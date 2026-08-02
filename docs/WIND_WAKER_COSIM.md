# Wind Waker Differential Co-Simulation

This is the GameCube/Wind Waker application of
`F:\Projects\recomp-template\DIFFERENTIAL-COSIMULATION.md`.

The runtime and Dolphin are independent processes. The runtime exposes parked
state over its localhost JSON TCP server; Dolphin exposes its PPC interpreter
through its GDB remote stub. Dolphin is an oracle and development tool only. It
is not linked into, shipped with, or called by the ordinary AOT-first runtime.

## Current trust boundary

`cosim_state` currently covers:

- the canonical PPC `CPUState` fields;
- all 24 MiB of MEM1;
- the actual MEM2 allocation (zero bytes in GameCube mode);
- the 256 KiB locked-L1 backing store.

It deliberately returns `complete:false`: canonical snapshots for every device
register, FIFO, DMA deadline, and scheduler are not present yet. Register/RAM
matches below are therefore strong localized evidence, not a claim of
full-machine equivalence.

## Harness validation

Validated on the Wind Waker runtime build on 2026-08-02:

| Gate | Result | Evidence |
|---|---|---|
| Runtime A-vs-A | Pass | 10 checkpoints, 1,000 instructions per checkpoint |
| Injected divergence | Pass | XOR of GPR7 localized immediately to the CPU sub-hash |
| Hash-vs-byte audit | Pass | 9,044 canonical CPU bytes, 24 MiB MEM1, empty MEM2, and 256 KiB locked L1 |
| Dolphin B-vs-B | Pending | Required before calling the Dolphin side fully deterministic |

`gate1` performs the byte audit every ten checkpoints by default. This caught
and removed an early harness bug that assumed a 64 MiB Wii MEM2 allocation in
GameCube mode.

## Dolphin launch requirements

Use the patched local `DolphinNoGUI.exe`, the real NTSC-U IPL, the same
`GZLE01.iso`, single-core PPC interpreter mode, and a fixed RTC. Two settings
are easy to get subtly wrong:

- the GDB port is `Dolphin.General.GDBPort`, not `Dolphin.Core.GDBPort`;
- `Dolphin.Interface.DebugModeEnabled=True` is required for continue-to-
  breakpoint. Single-step works without it, which can otherwise make the
  breakpoint failure look like a guest hang.

The clean oracle configuration used for the results below also disables panic
handlers, audio output, and graphics output. It uses
`F:\Projects\gcnrecomp\oracle\dolphin-user`, which contains the matching IPL.

For screenshot/audio oracle work, the isolated visual profile currently fixes
Dolphin's Unix RTC to `1785690000` (`2026-08-02 17:00:00 UTC`). Launch the
runtime with `GCN_RTC_FIXED=839005200`—the same instant expressed as GameCube
seconds since 2000-01-01. Do not compare randomized wave/scene state from the
runtime's default 2004 fixture against that profile.

Wind Waker's `cM_rnd` state is three guest words at `0x803F7338`,
`0x803F733C`, and `0x803F7340`. The title initializes them to `(100, 100,
100)`, but unrelated random consumers can advance the streams before a later
visual milestone. `dolphin-run-to --normalize-u32 ADDRESS:VALUE` can set fixed
values in both parked machines after a conditional checkpoint is reached.
Every before/after word is included in its JSON report. This is a diagnostic
oracle seam only: it requires `--runtime-port`, never runs on an unparked
machine, and is not part of the ordinary AOT/LLE runtime path.

The first title-wave initialization calls `cM_rndFX` at `0x802463E8` with
`LR == 0x80091234` and `r23 == 0`. The current fixed-RTC visual oracle observed PRNG words
`(0x000033C8, 0x00000797, 0x000033A1)` there. The configurable title defaults
are recorded in `WindWakerRecomp/game.toml`; pass those values explicitly when
normalizing a run rather than treating them as framework constants.
Launch the runtime with `GCN_CHECKPOINT_PC=0x802463E8`,
`GCN_CHECKPOINT_LR=0x80091234`, `GCN_CHECKPOINT_GPR=23`, and
`GCN_CHECKPOINT_GPR_VALUE=0`; launch-time auto-arm happens before the first
AOT block and avoids racing past this one-time initialization while the TCP
client connects.

For a post-normalization comparison, `dolphin-run-to --after-pc` uses
`checkpoint_continue` to install the next runtime condition before releasing
the first park. This is the race-free path from the RNG seam to `drawWave`:

```text
--after-pc 0x8009A148 --after-gpr 24 --after-gpr-value 3
```

`--gpr-memory` ranges are sampled at that second gate when it is present.
The runtime half of this handoff was smoke-tested against adjacent live AOT
blocks `0x80247910` and `0x802478D4`: the two parks had consecutive block
indices, proving that `checkpoint_continue` did not execute an unarmed block.
The complete RNG-to-`drawWave` Dolphin comparison still requires the validated
patched oracle launch profile; a stock Dolphin batch profile did not reach the
first title breakpoint and is not accepted as divergence evidence.

## Measured BS2-to-apploader gates

The runtime receives the IPL's decrypted `0x1AFE00`-byte BS2 payload and starts
at the same `0x81200150` entry as Dolphin.

| Guest PC | Meaning | Result |
|---|---|---|
| `0x81200150` | shared BS2 entry | all common PPC registers and BS2 code bytes match |
| `0x8120018C` | first post-timebase-delay instruction | all non-timing registers and sampled RAM match |
| `0x81200214` | post-SI initialization sequence | all non-timing registers and sampled RAM match |
| `0x812005D4` | completed BS2 DI transfer loop | all PPC registers except fake timebase match; full loaded payload matches |
| `0x81300000` | Wind Waker apploader handoff | all PPC registers except fake timebase match; full loaded payload matches |

The compared payload is `0x170000` bytes at `0x81300000`. Both machines
produced:

```text
SHA-256 cfc7f018e813e08278d17a9ffe44af46691b117114e59193e59d9a7400405629
```

The apploader entry gate required 98,442 retired runtime-interpreter
instructions. This exercises the LLE BS2, SI, DI register protocol, repeated
disc DMA transfers, and final PPC handoff; it does not shortcut directly to the
game executable.

## Explicit oracle seams

Dolphin materializes a restart-dependent fake-timebase epoch on the first
`mftb`. While parked under GDB, that hidden fake clock does not have the same
currency as the runtime's cycle-derived timebase. Comparisons therefore:

- report the raw timebase mismatch;
- ignore `r5`/`r6` only at the first delay-loop exit, where they are the two
  direct `mftb` carriers;
- ignore only the timebase after those registers have been overwritten;
- continue to fail on every other register or requested RAM byte.

Dolphin also seeds low-memory bootstrap metadata that the current BS2-only
runtime seam does not. The first difference is `0x80001804` (`0x00` runtime,
`0x53` Dolphin). It exists at the initial checkpoint and is not treated as a
newly created divergence. BS2 code and the disc-loaded apploader payload are
compared strictly.

## Next expansion

1. Add Dolphin-vs-Dolphin determinism and injected-oracle checks.
2. Add canonical per-device snapshots, beginning with PI/VI/DI/SI/DSP and their
   pending interrupt/DMA timing.
3. Use AOT-safe conditional PC checkpoints for game-level milestones so native
   shards remain enabled, then compare the exact RAM ranges that source each GX
   draw or DSP task.
4. Keep screenshots and audio samples as acceptance gates after architectural
   comparisons; matching hashes do not replace visible/audible validation.
