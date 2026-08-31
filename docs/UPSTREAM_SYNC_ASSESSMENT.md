# Upstream sync assessment — 2026-08-31

Question asked: what can gcnrecomp pull from upstream **without losing the
LLE-first work we put in**, what **contradicts** that work, and where have we
**done it better**.

Assessed in worktree `F:\Projects\_wt-gcnrecomp-upstream-audit`
(branch `audit/upstream-sync`, from `master` @ `d433b9a`).

## What "upstream" means here

| Upstream | Ref assessed | Relationship |
|---|---|---|
| `ExpansionPak/DolRecomp` | default branch **`main`** @ `1bec355` (2026-08-23) | our `recompiler/` ancestor |
| `mstan/DolRecomp` (our public fork) | `gcnrecomp` @ `e1015cf` (2026-07-14) | what `recompiler/` snapshots |
| fork base | `f3a129d5` (2026-07-01, "support physical pc aliases") | divergence point |
| `ExpansionPak/ModernGekko` | `master` @ `5417826` (2026-08-23) | re-audit of a prior reject |

Note: upstream's default branch is `main`, not `master` — `git merge up/master`
fails with "not something we can merge". `RecompCore-ModernGekko` is gone
(404); it was folded back into `ExpansionPak/RecompCore` branch
`moderngekko-vendor` by ModernGekko commit `048c426`.

## Divergence, measured

- **Upstream since base:** 72 commits, **75 files added, 0 removed.** New
  `src/ir/` (DolIR), `src/backend/llvm/` (~30 files), `src/backend/c_cfg.c`,
  `src/analysis/symbol_map.c`, `src/backend/{module_abi,symbols,variant_output}.c`,
  and 10 new tests.
- **Ours since base:** 14 fork commits (+2932/-365 across 26 files),
  concentrated in `src/backend/emitter.c` (+1288), `dispatch.c`,
  `ppc_cycles.c` (new), `frontend/container/ipl.c` (new).
- **Plus 3 commits local to gcnrecomp that never reached the public fork**
  (~370 lines): `7fb3d69` FP-unavailable exception, `0b4e42c` retail-title
  native-miss fallback, `7c1bb73` Wind Waker visual-parity tracing. Any
  re-vendoring must carry these forward explicitly — they are not in
  `mstan/DolRecomp`.

### The merge is far more tractable than the commit count suggests

Trial merge of `up/main` into fork tip `e1015cf` (throwaway clone, measured not
estimated):

- **75 new files land clean.** 9 files conflict, 33 hunks, ~2200 conflicted lines.
- `main.c`, `pipeline.c`, `cpu.c`, `test_jumptables.c` **auto-merge**.
- `emitter.c`: 19 hunks / 972 lines, but only ~5 are substantive. The largest
  (401 ours vs 1 theirs) is our poll-loop peephole sitting next to their
  chunk-table declaration — pure adjacency, resolved by keeping both.
- `dispatch.c`: 2 hunks / 409 lines — **this is the one real semantic
  collision** (see 2a).
- `CMakeLists.txt` / `README.md` conflicts are mechanical.

### Upstream's non-LLVM half builds and passes here

Verified by running it, not by reading CI config: upstream `1bec355` configured
and built with our exact `build.sh` toolchain (msys2 mingw64, Ninja,
RelWithDebInfo, `-DDOLRECOMP_ENABLE_LLVM=OFF`) and **ctest is 19/19 green**.
LLVM is `option(... OFF)` at `CMakeLists.txt:6`; with it off `find_package(LLVM)`
is never called, so there is no new hard dependency.

## Category 1 — FREE, and one of them is a live correctness bug

### 1a. The Gekko float pipeline (upstream `44f3fdb`) — highest priority

**We have this bug.** Our C backend inlines scalar FP as plain C:

- `recompiler/src/backend/emitter.c:1449` FADDS, `:1454` FSUBS, `:1459` FMULS,
  `:1464` FDIVS — `ctx->fpr[d] = (f64)(f32)(a OP b);`
