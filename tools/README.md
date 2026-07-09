# tools/ — external binary/asset tooling

## decomp-toolkit (`dtk`)

[encounter/decomp-toolkit](https://github.com/encounter/decomp-toolkit)
(MIT/Apache, Rust) — the de-facto standard GameCube/Wii binary CLI. Used here
as an **external tool**, not vendored, for:

- `dtk dol info / disasm` — cross-check the recompiler's decode of the IPL
  against an independent disassembler (Tool Skepticism: validate first outputs).
- `dtk disc info / extract` — pull DOL/FST/assets from GCM/ISO/RVZ when we
  reach the disc phase (M5+).
- Compression/container utils (Yaz0/Yay0, RARC, U8) for later game work.

Install with `cargo install --git https://github.com/encounter/decomp-toolkit`
or grab a release binary; keep it on PATH. Not committed.

## Reference-only hardware maps

- `gcrecomp` (MIT) `include/gcrecomp/hw/gc_hw.h`, `os_defs.h` — GameCube MMIO
  register map / OS struct layouts. Consult when modeling EXI/VI/DI/SI in
  `runtime/`; do not pull in its HLE OS layer.
