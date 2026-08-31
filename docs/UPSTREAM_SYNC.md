# Staying syncable with upstream DolRecomp

Goal: make "take the new upstream work" a routine, cheap operation rather than
an archaeology project. The 2026-08-31 assessment
([`UPSTREAM_SYNC_ASSESSMENT.md`](UPSTREAM_SYNC_ASSESSMENT.md)) cost a full
session largely because our fork had drifted as an opaque snapshot for six
weeks. This document is how we stop paying that repeatedly.

## The model: a rebasable topic series, not a snapshot

`recompiler/` is a vendored snapshot of `mstan/DolRecomp`. Treat that public
fork as **a linear series of small, topical commits on top of upstream `main`**,
and re-establish it by **rebase** on each sync:

```
git remote add up https://github.com/ExpansionPak/DolRecomp.git
git fetch up 'refs/heads/*:refs/remotes/up/*'     # note: default branch is main
git rebase --onto up/main <old-base> gcnrecomp
```

Rebase, not merge, because the fork's value is *our patch series*, and we want
each of our changes to keep its own identity and rationale as upstream moves.
A merge history hides which of our commits still applies.

> Upstream's default branch is **`main`**. `git merge up/master` fails with
> "not something we can merge" — that is the symptom, not a broken remote.

### Keep the series clean

- One topic per commit, with the *why* in the message. A commit whose diff
  spans the IPL container, cycle accounting and the emitter cannot be
  individually kept or dropped during a rebase.
- Never reformat upstream code, ever. Reformatting turns a zero-conflict hunk
  into a guaranteed conflict for no benefit.
- **Port upstream code as close to verbatim as practical, comments included.**
  Matching text is what makes a future rebase a no-op instead of a conflict.
  This is a real constraint on how we write ports, and it outranks local style
  preference. (Applied in the Gekko float-helper port: upstream's helper names,
  signatures, structure and comments were kept.)
- Anything local to gcnrecomp that is *not* in the public fork must be
  identified as such. The 2026-08-31 assessment found 3 such commits
  (`7fb3d69`, `0b4e42c`, `7c1bb73`) that a naive re-vendor would have silently
  dropped.

## Reduce our footprint inside upstream's hot files

This is the single biggest lever, and it is mostly unexercised.

Our +1288 lines in `recompiler/src/backend/emitter.c` collide with upstream's
emitter work on *every* sync. `frontend/container/ipl.c` — a whole new
container in its own file with one registration point — has never conflicted
with anything. That contrast is the design rule:

**Put our additions in their own translation unit and hook them in at one
call site.** For each candidate below, the win is that upstream's version of
`emitter.c` stays nearly theirs, so it rebases clean:

| Ours, currently inside `emitter.c` | Extractable? |
|---|---|
| Poll-loop peephole (`is_lwz_cmpli_beq_poll`, `emit_lwz_cmpli_beq_poll`) | Yes — self-contained predicate + emitter, two call sites. Best first candidate. |
| Reservation-key helpers in the emitted header | Partly — the emitted text is a block, could live in its own emitted-header module |
| Cycle charging / deadline yield (Phase A) | Hard — genuinely interleaved with branch emission; see the divergence register |
| MEM1 inline fast path (Phase C) | Partly — the emitted helper text is separable from the call-site selection |
| bl/blr shadow path (Phase D) | Hard — interleaved with `emit_direct_branch` |

Do the easy ones opportunistically whenever a sync makes us touch that code
anyway. Do not do a big-bang refactor for its own sake.

## The deliberate-divergence register

The expensive question during a rebase is "did we mean to differ here?" Answer
it in advance. **Every intentional divergence from upstream goes in this table**,
with the reason, so a future rebase resolves it in seconds instead of
re-litigating it.