- `:1491` FADD, `:1501` FMUL — plain double ops
- `:1559` FRSP — plain cast

That drops four hardware behaviours, none visible in the generated C:

1. **25-bit C-operand truncation on multiply.** The Gekko truncates a
   multiply's C operand to a 25-bit mantissa. Upstream's own test, which I
   built and ran here, quantifies the damage:
   `25-bit C operand : C full f64  12545/100000 differ (12.5%)`
   — 12.5% of full-mantissa multiplies produce a numerically wrong result.
   Not an edge case.
2. **`ps1` is never written on single-precision results.** Single-precision
   results occupy *both* halves of a paired-single register. The inline form
   leaves `ps1` holding whatever was there before, and `ps_*` then reads it:
   `ps1 : inline leaves it stale=yes, helper writes it=yes`.
3. **FPRF / FI / FR never updated** — `FPRF : inline 0x00, helper 0x04`.
4. **NaN / invalid-operation gating skipped.**

Scope check, in our favour: our FMADD family *does* route correctly through
`ppc_fma` (`emitter.c:1474-1484`) which applies `force_25_bit`
(`runtime/src/cpu_glue.c:764`) and writes `ps1`. The gap is confined to the
**non-FMA scalar ops** listed above.

**Why this matters right now:** paired-single is the Gekko's SIMD unit — it is
the path vertex and matrix maths take. Upstream's visible symptom on Skyward
Sword was *"character models losing limbs and bursting into stray triangles as
soon as they animated."* We have two open Wind Waker rendering beads whose
signatures rhyme with that: `beads-u2x.4` (model geometry
misplaced/exploded on the title screen, suspected skinning) and `beads-u2x.2`
(ocean renders flat solid blue in **both** render paths — i.e. upstream of the
renderer). **This is a candidate root cause for both and should be tested
before more GX-side investigation.** Treat that as a hypothesis to falsify, not
a conclusion: neither bead has been re-tested against this fix yet.

### 1b. `fcmp` — we are worse than the bug upstream fixed

Upstream fixed `ppc_fcmp` OR-ing into FPCC instead of replacing it (`2515808`).
We can't inherit that bug because **we never had `ppc_fcmp` at all** — base
`f3a129d5` didn't have it, and our `emit_fcompare`
(`recompiler/src/backend/emitter.c:73-87`) inlines the compare and writes
**only CR**:

- **FPSCR[FPCC]/FPRF is never updated by a compare.** Anything reading FPSCR
  after a compare (`mffs`, `mcrfs`) sees stale bits.
- **FCMPO and FCMPU are identical** — no `ordered` flag, so no VXVC on an
  ordered NaN compare and no VXSNAN. Verified upstream models both:
  `fcmpo vs fcmpu : VXVC ordered=0x00080000 unordered=0x00000000`.

Upstream's `ppc_fcmp` (`src/cpu/cpu.c:1321-1345`) handles all of it.

### 1b-note. The fix is a three-commit cluster, not one commit

Take them together: `44f3fdb` (re-route emission, emitter-only), `19ddaf5`
("fix emitted float semantics", +169 in `cpu.c`, −75 in `emitter.c`, plus test
growth), and `2515808` (the FPCC replace-not-OR fix). `2938368` / `0f0c995`
follow up on keeping the C backend independent of the inline FP helper.

There are **no free decoder fixes**: the only commits touching `src/frontend`
or `src/analysis` since base are LLVM plumbing (`f0a86be`, `789240f`,
`3606fbd`) and `2595de5` (code-mod support).

But the float cluster is **not** the whole of the free correctness work — see
1f, which was found only by reading a commit whose title says "refactor".

### 1f. `lwarx`/`stwcx.` reservations ignore the MEM1 cached/uncached alias bit

Hidden inside upstream `93b881c`, titled *"optimize C backend and split LLVM
backend"*:

