# ModernGekko audit — 2026-07-13

Snapshot audited: ExpansionPak/ModernGekko commit
`4b94e358098970bb6d0482cf383df560e463ed07` (2026-07-12).

## Decision

**Reject ModernGekko as this project's runtime foundation, dependency, or source
of device-model code.** It does not preserve our LLE-first IPL floor. Keep it as
a reference for a few narrow packaging ideas only, and independently implement
any idea we retain.

No known GPL incompatibility is what drives this rejection. ModernGekko
declares GPL-3.0-or-later and its identified RecompCore/Dolphin components are
GPL-compatible with this GPL-3.0 project. The rejection is based on
architecture, fidelity, maturity, and incomplete provenance hygiene.

## Did gcnrecomp already use it?

No evidence was found. Full working-tree and Git-history searches contain no
`ModernGekko` reference. gcnrecomp began on 2026-07-09; the two commits in the
audited ModernGekko repository are dated 2026-07-12. Similar CPU ABI, DolRecomp,
and Dolphin concepts are explained by both projects' shared DolRecomp and
Dolphin upstream ancestry, not by gcnrecomp borrowing from ModernGekko.

## Values check

| Requirement | Finding | Decision |
|---|---|---|
| Real IPL is the LLE floor | The primary runtime boots extracted `sys/main.dol` through Dolphin. It does not execute the real IPL. | Reject |
| Static recompilation | It supplies a versioned native-module ABI and selects Dolphin's `StaticRecomp` CPU core. | Reference only |
| Permanent CPU fallback | The full-runtime wrapper accepts `allow_interpreter`, but still selects Dolphin's `StaticRecomp` core and the exact fallback behavior lives in its pinned submodule. The legacy runtime cannot run without a module; only after module dispatch rejects an address does its limited integer/branch interpreter try one instruction and fail on unsupported opcodes. | Not established |
| Deterministic guest timing | The legacy event queue orders equal-cycle events by monotonically increasing ID, which is a sound small mechanism. Its DI completes every transfer after one cycle and its modeled device set is far too shallow to establish GameCube timing fidelity. | Idea only |
| Hardware/device fidelity | The legacy runtime includes partial PI, DI, AI/DSP-DMA, controller, and GX code. It has no IPL path, EXI/RTC/SRAM, memory-card protocol, VI timing model, ARAM, or DSP LLE core. | Reject |
| Disc detection through the BIOS | Its launcher can find ISO/WBFS images, use Dolphin DiscIO to identify them, and extract their system/files trees. Detection and extraction are host-side; its runtime then boots extracted `main.dol`. This does not demonstrate the IPL detecting a disc through DI. | Does not solve |
| Byte-exact validation | Its tests are component/unit tests. No equivalent of our four XFB goldens, ordered MMIO oracle, or repeated derived-mode determinism gates was found. | Reject as evidence source |
| Maturity | The repository has two commits and its README explicitly labels it unfinished. | Reference only |

## What its two runtime paths actually do

The default, full runtime embeds a pinned RecompCore/Dolphin tree, selects
Dolphin's static-recompiler CPU core, and asks Dolphin to boot the extracted
`sys/main.dol`. Full Dolphin may provide device models absent from ModernGekko's
own code, but this wrapper still bypasses the exact thing gcnrecomp exists to
preserve: reset -> real BS1 -> real BS2/IPL -> hardware-visible boot behavior.

The standalone `LegacyRuntime` is much smaller, but it is a partial game-DOL
host rather than a BIOS runtime. It cannot execute without a native module; its
small interpreter is only a fallback after that module declines an address.
Its DI recognizes only the `0xA8` DMA read command and schedules completion one
guest cycle later. Its DSP-facing code models audio DMA and interrupt registers,
not execution of the real DSP ROM and coefficient ROM. Those simplifications
conflict with the project's rule that HLE may improve or augment a working LLE
floor, never replace it.

## Licensing and provenance

- Top-level `PROVENANCE.md` declares GPL-3.0-or-later and pins the
  `vendor/dolphin` RecompCore submodule to `7ddd35f37313236819cda798ae882abe1efcde74`.
  It identifies Dolphin as primarily GPL-2.0-or-later and DolRecomp as GPLv3.
- The repository contains the full GPLv3 license text. This is compatible with
  gcnrecomp's GPL-3.0 licensing posture.
- The checked-in `vendor/dolphin_legacy` tree predominantly has per-file SPDX
  tags (including GPL-2.0-or-later, CC0-1.0, BSD-3-Clause, and MIT), but no
  aggregate `COPYING`, `LICENSE`, `NOTICE`, `AUTHORS`, or equivalent file was
  found in that tree. The top-level provenance document describes the
  RecompCore submodule but does not inventory the origins and revisions of
  `dolphin_legacy`, `fmt`, or `picojson`. Compatibility is plausible, but its
  redistribution/provenance record is incomplete. Do not copy from those trees
  without resolving provenance file by file.
