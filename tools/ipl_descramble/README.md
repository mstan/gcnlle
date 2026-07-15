# ipl_descramble

Offline GameCube IPL descrambler — the "descramble offline, recompile the
plaintext" half of **M0** (see `docs/ROADMAP.md`). **M1 adds the faithful
in-CPU/EXI descramble** (real BS1, running from the true reset vector) —
implemented in `runtime/src/exi.c`'s `gcn_exi_set_rom_scrambled`, which calls
this tool's `descramble_core.c` algorithm verbatim (one vendored source, two
consumers — this CLI and the runtime — never reimplemented a second time).

## What it does

The GameCube IPL body is stored under a data-independent stream cipher that the
on-chip BS1 bootrom descrambles at power-on. This tool applies segher's
descrambler to the documented body range and can slice out the plaintext
BS1+BS2 payload — the input the recompiler and the M0 seed contract consume.

```
ipl_descramble <ipl.bin> -o <descrambled.bin>     # full descrambled image
ipl_descramble <ipl.bin> --bs2 <bs2.bin>          # the BS1+BS2 payload slice
```

### Layout constants (from the descrambler + boot analysis; oracle-corrected
2026-07-09 — see `descramble_core.h`'s own comment for the correction history)

| Constant | Value | Meaning |
|---|---|---|
| `IPL_SCRAMBLE_START` | `0x100` | first scrambled byte |
| `IPL_SCRAMBLE_END` | `0x1AFF00` | one past the last scrambled byte |
| `IPL_BS2_FILE_OFF` | `0x100` | plaintext BS1+BS2 image begins here (post-descramble; right after the [0,0x100) plaintext copyright header) |
| `IPL_BS2_LOAD_ADDR` | `0x81200000` | BS1 loads here; Dolphin's HLE enters at 0x81200150 |

So the M0 seed payload is `descrambled[0x100 .. 0x1AFF00)`, loaded at
`0x81200000`. **BS2 itself** (the menu, at guest `0x81300000`) is NOT a fixed
file-offset slice of this image at all — M0 gets there by letting BS1's own
tail (from Dolphin's `0x81200150` landing point) perform its real EXI DMA and
dumping the post-DMA MEM1 (`runtime/generate_postdma.sh`); M1 (`runtime/
generate_bs1.sh`) gets there the honest way, running the true reset vector
`0xFFF00100` end to end (real HID0/BAT/MSR bring-up, then the same DMA).
Empirically, that DMA copies exactly `0x16FFE0` bytes starting at file offset
`0x820` — the earlier "to `IPL_SCRAMBLE_END`" estimate was wrong; the
remaining scrambled bytes are other data (later, on-demand resources) BS1's
own bulk copy never touches. See `docs/M1_PLAN.md` and `runtime/src/boot.c`'s
`GCN_BS1_BS2_DMA_LEN`.

## Provenance & correctness

The algorithm was reversed by **Segher Boessenkool (2008)**. The implementation
form is preserved from Dolphin's `EXI_DeviceIPL.cpp` at the commit identified in
`runtime/dsp_lle/UPSTREAM.md`, and was cross-checked **byte-for-byte against two
additional sources** before vendoring, per our tool-skepticism rule:

- gc-ipl wiki — `ogamespec/gc-ipl` `wiki/Descrambler.md`
- `FIX94/Nintendont` `loader/source/ipl.c`

Both agree on every constant (`t=0x2953 u=0xd9c2 v=0x3ff1 x=1`; feedback masks
`0xa740 / 0xfb10 / 0xb3d0`; the range `[0x100, 0x1AFF00)`).

### Test tiers

- **`test_descramble` (ROM-free, in CI):** pins the first 32 keystream bytes
  (regression on the LFSR math) and checks the involution property
  (descramble∘descramble = identity).
- **ROM-gated (manual, once you supply `bios/ipl.bin`):** descramble the real
  dump and verify the output against a known IPL hash — this is what confirms
  the keystream is genuinely segher's and not merely self-consistent. Not in CI
  because the firmware is copyrighted and not committed.

This is a build-time tool; it is **not** oracle code and shares nothing with
Dolphin.
