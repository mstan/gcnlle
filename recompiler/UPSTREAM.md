# Vendored DolRecomp fork provenance

- Canonical project: https://github.com/ExpansionPak/DolRecomp
- Integration fork: https://github.com/mstan/DolRecomp
- Fork branch: `gcnrecomp`
- Upstream base: `f3a129d50a28b4586c559a002e2f7bfc15ecf953`
- Vendored fork commit: `e1015cf7ee5d0f9b8f2cacb463417dc9115ff0f9`
- Initial vendoring date: 2026-07-09
- Fork pin updated: 2026-07-14
- License: GPL-3.0 (see `LICENSE`)

This directory STARTED as a source snapshot of the integration-fork commit
above. It is no longer identical to it: the commits listed below were made
directly in gcnrecomp and have NOT been pushed to the public fork. The public
fork preserves the gcnrecomp-specific change series so individual recompiler
improvements can be reviewed or proposed upstream without requiring the rest of
the runtime.

## Local commits beyond the vendored pin

These live only in gcnrecomp. A re-vendor that resets `recompiler/` to the fork
pin would silently drop them, so carry them forward explicitly (see
`docs/UPSTREAM_SYNC.md`).

| Commit | What |
|---|---|
| `7fb3d69` | model the FP-unavailable exception (vector 0x800) |
| `0b4e42c` | boot retail GCN titles with native-miss fallback |
| `7c1bb73` | Wind Waker visual-parity tracing |
| `4398992` | route Gekko float semantics through helpers; fold reservation aliases |
| `8125c2e` | `--symbol-suffix`, so overlay variants can share a guest address |

Upstream-contribution candidates among these (see
`docs/UPSTREAM_SYNC_ASSESSMENT.md`): the psq FP-unavailable exception priority
in `7fb3d69`, and the three-window reservation-alias fold in `4398992` --
upstream's own fix (`93b881c`) masks only the cached/uncached pair. `8125c2e`
parallels a `symbol_suffix` concept upstream already carries in its dispatch
emitter.

General DolRecomp users should use the canonical ExpansionPak repository.
