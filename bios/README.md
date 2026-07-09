# bios/ — GameCube IPL firmware

This directory holds the **GameCube IPL** (the console boot ROM / "BIOS"),
which gcnrecomp statically recompiles to produce the LLE boot experience:
the animated rolling-cube logo, the main menu, the memory-card manager,
date/time and sound/screen options, and the disc-load screen.

## You must supply your own dump

The IPL is copyrighted Nintendo firmware and is **not** distributed here
(see `.gitignore`). Dump it from a console you own, exactly as psxrecomp
expects you to supply `SCPH1001.BIN`. Place it here, e.g.:

    bios/ipl.bin           # NTSC-U / NTSC-J / PAL — region matters

## Region / revision

The IPL exists in several regional and revision variants (the boot
animation and menu differ). Record which dump you are targeting in your
per-build config; the recompiler slice-walks the specific image you point
it at.

### Dumps in use (identity, not the ROM itself)

`bios/ipl.bin` is the **NTSC-U (USA) v1.0** IPL. All three regional dumps are
2,097,152 bytes (0x200000) and were verified to descramble to valid plaintext
(recognizable IPL menu strings). SHA-1 of the scrambled dumps:

| Region | Primary | SHA-1 |
|---|---|---|
| USA (NTSC-U 1.0) | ← `bios/ipl.bin` | `015808f637a984acde6a06efa7546e278293c6ee` |
| JAP (NTSC-J) | | `f1b0ef434cd74fd8fe23698e2fc911d945b45bf1` |
| EUR (PAL) | | `6f305c37dc1fbe332883bb8153eee26d3d325629` |

After offline descramble (`tools/ipl_descramble`), the BS2 payload is
`descrambled[0x820 .. 0x1AFF00)` — **loads at and enters from `0x81300000`**.

## Scrambling (research item — Milestone 0)

The GameCube IPL image is stored **scrambled**; the on-chip BS1 boot code
descrambles the main IPL body at power-on. The faithful LLE path recompiles
the descrambler too (or descrambles offline and recompiles the plaintext).
See `docs/ROADMAP.md` Milestone 0.
