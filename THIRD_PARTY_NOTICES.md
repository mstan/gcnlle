# Third-party notices

gcnrecomp is distributed under GPL-3.0. It includes or adapts the following
third-party free-software components. Original notices and SPDX identifiers are
retained in source files where present.

## DolReComp

`recompiler/` is vendored from the project-specific
[mstan/DolRecomp](https://github.com/mstan/DolRecomp) integration fork, based on
the official [ExpansionPak/DolRecomp](https://github.com/ExpansionPak/DolRecomp)
project. It is licensed GPL-3.0-or-later. The exact source pin and local changes
are documented in `recompiler/UPSTREAM.md`.

## ModernGekko/RecompCore module activation

The content-validated title-module activation in `runtime/src/aot_module.c`
and `runtime/tools/gen_title_module_tables.py` adapts the chunk identity and
invalidate/reverify design from
[ExpansionPak/ModernGekko](https://github.com/ExpansionPak/ModernGekko) and its
RecompCore-ModernGekko dependency, licensed GPL-3.0-or-later. Exact reference
revisions and the retained LLE boundary are documented in
`docs/GPL_EXPERIMENT.md`.

## Dolphin DSP LLE and format references

`runtime/dsp_lle/Core/DSP/` and selected support headers are derived from the
[Dolphin Emulator Project](https://github.com/dolphin-emu/dolphin), licensed
GPL-2.0-or-later. Project-authored adapters and compatibility shims are marked
separately. The exact upstream commit and harvested file set are documented in
`runtime/dsp_lle/UPSTREAM.md`.

The memory-card image tooling is a project-authored implementation based on
Dolphin's GPL-2.0-or-later memory-card source and retains detailed source
citations in `runtime/include/memcard/memcard_image.h`.

## GameCube IPL descrambler

`tools/ipl_descramble/descramble_core.c` implements the GameCube IPL
descrambler reversed by Segher Boessenkool, copyright 2008 Segher Boessenkool,
and is distributed under GPL-2.0-or-later. Its provenance and validation are
documented in `tools/ipl_descramble/README.md`.

## User-supplied data

Nintendo GameCube firmware, DSP ROMs, games, save files, generated IPL source,
and screenshots are not distributed by this repository. Users must supply any
required dumps from hardware or media they are authorized to use.