| Area | Upstream | Ours | Why ours stays |
|---|---|---|---|
| Cycle accounting / yield | `ctx->downcount` charged per block from `c_cfg`'s `block_cycles`, yield on a fixed `DOLRECOMP_C_LOOP_CYCLE_BUDGET` (`emitter.c:387,401,1892,1947,1958`) | `dr_cycles` / `dr_deadline` / `DR_RET`, Phase A coalesced per-block charging, `ppc_cycles.c` derived per-instruction Dolphin-model costs, lazy DSP flush | Ours is oracle-gated derived cycle accuracy; theirs is a fixed budget. Taking theirs is a fidelity regression. **Resolve every `downcount` hunk in our favour.** |
| SPR storage | `spr_read`/`spr_write`/`cache_control` callbacks; no `spr[1024]` | flat `u32 spr[1024]` | The C emitter never names their callbacks — it is cpu.c/LLVM-internal. Our 22 `spr[...]` uses stay valid. Decline the restructuring. |
| `psq` exception priority | `psq_check_enabled` does only the HID2 PSE/LSQE program check | MSR[FP]=0 ranked **above** the HID2 check (`runtime/src/cpu_glue.c:344-355`) | Matches Gekko exception priority. **Offer this upstream.** |
| `lwarx` reservation aliasing | masks bit `0x40000000` only (cached/uncached pair) | canonicalizes to a RAM offset across all three windows — cached, uncached, and the bare physical window below `GC_MAIN_RAM_SIZE` | Our address map accepts the bare physical window, which their single-bit mask does not fold. **Offer this upstream.** |
| `c_cfg` native-loop outlining | loop outlining, direct back-edges, routed local returns | not adopted | It is a perf option, not a correctness fix, and it collides with Phase A-D. Its `mftb`-in-native-loop hazard does not exist for us: our backward branches are `goto`-to-label so bodies re-execute. |
| CPU ABI implementation | generated code links `src/cpu/cpu.c` | runtime **reimplements** the ABI in `runtime/src/cpu_glue.c` + `memory.c` | Deliberate: the runtime is a standalone host of the contract (as reshine is). **Consequence: every CPU-semantics take lands TWICE.** See below. |
| Dispatch | uniform runs **plus** a page index across runs (`DISPATCH_PAGE_SHIFT`) | uniform-run tables with a pow2 shift fast path, chained across runs | **Unresolved** — theirs is more general for multi-section retail DOLs, ours has a shift fast path. Tracked in `beads-u2x.8`; measure before choosing. |

## The two-landing-sites rule

Because our runtime reimplements the CPU ABI, **any change to CPU semantics
must land in both trees or it does nothing**:

- `recompiler/src/cpu/cpu.h` + `recompiler/src/cpu/cpu.c`
- `runtime/include/cpu/cpu.h` + `runtime/src/cpu_glue.c` (and `memory.c` for
  bus-side behaviour)

Generated code links the **runtime**. A fix applied only under `recompiler/`
compiles, passes the recompiler's own tests, and is inert in the shipped
emulator. This has already bitten twice — the reservation-alias bug is present
in both trees, and the float helpers had to be added to both.

When you touch one, grep the other before you commit.

## Adopt upstream's tests as the shared contract

Upstream's `dr_cpu`-only tests are backend-agnostic and cost nothing to keep:
`test_fpscr.c`, `test_float_semantics.c`, and the end-to-end `test_c_execute.c`.
Keeping them green against *our* `cpu.c` **and** mirroring them against
`cpu_glue.c` is the cheapest available detector of silent divergence — it is
how a future upstream semantics fix announces itself as a test failure rather
than as a rendering artifact six weeks later.

`test_float_semantics.c` pins an exact divergence count (12545/100000). If that
number moves after a sync, our helper and upstream's differ — that is a
finding, never a constant to retune.

## Sync checklist

1. `git fetch up 'refs/heads/*:refs/remotes/up/*'`; read
   `git log --oneline --no-merges <old-base>..up/main`.
2. **Do not triage by commit title.** The 2026-08-31 assessment nearly missed a
   real atomics bug because it shipped inside a commit called "optimize C
   backend and split LLVM backend". Read every diff that touches `src/cpu/`,
   `src/backend/emitter.c`, `src/backend/dispatch.c`, or `src/frontend/`.
3. Rebase our series onto `up/main`; resolve using the divergence register.
4. Land any CPU-semantics change in **both** trees.
5. Build both configurations (Vulkan and `GCN_VULKAN=OFF`), run the recompiler
   ctest under the MinGW runner, then run the regen chain
   (`_work/regen_chain.sh` — `generate.sh` alone produces a bootstrap-only tree
   and nukes the good 4-bank tree; the correct order is generate → build →
   generate_postdma → build → generate_bs1 → build, final tree 158 chunks).
6. Update `recompiler/UPSTREAM.md` with the new pins, and this file's
   divergence register with anything new.
