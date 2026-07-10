# Dynamically-Loaded & Relocated Code (gcnrecomp)

How gcnrecomp handles code that **appears at runtime and can share a memory
address with different content over time** — the class psxrecomp calls
"overlays." This doc distills psxrecomp's shipping implementation (read from the
**source**, which is authoritative over its design docs — several of which have
drifted; drifts flagged in §9) and lays out the gcnrecomp adaptation (§10).

Sources under `F:\Projects\psxrecomp\psxrecomp\`: `runtime/src/overlay_loader.c`
(dispatch/candidate table/validation/registration, ~2150 lines),
`overlay_capture.c`, `code_provider.c` + `overlay_backend.c` (backend seam),
`autocompile.c` (the LIVE async compiler), `dirty_ram_interp.c` (the fallback
interpreter), `memory.c` (dirty-page + self-mod hooks), `psx_icache.c`
(timing-only), `overlay_compile_worker.c` + `overlay_sljit.c` (the *deprecated*
sljit tier), `tools/compile_overlays.py` (offline compile/ingest).

---

## 1. The problem in gcnrecomp terms

The recompiled IPL boots in two stages (oracle-confirmed, docs/ROADMAP.md M0):

- **Stage 1 (loader):** `descrambled[0x100…]` at `0x81200000`. Statically
  recompiled; locksteps with Dolphin through the entire captured boot.
- **Stage 2 (menu):** stage 1 **DMA-copies `descrambled[0x820…]` from the EXI
  mask-ROM into `0x81300000`** and jumps there. At runtime `MEM1[0x81300000]`
  therefore holds *different bytes* than the statically-recompiled image had at
  that address, so our `func_81300000` is stale → executes garbage → alignment
  fault. This is the dynamically-loaded-code problem.

Two ways to be correct here, and we want **both** (per the LLE-first +
completeness discipline):

1. **Modify-before-recomp / at rest (static, "Layer B"):** we can descramble
   the IPL and know the DMA source→dest mapping *ahead of time*, so we recompile
   stage 2 at its runtime address `0x81300000` and link it in — zero runtime
   cost. This solves the *known* stage-2 completely. **This is the immediate
   fix (§11).**
2. **Runtime overlay system ("Layer A"):** for any code that appears at runtime
   we did **not** predict (a later DMA'd stage, self-modifying code, a revision
   whose bytes differ), an interpreter runs it once while a background compiler
   builds a native shard, and a content-hash validity gate guarantees we never
   run a shard whose bytes don't match live RAM. This is the general safety
   net and the correctness floor. It makes us **native, not purely static —
   which is acceptable and intended.**

---

## 2. psxrecomp architecture: two layers

- **Layer B (ahead-of-time):** captured overlays are compiled into the shipped
  binary's dispatch table. Contribution loop: capture JSON → `compile_overlays.py`
  ingest → `game.toml [[overlays]]` → recompiler emits C → linked in. Checked
  before Layer A. Zero runtime cost.
- **Layer A (runtime self-healing):** on a dispatch miss into a runtime code
  window, load any cached native shard for it and register it; if none, the
  **interpreter runs it once** while a **background compiler** emits C → compiles
  a shard → registers it, so from the *next* entry it runs native.

Tier order actually in force (from `overlay_backend.h`, sljit removed):
**`static ▸ gcc-DLL ▸ tcc-DLL ▸ interpreter`**.

---

## 3. Identity / keying — the crux ("shards share memory, tag them uniquely")

**Content, not address, is the identity.** Two keys, do not conflate:

- **Shard/DLL-file key** = `region_start + region_crc` → filename
  `<region_start:08X>_<region_crc:08X>.dll`. `region_start` = phys addr of the
  first dirty page in the capture window (walkback-clamped to fixed windows so
  the key is stable); `region_crc` = CRC32 of the whole region image. Multiple
  DLLs can share one `region_start` (same address, different content over time).
- **Per-function validity key** = `(entry, code_crc)` — the real disambiguator.
  `code_crc` = CRC32 over **only that function's reachable code byte-ranges**
  (jump-tables/data excised, so the hash is stable across reloads). Stored in a
  `.ranges` sidecar manifest:
  ```
  F <entry:08X> <code_crc:08X>     one per function
  R <lo:08X> <len:X>               one per coalesced reachable code range
  ```

At dispatch, every registered function is a **`Candidate`** chained per guest PC
(open-addressed hash on the phys addr, `Candidate.next` links variants at the
same address). Collision resolution = **content match**: run the first
non-blacklisted candidate whose **live-RAM CRC over its ranges == its
`code_crc`**. Village-code and overworld-code at the same `0x800E7xxx` are two
candidates on one chain; whichever matches live RAM *right now* wins. `code_crc`
comes from the compile-time manifest (not sampled at registration) so validity
is timing-independent — that is what makes address-reuse and reload-on-return
correct.

**Two hashers must be bit-identical.** psxrecomp pins CRC32 (zlib poly
`0xEDB88320`, init/final `0xFFFFFFFF`) on both the offline tool (`binascii.crc32`)
and the C runtime (`crc32_compute`), both over the raw little-endian RAM image.
gcnrecomp must pin one hash identically across our emitter and runtime, or the
whole validity model silently degrades to interpretation. (Gekko is big-endian
guest, but the hash is over the host byte image of MEM1 — pick and pin one
convention.)

---

## 4. Capture

- **Hook:** at DMA completion into a code window (PSX: `execute_ch3_cdrom`,
  `load_start < 0x1C0000`). GCN analogue: the **EXI mask-ROM DMA** that lands
  stage 2 (and any later DMA into an executable region).
- **Capture from EXECUTION-TIME live RAM, not DMA-landing bytes.** (§9 drift-B:
  the code captures live RAM at a coherent post-fixup moment; capturing raw
  DMA-time bytes missed relocated jump tables and caused a blue screen.) For us,
  descrambled-at-rest bytes are already post-fixup, but if we ever capture at
  runtime, capture live RAM.
- **Capture set** keyed **write-once by `load_addr`** (same address re-DMA'd is a
  no-op); stores a copy of the bytes + a state (`QUEUED/COMPILING/COMPILED`).
- **Seeds** = candidate function-entry PCs (walk roots), by an evidence
  hierarchy: declared/`game.toml` entries → **executed PCs** (strongest) →
  direct call targets → resolved jump-table targets → **never a whole-byte
  sweep** (decoding data as code fails the whole region).

---

## 5. Dispatch & the code-provider seam

`overlay_loader_dispatch(cpu, addr)` is called **before** the interpreter, in
precedence: static native → static overlay → **overlay candidate table** →
interpret. For each candidate at the PC:

- **Generation fast-path:** each candidate caches `val_gen` = sum of page
  generations over its watched code pages at last validation. If `state==VALID`
  and the gensum is unchanged (no watched page written since), **trust it, skip
  the CRC**. Otherwise re-hash live RAM over its ranges and compare to
  `code_crc`. Match → run native (revalidate); mismatch → mark INVALID, try next
  candidate, else fall to interpreter.
- Registration appends a candidate, marks its code pages **watched**, sets
  `code_crc` from the manifest, links the per-PC chain. Live-active state is a
  stack of currently-executing entries (for self-mod detection), not a single
  "active overlay" pointer — the active variant is resolved per-dispatch by
  content.

**`code_provider.{c,h}`** is a backend-agnostic seam so the dispatch spine never
names a compiler:
```c
typedef struct CodeProvider {
    const char *name;                       // "gcc" | "tcc" | "sljit"
    int  (*available)(void);
    int  (*request)(void);                  // batch/async: start off-thread compile
    int  (*busy)(void);
    void (*poll_main)(void);                // apply finished compile on emu thread (rescan)
    void (*compile_fragment)(uint32_t entry, const uint8_t*, uint32_t,
                             uint32_t base, CompiledFragment* out); // sync on-miss (sljit)
} CodeProvider;
```
- **gcc/tcc provider** = batch/async (`request/busy/poll_main`); `compile_fragment
  == NULL` (a compiler spawn is far too slow for the dispatch path). Backs both
  gcc and tcc — same pipeline (recompiler→C→compiler→shard→register), differing
  only in the compiler binary. **This is the live path** and the model for us.
- **sljit provider** = sync in-process JIT (`compile_fragment`). **Deprecated /
  gated off by default** (`g_sljit_tier_enabled = 0`, emitter miscompiles).

---

## 6. Interpreter fallback (the correctness floor)

**The real fallback is `dirty_ram_interp.c`** — a full instruction-by-instruction
guest interpreter over the *same* `CPUState`/memory bus and cycle model. (Note:
`psx_interpreter.c` + `stub_interpreter.c` are **oracle-only** — the reference
interpreter for diffing and its no-op shipping stub — **not** the runtime
fallback. Don't build a "simple stub" second interpreter.)

- **Entered** via a single choke point `dirty_ram_dispatch(cpu, addr, stop_addr)`
  from the generated dispatch trampoline when a PC is not statically compiled and
  no native candidate claims it — but **only for dirty pages / runtime windows**;
  clean statically-known code never interprets.
- **Exits** back to the dispatch loop on: a return (`pc==0` via `jr $ra`); a tail
  transfer to non-local/compiled code (surface the target in `cpu->pc`,
  re-dispatch flat — a tail carries no return obligation, so it must not nest a
  fresh trampoline); reaching `stop_addr` (the caller's return contract
  boundary); or an unsupported/stale opcode.
- **THE CALL CONTRACT (load-bearing, port verbatim in spirit):** a callee run as
  a C-return *unit* (native shard or a nested interpret) must itself **hold the
  continuation** — on the host C stack, or by resuming at `return_pc`. Surfacing
  a CALL as a bare `cpu->pc = target` to the tail-call loop **drops the
  interpreted caller's continuation → host-stack leak → crash.** psxrecomp
  routes `jal`/`jalr` through `overlay_loader_call_native` / an explicit
  `psx_call_contract(return_pc, site_sp)`, never an open-coded pc-chain for
  non-local callees. gcnrecomp's PPC interpreter must do the same for `bl`/`bctrl`
  / `blr`.

---

## 7. Dirty-RAM & self-modification detection

Two independent things — do not conflate:

- **`psx_icache.c` is TIMING ONLY** (a faithful R3000A I-cache *fetch-cost*
  model). It has nothing to do with stale-code detection. (GCN analogue would be
  a Gekko cache *timing* model — not needed for correctness.)
- **The stale-code detector is a 4 KB-page dirty bitmap** over all of RAM, bumped
  on **every CPU store and every DMA write** (all funnel through one store
  chokepoint in `memory.c`; audited that no native path mutates RAM behind it).
  On a write to a **watched code page**, `page_gen[page]++` (lazy invalidation on
  next dispatch) **and**, if the write lands in the ranges of a
  **currently-executing** native entry, that entry is **blacklisted** (can't
  recover mid-activation → must interpret). The per-candidate generation counter
  (§5) + code-only CRC turns this into: reused address holds multiple
  self-validating candidates; a store invalidates only the affected ones.

---

## 8. Async compile & hand-off

**LIVE path (`autocompile.c`): subprocess → shard → rescan.**
1. `autocompile_request()` spawns the compiler (`compile_overlays.py` →
   recompiler → C → gcc/tcc → shard file) on a **watch thread**; capped to **one
   in-flight** compile.
2. The watch thread waits for exit, sets a done flag.
3. On the **emu thread**, `poll_main()` sees done and calls
   `overlay_loader_rescan()` — re-scans the cache dir, loads new shards, registers
   candidates. **The candidate table is mutated only on the emu thread**, between
   guest instructions.
4. **Hand-off is next-dispatch, never a mid-execution hot-swap.** First
   encounter interprets; the compiled shard is picked up on a *later* entry to
   that PC. Artifacts are published **atomically** (temp file + rename), so a
   partial shard is never scanned; and every native call is content-validated
   (§3) before running, so a just-registered shard whose bytes don't match live
   RAM is rejected to the interpreter.

**Threading/safety:** single-writer dispatch table (emu thread only, no lock on
the hot path); workers read only snapshot copies of guest RAM (no live-RAM race);
queue/dedup guarded by one mutex on the cold path; cross-thread signal via one
atomic flag; the dispatch thread never blocks on a worker (compiles are
off-thread; the starvation watchdog is never starved).

**sljit worker (`overlay_compile_worker.c`) is the gated/off in-process JIT** —
report it exists but it's not the live path.

---

## 9. Cache, versioning, and doc/code drift

**Cache path:**
`<cache>/<game_id>/<tier>/<arch-abi>/cg<N>_<hash>/<region_start>_<crc>.dll` + a
`.ranges` sidecar. Three independent invalidation axes:
1. **`cg<N>_<hash>`** — codegen version + hash of the emitter sources; new
   codegen → fresh dir, old coexists, no migration.
2. **ABI-tag export** — each shard exports an ABI/flavor tag; mismatch → deleted
   + regenerated (one-shot preflight sweep to avoid per-dispatch delete storms).
3. **Per-function `code_crc`** — content mismatch → candidate INVALID → interpret.

**Doc/code drift the agents flagged (code is authoritative):**
- `OVERLAY_CACHE_V2.md`'s "bundle-keyed DLL filenames + persistent provider
  index" is **NOT implemented** — DLL files are still `<region_start>_<crc>.dll`;
  V2 shipped as per-function `(entry, code_crc)` identity + a build-time
  coverage-skip dedup + generation-gated dispatch.
- The capture is **execution-time live RAM**, not the DMA-time "unpatched" bytes
  the header/`overlay-recompilation-design.md §2.2` describe.
- Runtime **defers all shard loading to the first dispatch-miss**
  (`try_load_region`); the "LoadLibrary on DMA" step in `overlay-discovery.md` is
  a no-op stub.
- **sljit is deprecated/gated off**; the live async backend is gcc/tcc via
  subprocess (`autocompile.c`), even though `ASYNC_OVERLAY_COMPILE.md` and the
  worker file are framed around sljit.

---

## 10. gcnrecomp adaptation

Our case is a **strict subset** of PSX overlays and mostly easier: a single
BS2 stage at a fixed address, with the source known at rest. Plan:

1. **Layer B first — the static win (immediate, §11).** Pre-descramble the IPL
   (done) and recompile **stage 2 at `0x81300000` from `descrambled[0x820…]`**,
   linked alongside stage 1. The recompiler's `emit_code_sections_split` already
   takes a *list* of sections (different bases), so this is a section-list
   extension, not a rewrite. Zero runtime cost; unblocks the boot into the menu.
2. **Keep the content-hash validity gate even for the static stage.** Emit a
   `.ranges`-style manifest (`F <entry> <code_crc>`, `R <lo> <len>`, `code_crc`
   over reachable code bytes only). A candidate is callable **iff live RAM over
   its ranges hashes to `code_crc`** — fails safe to the interpreter. This is our
   protection if the at-rest bytes ever differ from what lands in RAM (a
   revision/dump mismatch, a relocation we didn't model). Pin one CRC identically
   in emitter + runtime.
3. **PPC (Gekko) interpreter fallback** — the `dirty_ram_interp.c` analogue: a
   full Gekko interpreter over the same `CPUState`/MEM1 bus, entered at a
   `gcn_dyn_dispatch(cpu, addr, stop_addr)` choke from the dispatch trampoline
   for dirty/unknown PCs only. **Port the call contract** (§6) for
   `bl`/`bctrl`/`blr`. This is the correctness floor — first-encounter code runs
   here, never stalls.
4. **Page-granular dirty-RAM + self-mod detection** (§7): a 4 KB-page bitmap over
   MEM1's 24 MB, bumped at the single store chokepoint (already `memory.c`) and
   on DMA writes (EXI/DI/ARAM); per-candidate generation counters + code-only
   CRC; blacklist active self-mod.
5. **Async shard compile via the `CodeProvider` seam** — since our recompiler
   already emits C, the gcc/tcc-subprocess path is the proven model; add an
   optional **in-process libtcc** backend (`tcc_compile_string` → `tcc_relocate`
   → `tcc_get_symbol("func_XXXXXXXX")`, register directly — sub-100 ms, no DLL
   file) for toolchain-less machines. gcc on a developer machine, tcc otherwise.
   **Skip sljit** (deprecated in psxrecomp; and we emit C, not IR). Preserve the
   file/artifact handoff + **single-writer emu-thread registration** + atomic
   publish + next-entry hand-off — those are the properties that make it
   lock-free and crash-safe.

### Build order

1. **Stage-2 static recompile (Layer B).** Multi-section recompile; pins the
   DMA source→dest mapping from the EXI DMA MAR/LEN sequence; validated against
   the Dolphin oracle the same way (extend the trace past the current window).
   *Unblocks the boot.*
2. **`.ranges` manifest + content-hash validity gate** (cheap, fails safe).
3. **Gekko interpreter fallback + the call contract** (the floor; also needed by
   any later runtime-loaded code).
4. **Dirty-RAM/generation/self-mod detection.**
5. **`CodeProvider` seam + libtcc/gcc async shard compile** (self-healing runtime
   cache) — only if/when we hit runtime-loaded code we can't pre-capture.

Steps 1–2 fix the immediate boot; 3–5 are the general framework, built when a
runtime-only code path demands them.

---

## 11. Immediate next step

Recompile **stage 2 at `0x81300000`** (`descrambled[0x820…]`) and link it into
`gcn_boot`, then re-diff against Dolphin (extend the oracle trace past the
current 20,627-MMIO window). Expected: the runtime executes the menu code Dolphin
runs (last traced PC `0x81300280`), and the lockstep continues into the menu —
where VI/GX (M2) and the RTC/SRAM/memcard command paths (M3/M4) come into play.