```c
 static void clear_matching_reservation(CPUState* cpu, u32 addr) {
-    if (cpu->reserve_valid && ((cpu->reserve_addr ^ addr) & ~31u) == 0)
+    const u32 reserve_addr = cpu->reserve_addr & ~0x40000000u;
+    const u32 store_addr = addr & ~0x40000000u;
+    if (cpu->reserve_valid && ((reserve_addr ^ store_addr) & ~31u) == 0)
         cpu->reserve_valid = false;
 }
```

A reservation is held on a **physical** address, but we compare raw effective
addresses. MEM1 is visible through two aliases differing by exactly one bit —
cached `0x80xxxxxx` and uncached `0xC0xxxxxx`, and
`0x80000000 ^ 0xC0000000 == 0x40000000`. So a `lwarx` through one alias
followed by a store to the same physical word through the other **fails to
clear the reservation**, and the following `stwcx.` succeeds where hardware
fails it. That is an atomic primitive failing silently — lock queues and
interrupt-safe list manipulation, which IPL/OS code uses.

**We have it at 15 comparison sites across 4 locations**, and the two that
actually ship are in `runtime/`:

- `runtime/src/memory.c:278-281` — the bus primitive generated C calls; its own
  comment calls it a "mirror of recompiler cpu.c"
- `runtime/src/cpu_glue.c:434-436` — the `dcbz_l` path
- `recompiler/src/cpu/cpu.c:126-128`
- `recompiler/src/backend/emitter.c:806-884` — **12 inlined copies** emitted
  into the generated fast-path stores

Filed as `beads-u2x.9`. Note the emitter repeats the compare as a raw string 12
times, so the fix shape is one shared emitted helper, not 12 edited string
literals.

**Method lesson worth keeping:** I scanned the same 72-commit list and skipped
`93b881c` on its title. Title-based triage of upstream history is unsafe — the
commit that carried a CPU-semantics fix advertised itself as a refactor.

### 1c. Two landing sites, not one — the trap to design for

Our runtime **reimplements** the DolRecomp cpu.c ABI in
`runtime/src/cpu_glue.c` rather than linking `recompiler/src/cpu/cpu.c` (stated
at `runtime/src/cpu_glue.c:1-25`). So **every CPU-semantics take must land
twice**, and a fix applied only to `recompiler/` is inert in the shipped
runtime.

Good news: the machinery is already on our runtime side —
`set_fp_exception` (`cpu_glue.c:643`), `set_fprf` (`:682`),
`force_25_bit` (`:764`), `classify_f32`, `ppc_fma` (`:783`). Adopting 1a/1b is
mostly **re-routing emission + adding the missing helpers**, not porting a
numeric core.

### 1d. Free regression tests (backend-agnostic, verified green here)

- `tests/test_fpscr.c` — links `dr_cpu` only. Pins FPCC replace-not-OR,
  fcmpo/fcmpu VXVC distinction, FPRF C-bit preservation, VE-suppressed
  invalid-add, ZE-suppressed div-by-zero, FI/FR/XX on inexact.
- `tests/test_float_semantics.c` — links `dr_cpu` only. Pins the 12545/100000
  divergence exactly, and the `ps1`-staleness → `ps_add` ps1-lane corruption chain.
- `tests/test_c_execute.c` + an additive diff to `test_codegen_emit.c` —
  end-to-end: emit C from PPC words, compile it, execute it, assert
  registers/FPSCR/CR. This is the one that earns its keep (see the
  implementation postscript).

  **Correction:** an earlier draft of this document claimed our
  `test_codegen_emit.c` was "already byte-identical to upstream's pre-change
  version". That was wrong — it matches upstream `5d1ee26`, five commits behind
  the real pre-change point `93b881c`, so it was also missing `native_loop`,
  `local_call` and `memory_loop`. The port was larger than one commit's worth of
  fixtures.

### 1e. Standalone tooling, free but not yet actionable

`src/analysis/symbol_map.c` + `src/backend/symbols.c` — CodeWarrior `.map`
parsing → sanitized C symbol defines, with dedup/sort/unaligned-reject and size
inference from the next entry. No LLVM dependency, no equivalent in our tree.
Useful **if/when** we have a `.map`; we have none for the IPL today.