- `tools/moderngekko_port.cpp` hard-codes RecompCore revision `42a6bb23...` into
  its cache identity, while the repository and provenance document pin
  `7ddd35f3...`. Cache hits trust artifact existence, status checks compare only
  the DOL hash, and publication is not atomic. That inconsistency makes its
  generated-module provenance/cache metadata unsuitable to adopt unchanged.

## Narrow ideas worth retaining

1. **Versioned module descriptors.** Its ABI validates CPU-state size, game ID,
   entry point, and code/SMC/chunk range structure. It carries chunk hashes but
   only checks that the pointer is non-null; it does not recompute or compare
   them, nor bind the module to the inspected DOL hash. Our planned second-bank
   game/homebrew interface should independently use these categories and
   actually verify content hashes, without adopting its Dolphin runtime or ABI
   wholesale.
2. **Content-addressed build artifacts.** Hashing the input DOL plus toolchain,
   ABI, and generator identities is a useful packaging pattern. It must remain a
   build-time reproducibility guard, never a result-skipping runtime cache.
3. **Stable same-cycle event ordering.** `(due_cycle, insertion_id)` is a simple
   deterministic ordering rule. Our existing device scheduler remains the
   authority because it is already oracle- and golden-gated.

ModernGekko's own runtime code does not presently solve our performance
campaign, real IPL boot, calendar/RTC, memory cards, DSP LLE, or BIOS-visible
disc detection better than the code and evidence already in gcnrecomp. Its
production path delegates the broader device set to full Dolphin while still
bypassing the IPL, so it cannot substitute for that evidence.

## Primary sources

- <https://github.com/ExpansionPak/ModernGekko>
- <https://github.com/ExpansionPak/ModernGekko/blob/4b94e358098970bb6d0482cf383df560e463ed07/PROVENANCE.md>
- <https://github.com/ExpansionPak/ModernGekko/blob/4b94e358098970bb6d0482cf383df560e463ed07/src/runtime/dolphin_runtime.cpp>
- <https://github.com/ExpansionPak/ModernGekko/blob/4b94e358098970bb6d0482cf383df560e463ed07/src/runtime/legacy_runtime.cpp>
- <https://github.com/ExpansionPak/ModernGekko/blob/4b94e358098970bb6d0482cf383df560e463ed07/src/hardware/disc_interface.cpp>
- <https://github.com/ExpansionPak/ModernGekko/blob/4b94e358098970bb6d0482cf383df560e463ed07/include/moderngekko/module_abi.h>

---

# Delta re-audit addendum — 2026-08-31

Re-audited snapshot: ExpansionPak/ModernGekko `master` @
`5417826` (2026-08-23). The audited commit `4b94e358` **is** an ancestor of
master, so this is a genuine delta, not a fork or rewrite. Full findings and the
parallel DolRecomp assessment are in
[`UPSTREAM_SYNC_ASSESSMENT.md`](UPSTREAM_SYNC_ASSESSMENT.md).

## Decision: unchanged

**The reject-as-foundation/dependency/device-model-source decision stands, with
its original reasoning intact.** Do not revisit on maturity grounds alone.

## What actually changed

66 commits, 56 files, +9146/-588 since the audit. It is no longer the
"two commits, README says unfinished" repo the audit described:

- New subsystems under `src/`: `hardware/`, `gpu/`, `audio/`, `memory/`,
  `timing/`, `dolphin_shims/`. New tooling under `tools/` (netplay, PGO, DOL
  patching, cache affinity, an automation protocol).
- CI now builds and tests on three platforms (`.github/workflows/build.yml`).
- The README dropped its "Repo is not done" disclaimer and now documents a
  code-mod ABI plus a "Hall of Fame" of third-party recomps built on it
  (unverified third-party claims, not evidence this survey validated).
- Its lighter `LegacyRuntime` now owns real device objects (`AddressSpace`,
  `MmioBus`, `EventScheduler`, `ProcessorInterface`, `DiscInterface`,
  `AudioSystem`, GX command processor) instead of delegating everything to full
  Dolphin. This is a genuine architectural refinement.

## Why the decision survives it anyway

Every axis the rejection rested on is unchanged, re-verified at master:

