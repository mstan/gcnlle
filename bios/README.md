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

## Scrambling (research item — Milestone 0)

The GameCube IPL image is stored **scrambled**; the on-chip BS1 boot code
descrambles the main IPL body at power-on. The faithful LLE path recompiles
the descrambler too (or descrambles offline and recompiles the plaintext).
See `docs/ROADMAP.md` Milestone 0.