## Category 2 — CONTRADICTS, or needs a judgment call

### 2a. `dispatch.c` — both of us built O(1) dispatch independently

Ours (`e1a7e92`) collapses maximal runs of uniform-size, perfectly-tiling
functions into a table indexed by `(address-base)/stride`; isolated functions
keep a range check. Tuned for the IPL's ~156 contiguous `0x4000` chunks, and
degrades to the old chain in the worst case.

Upstream (`2f00fd7`, `a2b02e5`) does uniform runs **plus a page-index table**
(`DISPATCH_PAGE_SHIFT`) mapping address pages to runs, so lookup is O(1)
*across* runs rather than O(#runs); it also tolerates one trailing short chunk
per run and warns + falls back to linear on overlapping ranges.

**Upstream's looks strictly more general.** For a single-run image (the IPL)
ours is equivalent; for multi-section retail DOLs — which is exactly where
`0b4e42c` took us — theirs should win. Recommendation: take upstream's, then
re-verify our IPL dispatch numbers rather than assuming parity.

### 2b. Cross-chunk direct calls vs our bl/blr shadow path

Upstream `98f77b6`/`cd9c92d`/`0a1946b` emit direct C calls across chunk
boundaries (new `emit_cross_chunk_call`, a chunk-starts table, a shared
`dolrecomp_call_depth` guard, and `DOLRECOMP_NO_DIRECT_CALLS` to opt out); it
also changed `emit_direct_branch`'s signature and `emit_function`'s return type
(`void` → `bool`). Ours (`18c6bc8` / repo `355046f`) keeps `bl`/`blr` inside the
chunk via a shadow path plus inlining budgets. Same goal, different mechanism,
and they touch the same functions. **Needs a deliberate head-to-head; do not
merge both blindly.**

### 2c. `c_cfg.c` native-loop outlining — an optimization we lack, not a bug we have

Upstream restructures the C backend's CFG (loop outlining, direct back-edges,
routed local returns) and its test pins a real hazard: a loop containing `mftb`
must **not** stay a native C loop or the timebase read executes once.

That hazard is a property of *their* optimization. We don't have it: our
backward branches are `goto`-to-label so instruction bodies re-execute, and our
only generated native loop is the narrow poll-loop peephole
(`emitter.c:418`) — restricted to `lwz`/`cmplwi`/`beq`, EA computed once,
`__atomic_load_n` so device writes are observed, and deadline-bounded.

So this is a **perf option with high collision cost against our Phase A-D
emitter work**, not a correctness debt. Deprioritize relative to Category 1.

### 2d. `CPUState` has diverged structurally — this is the deep one

Upstream HEAD changed `struct CPUState` (`src/cpu/cpu.h`, 97 lines changed,
`cpu.c` 617):

- **removed** the flat `u32 spr[1024]` array (which our fork still has),
  replacing SPR access with `spr_read`/`spr_write`/`cache_control` callbacks
  plus `external_pointer`;
- **added** `s64 downcount; s64 cycle_budget;`;
- added an `exram`/`mem2` union for field-name aliasing.

Our fork instead appended `u64 cycles; u64 cycle_deadline;` at the tail per our
own trailing-runtime-only-fields contract, and our runtime owns its own
`CPUState` by policy. This is the root of the semantic `cpu.h` merge conflict
and it **gates the LLVM backend**, which reads state through ~50 hardcoded
`offsetof(CPUState, field)` sites.

