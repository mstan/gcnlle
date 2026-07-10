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

`bios/ipl.bin` is the **NTSC-U (USA) v1.0** IPL — a canonical, known-good dump
(CRC32 `6D740AE7`, MD5 `FAE2B558FFC344467170520D62177E5C`, matching the
documented USA 1.0 hashes). All three regional dumps are 2,097,152 bytes
(0x200000) and descramble to valid plaintext (recognizable IPL menu strings).
SHA-1 of the scrambled dumps:

| Region | Primary | SHA-1 |
|---|---|---|
| USA (NTSC-U 1.0) | ← `bios/ipl.bin` | `015808f637a984acde6a06efa7546e278293c6ee` |
| JAP (NTSC-J) | | `f1b0ef434cd74fd8fe23698e2fc911d945b45bf1` |
| EUR (PAL) | | `6f305c37dc1fbe332883bb8153eee26d3d325629` |

Note: Dolphin warns our USA dump is "not known-good" and its message cites
CRC `6DAC1F2A` — but that is the **JAPAN (DOL-001)** bootrom hash, not USA.
Our `6D740AE7` is the correct USA 1.0 CRC; Dolphin boots it fine.

After offline descramble (`tools/ipl_descramble`), the BS2 code image is
`descrambled[0x100 .. 0x1AFF00)` — **loads at `0x81200000`, entry `0x81200150`**
(oracle-corrected 2026-07-09; `[0, 0x100)` is the copyright header, and the
earlier `0x820`/`0x81300000` values were wrong). Reference USA 1.0/1.1/1.2 CRCs:
`6D740AE7` / `D5E6FEEA` / `86573808`.

## Scrambling (research item — Milestone 0)

The GameCube IPL image is stored **scrambled**; the on-chip BS1 boot code
descrambles the main IPL body at power-on. The faithful LLE path recompiles
the descrambler too (or descrambles offline and recompiles the plaintext).
See `docs/ROADMAP.md` Milestone 0.
