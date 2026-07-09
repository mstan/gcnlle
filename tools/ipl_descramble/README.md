# ipl_descramble

Offline GameCube IPL descrambler — the "descramble offline, recompile the
plaintext" half of **M0** (see `docs/ROADMAP.md`). The faithful in-CPU BS1
descramble is deferred to M1.

## What it does

The GameCube IPL body is stored under a data-independent stream cipher that the
on-chip BS1 bootrom descrambles at power-on. This tool applies segher's
descrambler to the documented body range and can slice out the plaintext BS2
payload — the input the recompiler and the M0 seed contract consume.

```
ipl_descramble <ipl.bin> -o <descrambled.bin>     # full descrambled image
ipl_descramble <ipl.bin> --bs2 <bs2.bin>          # just the BS2 payload
```

### Layout constants (from the descrambler + boot analysis)

| Constant | Value | Meaning |
|---|---|---|
| `IPL_SCRAMBLE_START` | `0x100` | first scrambled byte |
| `IPL_SCRAMBLE_END` | `0x1AFF00` | one past the last scrambled byte |
| `IPL_BS2_FILE_OFF` | `0x820` | plaintext BS2 begins here (post-descramble) |
| `IPL_BS2_LOAD_ADDR` | `0x81300000` | BS1 copies BS2 here and enters it |

So the M0 seed payload is `descrambled[0x820 .. 0x1AFF00)`, loaded at and
entered from `0x81300000`.

## Provenance & correctness

The algorithm was reversed by **Segher Boessenkool (2008)**. The implementation
in `descramble_core.c` was cross-checked **byte-for-byte against two independent
sources** before vendoring, per our tool-skepticism rule:

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