**Split the ripple — our ABI contract is by field name, not byte layout**
(`runtime/include/cpu/cpu.h:20-23`: generated code "only names fields, never
assumes sizeof or trailing layout"). So what matters is only what the *C
emitter emits*:

- **The SPR restructuring is a non-issue for us.** `spr_read`, `spr_write`,
  `cache_control` and `external_pointer` are **never named by the C emitter** —
  they are cpu.c- and LLVM-internal. We can decline that change and keep
  `spr[1024]`; our 22 `spr[...]` uses across `cpu_glue.c`, `debug_server.c` and
  `dispatch.c` are safe.
- **`downcount` is a real collision, and it is the deepest one in the tree.**
  Upstream's C emitter emits it throughout: `emitter.c:1947` charges
  `ctx->downcount -= block_cycles[i]` per basic block in the main emission
  loop, `:1958` yields on `downcount <= -DOLRECOMP_C_LOOP_CYCLE_BUDGET`,
  `:387`/`:401` emit that same yield check on every local backward branch
  inside the general `emit_direct_branch`, and `:1892` does it again in the
  outlined-loop path.

That is **a second, complete cycle-accounting and yield architecture**, sitting
in exactly the emitter regions where we built ours (`dr_cycles`,
`dr_deadline`, `DR_RET`, Phase A coalesced per-basic-block charging, and
`ppc_cycles.c` derived per-instruction Dolphin-model costs with lazy DSP flush).
The two are mutually exclusive, and its per-block costs come from `c_cfg`'s
`block_cycles` — so declining `c_cfg` (2c) does **not** cleanly decline
`downcount`; they are entangled.

**Ours should win.** Theirs is a fixed cycle budget; ours is oracle-gated
derived cycle accuracy modeled on Dolphin's per-instruction costs. Adopting
theirs would be a fidelity regression, not a gain.

**Consequence for sequencing:** a blind full merge is *not* the cheap route to
the Category 1 fixes. It drags in an architecture fight whose resolution is
"keep ours" anyway. See the revised order below.

### 2e. FP-unavailable — convergent, and each side has the better half

Base had no FP-unavailable modeling. Both sides added it **independently,
within days**: upstream in `3606fbd` (2026-07-14 21:08), ours in `7fb3d69`
(2026-07-16) — we forked at 2026-07-14 19:52, hours before theirs landed. Ours
came out of a real bug (IPL menu garble: the audio/DSP-mail handler's
paired-single work running on live FP registers).

The constants differ (`PPC_EXC_FP_UNAVAILABLE` 0x40 + `PPC_VECTOR_FP_UNAVAIL`
vs theirs 0x20 + `PPC_VECTOR_FP_UNAVAILABLE`), so the `cpu.h` conflict is
load-bearing, not cosmetic.

- **Theirs is better on cost:** `ppc_fp_available_inline` (`cpu.h:189-193`)
  avoids an out-of-line call per FP site — they measured `ppc_fp_available` at
  ~7.1% self time on Mario Kart: Double Dash with 121,874 FP sites.
- **Ours is better on fidelity:** our `psq_check_enabled`
  (`runtime/src/cpu_glue.c:344-355`) ranks MSR[FP]=0 **above** the HID2
  PSE/LSQE program check, matching hardware exception priority. Upstream's
  (`src/cpu/cpu.c:738-744`) does only the HID2 check and relies on the
  emitter's site gate.

Best merge: take their inline form, keep our priority ordering and SRR0
re-execution semantics.

## Category 3 — where we are ahead

- **AOT module content verification.** `runtime/src/aot_module.c:56-92`
  actually recomputes FNV-1a64 over live guest RAM per chunk and compares
  against the stored hash, with VERIFIED/FAILED/UNVERIFIED state and
  write-invalidation (`:94-115`). Upstream's `src/backend/module_abi.c` does
  **zero** hashing (it is a host-ISA-variant selector; only checks
  `cpu_state_size`). ModernGekko carries `chunk_hashes` but never verifies them
  — its only non-assert use folds the *self-reported* values into a netplay
  fingerprint (`tools/netplay_compatibility.cpp:91`). Ours is the strongest of
  the three; the `THIRD_PARTY_NOTICES.md` claim to that effect checks out.
- **FP-unavailable psq exception priority** (2e) — a genuine upstream
  contribution candidate.
- **Ours alone:** real-IPL container support (`frontend/container/ipl.c`),
  derived cycle accuracy (`ppc_cycles.c`), poll-loop deadline yield, MEM1
  inline fast path, bl/blr shadow, and the whole LLE runtime.

## The LLVM backend — worth knowing about, not worth adopting yet

- **Opt-in and isolated.** DolIR is *not* a mandatory new layer: the C backend
  never includes `ir/`, and `dr_backend` does not link `dr_ir`. Backend
  selection is one branch in `pipeline.c:1144-1160`; default is C
  (`cli.c:173`).
- **Best documented claim: ~0.958x of a PGO'd C build** — but be skeptical.
  There is **no results document at HEAD**; it was replaced by two screenshots
  (`e2fac49`) with the numbers moved to an off-repo PR description. Recovered
  from history, the figure is Mario Kart: Double Dash, **one savestate, one
  Windows host**, and the authors state plainly it *"was not re-measured against
  this exact commit series."* The +73.4% headline is IR-level instrumentation
  PGO on a single scene; their split-screen workload lands ~6% short.
- **Cost is real:** LLVM 19-or-20 pinned (`CMakeLists.txt:15-17`), MinGW needs
  LLVM's shared import library, and the ~50 `offsetof(CPUState, ...)` sites must
  be reconciled against 2d before any of it can be trusted.
- **Our workload is the wrong shape for their evidence.** The IPL/boot path is
  branch- and syscall-heavy, not a steady-state racing inner loop. And
  single-scene PGO profiles sit uncomfortably close to a per-game hint, which
  PRINCIPLES.md rules out for the foundation.

Verdict: **not now.** Revisit after Category 1 lands and 2d is resolved, and
only with a measurement on our own workload.

## ModernGekko re-audit — the reject stands

Re-audited `4b94e358` (audit) → `5417826`: **66 commits, 56 files, +9146/-588**,
CI on three platforms, a code-mod ABI, and a README "Hall of Fame" of
third-party recomps. It is no longer a two-commit unfinished repo.

None of that moves the axis the rejection was about. Re-verified at master:

- Still boots extracted `sys/main.dol` through Dolphin's `BootManager`
  (`src/runtime/dolphin_runtime.cpp:758-785`); **no IPL/BS1/BS2 path exists
  anywhere** in its own sources.
- Still no EXI, RTC, SRAM, memory-card protocol, or ARAM in ModernGekko-owned
  code. VI is one interrupt-cause bit. Audio models DSP MMIO only — no DSP LLE.
- DI still recognizes only command `0xA8` and completes after 1 cycle
  (`src/hardware/disc_interface.cpp:96,103`).
- Its lighter `LegacyRuntime` now owns real device objects (a genuine
  refinement) but still cannot run without a native module and still bootstraps
  low memory via Dolphin's `DolphinBoot::SetupMemory` — exactly the
  fake-post-BS2 pattern CLAUDE.md names.
- Provenance hygiene did not net-improve: `vendor/dolphin_legacy` still has no
  aggregate license (681/700 files carry SPDX), `fmt`/`picojson` are still
  un-inventoried, and `PROVENANCE.md` is now itself stale — it names the retired
  `RecompCore-ModernGekko.git`/`main` @ `8b47e90b` while `.gitmodules` points at
  `RecompCore.git` branch `moderngekko-vendor` @ `55c7b023`. The hard-coded
  revision bug moved from code-vs-code to document-vs-code.

**Two ideas worth independently reimplementing** (not importing):

1. **Cache-domain thread affinity** (`tools/cache_affinity.hpp`) — derive the
   logical processors sharing the largest same-level cache and pin to that
   domain. Public Win32 API technique; relevant to our perf campaign.
2. **PGO profile content-hash + stale rejection** (`tools/pgo_support.*`) —
   hash the merged profile and refuse a stale one. PGO is our one working
   codegen lever, and upstream DolRecomp independently did the same thing
   (`e07701f`). Worth having as a build-reproducibility guard.

## Recommended order

Revised after finding the `downcount` collision (2d). The guiding rule is
**take what is better or neutral, decline what would regress us** — not
"import the maximum number of upstream commits."

**Stage 1 — the correctness pass (do this first, as one unit).**
Port, do not merge: the float cluster (`44f3fdb` + `19ddaf5` + `2515808`,
`beads-u2x.6`, P0) and the reservation alias mask (`93b881c`'s fix,
`beads-u2x.9`, P1). These are genuinely separable — they concern *which helper
computes a value*, not how cycles are charged, so they carry none of the
`downcount`/`c_cfg` entanglement. They land in the same four files
(`recompiler/src/cpu/cpu.c`, `recompiler/src/backend/emitter.c`,
`runtime/src/cpu_glue.c`, `runtime/src/memory.c`), so one regen + validation
cycle covers both. Gate with `test_fpscr.c` + `test_float_semantics.c` + the
`test_c_execute.c` port (1d), which build against `dr_cpu` alone and are already
verified green here.

**Stage 2 — test the Wind Waker hypothesis immediately.** With Stage 1 in,
re-run the title screen and the ocean and record the result **either way**.
If the FP fix explains `beads-u2x.4` / `beads-u2x.2`, it redirects a whole line
of GX-side investigation; if it doesn't, that disproof belongs in both beads
before anyone re-derives it.

**Stage 3 — the re-vendor decision (`beads-u2x.7`), now informed.** With
Stage 1 already banked, the merge's remaining value is dispatch (2a),
cross-chunk calls (2b), the symbol-map tooling (1e) and staying close to
upstream. Its remaining cost is the `downcount` reconciliation, which must
resolve as "keep our cycle architecture." Do it as an explicit merge with that
resolution rule written down in advance, not hunk-by-hunk improvisation. Carry
the 3 fork-absent local commits forward.

**Stage 4 — measure, don't assume.** Dispatch head-to-head on our own images
(`beads-u2x.8`; may legitimately be a null for the single-run IPL), then
cross-chunk calls. Under the standing bench protocol: runaway check first, no
device-kill A/B.

**Stage 5 — `CPUState` for LLVM only.** The SPR half is a non-issue for the C
backend (2d); the `offsetof` reconciliation only matters if LLVM is ever
adopted, which needs a measurement on our workload first.

**Offer upstream:** our psq FP-unavailable exception priority ordering (2e).

### Still open (work, not decisions)

- Does routing FP through helpers cost throughput? Upstream measured the FP
  gate at ~7.1% self time when called per site and answered it with an
  inlinable form (`79534d8`). If re-routing measures a real loss, take that
  same approach — emit an inlinable helper — rather than keeping wrong
  arithmetic.
- Is `~0x40000000` the complete alias story for *our* address map? Check
  against the configured RAM/MMIO windows before hardcoding upstream's constant.
- `runtime/src/interpreter.c:689-690` implements `stwcx.` by checking
  `reserve_valid` alone, never comparing the address — inconsistent with the
  recompiled path. Justify in a comment or align.

## Method note

Every measurement in this document was produced in this session: the trial merge
was run in a throwaway clone, upstream `1bec355` was configured/built/tested
locally with our own toolchain, and the float-divergence figures are the stdout
of upstream's `test_float_semantics` binary compiled here. Subagent surveys were
used for breadth; their load-bearing claims were re-verified against the code
before being written down.

---

## Implementation postscript — Stage 1, 2026-08-31

Stage 1 (Category 1a/1b/1f + the tests) is implemented on branch
`fix/gekko-fp-and-reservations`. **Recompiler suite: 16/16 green**, up from 13
registered tests with 3 failing on `master`.

Our ported helpers are numerically identical to upstream's — `test_float_semantics`
reproduces upstream's pinned count exactly against our own `cpu.c`:

```
25-bit C operand : C from lfs   0/100000 differ (0.0%)
                 : C full f64   12545/100000 differ (12.5%)
ps1              : inline leaves it stale=yes, helper writes it=yes
FPRF             : inline 0x00, helper 0x04
fcmpo vs fcmpu   : VXVC ordered=0x00080000 unordered=0x00000000
ps_* consumer    : ps0 13 (want 13), ps1 22 (want 22)
```

### Four bugs found during implementation that the assessment had not predicted

The assessment predicted the float and reservation defects. Implementing them
surfaced four more, three of them ours:

1. **`7c1bb73` broke the recompiler's own codegen gate.** The emitter emitted
   `#include "debug/rings.h"` unconditionally, but generated C must also compile
   standalone against the recompiler include path — that is what
   `codegen_compile`, `bl_shadow_exactness` and `poll_loop_exactness` do. Three
   tests had been red on `master` since that commit. Fixed: the include sits
   behind `DOLRECOMP_HOST_RINGS`, with a no-op inline fallback and the call sites
   byte-identical in both configurations, so the codegen tests keep testing what
   ships. The runtime opts in via `target_compile_definitions(gcn_boot ...)`.
2. **`ppc_dcbz` and `ppc_icbi` were declared and called but never implemented in
   the recompiler's reference host** — only in `runtime/src/cpu_glue.c`. So the
   recompiler could not link its own generated output. This was invisible
   *because* of bug 1: the codegen tests never reached the link step. Fixed with
   reference implementations in `recompiler/src/cpu/cpu.c`.
3. **`ps_merge` rounded through f32.** It is a pure lane move; ours called
   `dolrecomp_ps_round` on each lane, destroying the low mantissa bits of any
   double that is not single-representable — live corruption on the
   vertex/matrix path. It also wrote `fpr[rD]` before reading `rB`, so a merge
   with `rD == rB` read back the value it had just written. Both fixed by
   porting upstream's `emit_ps_merge` (verbatim moves via temporaries).
4. **Paired-single compares had the same FPSCR gap as scalar `fcmp`.**
   `ps_cmpu0/o0/u1/o1` wrote only CR, never FPSCR[FPCC]/FPRF, collapsed the
   ordered/unordered distinction (so `ps_cmpo*` never signalled VXVC on a NaN),
   and truncated both operands to f32 before comparing. Routed through
   `ppc_fcmp` with bank and ordered selection.

**Bugs 3 and 4 were caught by the ported `c_execute` test on its first run** —
its "compare" fixture is `ps_cmpo0`, not `fcmpo`, and its merge fixture
deliberately uses `0x1.0000000000001p+0`, a double chosen to be non-single-representable.
That is the clearest possible argument for adopting upstream's tests as a shared
contract: they immediately found defects in our code that no upstream commit
mentions.

### Scope actually delivered

Beyond the ten scalar helpers, the paired-single **binary** ops
(`ps_add/sub/mul/div`) were also ported and routed — `ps_mul` now applies the
Gekko 25-bit C-operand truncation on **both** lanes, which the inline form
skipped. Still inline and pending a later pass: `ps_madd*`, `ps_sum*`,
`ps_muls*`, `ps_res`, `ps_rsqrte`, `ps_sel`, and `ps_neg/abs/nabs/mr`.

The reservation fix landed at all 15 comparison sites, consolidated to **one**
implementation per tree rather than the copies it replaced:
`gcn_reservation_key()` / `gcn_reservation_hit()` in
`runtime/include/memory/memory.h` (shared by `memory.c` and `cpu_glue.c`),
`reservation_key()` in `recompiler/src/cpu/cpu.c`, and
`dolrecomp_reservation_key()` emitted once into the generated header instead of
twelve duplicated string literals.

### Still outstanding for Stage 1

- **The Wind Waker hypothesis is UNTESTED.** Nothing here has been run through a
  regen + IPL/title boot yet, so there is no evidence for or against
  `beads-u2x.2` / `beads-u2x.4`. That is Stage 2 and it needs the regen chain
  (`_work/regen_chain.sh`).
- No throughput measurement yet on routing FP through out-of-line helpers.
- `beads-u2x.10` (the `gcn_runtime_core` link break, also from `7c1bb73`) is
  still open and still blocks the runtime unit-test suite.