| Audit finding | Status at `5417826` |
|---|---|
| Boots extracted `sys/main.dol` through Dolphin, not the real IPL | **Unchanged.** `src/runtime/dolphin_runtime.cpp:758-785` still builds `BootParameters::GenerateFromFile(main_dol)` and calls `BootManager::BootCore`. |
| No real-IPL path | **Unchanged.** No `ipl`/`bs1`/`bs2`/`bootrom` reference exists anywhere in its own sources. |
| No EXI / RTC / SRAM / memory-card / ARAM | **Unchanged.** No matches in ModernGekko-owned code. |
| No VI timing model | **Unchanged.** VI appears only as an interrupt-cause bit, `src/hardware/processor_interface.cpp:31`. |
| No DSP LLE | **Unchanged.** `src/audio/audio_system.cpp` models DSP MMIO control/interrupt registers only; no DSP ROM execution. |
| DI recognizes only `0xA8`, completes in 1 cycle | **Unchanged.** `src/hardware/disc_interface.cpp:96,103`. |
| HLE as baseline | **Unchanged.** `LegacyRuntime::Reset()` still bootstraps low memory via Dolphin's `DolphinBoot::SetupMemory` (`src/runtime/legacy_runtime.cpp:87`) — the fake-post-BS2 pattern `CLAUDE.md` names. |
| No byte-exact validation evidence | **Unchanged.** `tests/gx_state_test.cpp:89` asserts a mock device flag, not a golden framebuffer or hash. |

## Module ABI: no backport opportunity (audit item 1 closed)

The audit retained "versioned module descriptors" as an idea and noted
ModernGekko carried chunk hashes but never verified them. Its module ABI has
since bumped 2 → 3 (`include/moderngekko/module_abi.h:14`), but **the
verification gap is unchanged**: `src/runtime/module.cpp:30` still only checks
`chunk_hashes == NULL` and that ranges structurally tile. The only other
non-assert use folds the *self-reported* hash values into a netplay fingerprint
(`tools/netplay_compatibility.cpp:91`) — trust-on-receipt, not verification.

Our `runtime/src/aot_module.c:56-92` already implements the stronger design the
audit told us to build ourselves: real FNV-1a64 hashing over live guest RAM per
chunk, compared against the stored hash, with VERIFIED/FAILED/UNVERIFIED state
and write-invalidation (`:94-115`). No change warranted to `aot_module.c` or
`gen_title_module_tables.py` on this account.

## Provenance hygiene: did not net-improve

- `vendor/dolphin_legacy` still has no aggregate `LICENSE`/`COPYING`/`NOTICE`/
  `AUTHORS`; 681 of 700 files carry per-file SPDX tags.
- `fmt` and `picojson` origins are still not inventoried in `PROVENANCE.md`
  (zero matches), though both vendor their own license text.
- The hard-coded-revision defect the audit flagged in
  `tools/moderngekko_port.cpp` is **fixed in code** — its
  `RECOMPCORE_REVISION` now matches the pinned gitlink `55c7b023` exactly and
  has been bumped in lockstep since. But the defect **moved** rather than
  closed: `PROVENANCE.md` still names the retired
  `RecompCore-ModernGekko.git` / `main` @ `8b47e90b`, while `.gitmodules` points
  at `https://github.com/ExpansionPak/RecompCore.git` branch
  `moderngekko-vendor`. Same class of problem — documented facts not
  mechanically kept in sync with the pin — now document-vs-code instead of
  code-vs-code.

Note for future reference: `RecompCore-ModernGekko` no longer exists as a
repository (404). ModernGekko commit `048c426` folded it back into
`ExpansionPak/RecompCore` as branch `moderngekko-vendor`. The submodule is not
checked out in the survey clone, so DolRecomp's revision beneath it was **not
confirmed** — recorded as unknown rather than assumed.

## Newly worth retaining (ideas only, reimplement independently)

Consistent with the original audit's posture — retain the idea, write our own code:

4. **Cache-domain thread affinity.** `tools/cache_affinity.hpp` uses
   `GetLogicalProcessorInformationEx(RelationCache, ...)` to find the logical
   processors sharing the largest same-level cache and pins the process to that
   domain (opt-in). Header-only, Windows-only, no Dolphin includes, no HLE-boot
   assumption. Public Win32 technique, not creative expression worth copying —
   relevant to our own performance campaign.
5. **PGO profile content-hash and stale rejection.** `tools/pgo_support.*`
   derives a deterministic per-build workspace, merges `llvm-profdata` output,
   computes a `merged_profile_sha256`, and carries an explicit
   `reject_stale_profile` flag. PGO is our one codegen lever that has repeatedly
   measured a real win, and upstream DolRecomp independently built the same guard
   (`e07701f`, "Gate a PGO build against a stale profile instead of degrading
   silently"). Worth having as a build-reproducibility guard.

Everything else screened out: `tools/dol_patch.*` is per-title byte patching of
the HLE-boot artifact; `tools/automation_protocol.*` depends on Dolphin's
`InputOverrider.h` and duplicates our TCP debug surface; netplay tooling is
chassis-coupled and not a goal; the new `src/gpu`/`src/audio`/`src/hardware`
device models are device models, and still shallower than what we have.
