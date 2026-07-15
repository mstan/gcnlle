# DolRecomp — gcnrecomp integration fork

This directory is vendored from the project-specific DolRecomp fork used by
[gcnrecomp](https://github.com/mstan/gcnrecompiled), an LLE-first experiment
that statically recompiles the Nintendo GameCube IPL and runs it against a
low-level hardware runtime.

For the canonical DolRecomp project, general-purpose GameCube/Wii/Wii U
recompilation, documentation, support, and new development, use
**[ExpansionPak/DolRecomp](https://github.com/ExpansionPak/DolRecomp)**.
This fork is not intended to replace or compete with the official project.

The public integration history lives at
**[mstan/DolRecomp](https://github.com/mstan/DolRecomp)**. See
[UPSTREAM.md](UPSTREAM.md) for the exact vendored commit.

## What this branch adds

- A GameCube IPL frontend for a user-supplied, descrambled boot-ROM payload,
  including explicit load base and entry-point handling.
- Generated-code ABI changes needed by the gcnrecomp LLE runtime.
- O(1) generated dispatch and nullable host-call dispatch.
- Optional derived-cycle accounting, deadline yields, and coalesced basic-block
  cycle charging.
- Generated MEM1 fast paths and exact in-chunk `bl`/`blr` shadow handling.
- Code-generation performance work: selective PC stamps, invocation-local
  state, inline helper paths, and a class-based tight polling-loop yield.
- Focused compile-and-run regression tests for the shadow-call and polling-loop
  transformations.

The IPL mode and runtime ABI are gcnrecomp-specific. The dispatch, exactness,
testing, and code-generation improvements are the most plausible candidates
for focused upstream work.

## Build and test

Requirements are the same as upstream: CMake, a C11 compiler, and optional
zlib support for compressed RPX input.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

This directory does not include Nintendo firmware, game images, save data, or
generated IPL source. Those inputs remain outside Git.

## Upstreaming

Issues in the general recompiler should be reproduced against and reported to
the official project. If a change here is useful upstream, prefer a small,
focused pull request against `ExpansionPak/DolRecomp` rather than merging the
integration branch wholesale.

The public fork preserves the individual recompiler steps so upstream authors
can inspect or cherry-pick narrowly scoped work.

## Attribution and development disclosure

DolRecomp is authored and maintained by the ExpansionPak contributors. Their
copyright, contributor records, and GPL-3.0 license are preserved here.

The changes on the `gcnrecomp` branch are maintained separately by the
gcnrecomp project and include AI-assisted development under human direction
and review. They must not be attributed to the official DolRecomp authors, and
the official project's own development statements apply only to its upstream
repository.

## License

GPL-3.0. See [LICENSE](LICENSE) and [UPSTREAM.md](UPSTREAM.md).
