# Recomp Project Principles

These rules are system-agnostic. Platform-specific folders may add details,
but they should not weaken these fundamentals.

Everything below assumes the goal is a faithful representation of the game.
For deliberate faithfulness breaks (widescreen and other opt-in
enhancements), see ENHANCEMENTS.md — its Rule 1 (default is
system-authentic, byte-identical with the option off) is what keeps these
fundamentals intact underneath an enhancement layer.

**Faithfulness is the foundation, not the destination.** Build the
foundation **LLE-first** — static recompilation / native execution, and on
platforms that recompile their own firmware/BIOS, the real recompiled
firmware — proven against an independent oracle. Once that foundation
holds, the *destination* is deliberate enhancement: widescreen, higher
frame rates, accelerated load times. Ship enhancements **on top of** the
faithful core, never instead of it. The phasing is a rule, not a mood: the
per-game shims and hacks ENHANCEMENTS.md permits belong to the
**enhancement phase, after the foundation is proven** — they are never a
way to *reach* the foundation. (See "LLE Is the Baseline; HLE Is a
Subsystem Replacement" below for where a faithful HLE swap is allowed
*within* the foundation.)

## Ground Truth

- The original ROM/disassembly and a trusted interpreter/oracle are the
  behavioral source of truth.
- Generated C is evidence, not authority. If generated C is wrong, fix the
  recompiler, runtime, analyzer, or game metadata, then regenerate.
- Before debugging a symptom, verify which runner, generated source tree,
  executable, ROM, and config are actually used by the build you are running.

## Tool Skepticism

- Treat every tool result as untrusted until validated against another source
  or a known-good case.
- Validate first outputs manually: grep results, Ghidra labels, generated
  call targets, TCP commands, frame logs, screenshots, and diff tools.
- If observability is missing, extend the structured debug surface
  (TCP/rings/traces/snapshots). Do not build conclusions on ad hoc printf spam.

## Screenshot Before Asserting Visible State

- You are REQUIRED to take a screenshot of what is on screen BEFORE asserting
  anything about the visible state — "at the title", "softlocked", "frozen",
  "reached file-select", "black screen", "playing", etc. Screenshot first, claim
  second. Never assert from memory, from what the user said, from a counter, or
  from a ring/latch flag.
- Counters, ring `frozen`/`halted` flags, and event totals describe INSTRUMENT
  state, not pixels. A latched ring flag or a static counter is not "frozen on
  screen"; a live counter is not "working on screen". Conflating them reports the
  wrong thing and erodes trust.
- Capture through the debug surface, never by foregrounding/stealing focus:
  use the TCP `screenshot`/`screenshot_file` command (or a bare off-screen
  window blit). Read the image, then describe what the pixels show. Native
  captures may be PNG; some oracles (e.g. Beetle) write BMP — convert before Read.
- This applies even when the user has already described the screen: confirm it
  with a capture before building any claim or conclusion on top of it.

## Hints Are Not Correctness

- CFG/config hints are bootstrap aids, not a first-class model of the program.
- A hint may expose a missing edge, function, table, or data shape; the proper
  fix is to improve discovery, decoding, analysis, or generation so the next
  game benefits too.
- Use per-game config only for facts that are genuinely per-game: entry points,
  ROM identity, RAM layout, tables, bank maps, and verified metadata.
- Do not paper over a compiler/runtime bug with a per-game hint unless a class
  fix and a mechanical audit are both blocked, and document that debt.

## Control Flow Semantics

- Preserve the target CPU's semantics, not the surface shape of emitted C.
- Direct calls, tail branches, fallthrough between generated split functions,
  computed dispatch, interrupts, and returns can have different stack and
  status behavior. Model those differences explicitly.
- Stack-affecting idioms that skip caller code, synthetic returns, delay slots,
  banked returns, or interrupt frames are class-level recompiler/runtime
  problems. Fix the class and audit all instances.
- Host coroutine/fiber stacks are runtime implementation details, not guest
  save-state data. Save/load must restore guest CPU, memory, devices, and a
  well-defined generated-code resume boundary; never depend on resuming an old
  host C stack after replacing guest state.

## HLE Dispatch Is an Allowlist — Make Misses Loud

When you replace a guest indirect-dispatch mechanism (cooperative-task
scheduler, jump table, command/script interpreter, `JMP (addr,X)`) with a
host-side dispatcher — a C switch or table keyed by the target PC — that table
is a **hand-maintained allowlist**. This is its own failure class, and it is
invisible unless you design for it:

- **Targets that arrive as immediate operands are not call targets and are not
  auto-discovered.** A handler PC loaded with `LDA #imm` and then installed
  (written into a task slot, a vector, a dispatch cell) is data, not a `JSR`/
  `JSL` target. Static discovery will miss it. Every such entry must be declared
  explicitly, and a sibling of a known entry (e.g. the fade-OUT twin of a
  discovered fade-IN handler) is exactly the kind of thing that gets missed.
- **The dispatcher's fallback MUST loudly trap**, routing to the same
  unresolved-dispatch / unresolved-stub surface the static path uses. A `default:`
  that silently `return`s normal is a hole in your miss-detection: the guest
  installs the task, the host scheduler reads the slot, the dispatcher finds no
  case — and a whole behavior simply never happens, with no crash, no trap, no
  log. That signature (an effect that should occur just... doesn't) cost real
  sessions because nothing pointed at it. If every HLE dispatch fallback fed the
  miss log, it would have been a one-line find.
- Audit HLE dispatch tables for completeness the same way you audit dispatch
  misses on the static path — they are the same class of bug on a surface your
  miss-checker doesn't watch by default.

## LLE Is the Baseline; HLE Is a Subsystem Replacement, Not a Starting Point

**LLE / static recompilation / native execution is the baseline.** Architect
as much of the system that way as you can, and on platforms that recompile
their own firmware/BIOS, run that recompiled firmware. HLE is a permitted
tool, but its **role** is the bright line:

- ✅ HLE as a **deliberate subsystem replacement** — the LLE baseline stays
  everywhere else and you hunk out *one* subsystem to a host-side
  reimplementation because LLE hit a real landmine there.
- ❌ HLE as the **starting point / sole implementation** — building the system
  on HLE first and backfilling LLE "later." This is the historical failure
  mode: it leaves *half an ecosystem* — an HLE shell that was never backed by
  a faithful core, where the HLE *is* the behavior and nothing arbitrates it.

So the question is never "LLE or HLE?" in the abstract; it is "is this a
deliberate replacement of one subsystem on top of a proven LLE baseline, or
is it the thing standing in for the baseline?" The second is forbidden.

### When a faithful subsystem-HLE replacement IS allowed (foundation phase)

Replacing a subsystem with HLE during foundation work is allowed only when
ALL of these hold:

- **It targets a genuine landmine in the LLE path** — non-determinism that
  cannot be made deterministic (e.g. host coroutine/fiber scheduling with no
  hardware analog), or *profound* performance loss — not mere inconvenience.
- **It is general, not per-game** — it replaces the subsystem for every game
  on the platform, keyed to the platform's documented mechanism, never to one
  title's expectations.
- **It operates on the real guest state** — reads/writes the actual guest
  structures (control blocks, queues, registers in guest RAM) so the rest of
  the still-LLE system sees consistent data, and faithfully reproduces the
  **documented** mechanism (hardware/firmware spec), not a guessed one.
- **It is continuously validated against an independent oracle** — the
  separately-authored emulator (see "The Differential Oracle…" below), not the
  code under test. A subsystem-HLE that no oracle checks is indistinguishable
  from the forbidden kind.

A swap meeting all four is a *replacement*, not a stub: it runs faithful logic
and is policed by the oracle. It removes the LLE landmine without abandoning
the faithful baseline around it.

### The forbidden kind: fake-the-answer HLE

What stays forbidden is **load-bearing HLE that fakes the answer** —
synthesizing a specific result so a milestone *looks* done ("deliver this
event so the screen unlocks"), with no faithful path beneath it and no oracle
check. It masks recompiler bugs (the path that would expose the bug no longer
runs) and fakes foundation progress (theater, not a milestone). The HLE
*dispatch allowlist* of the previous section is load-bearing in this sense —
hence its misses must trap loudly.

The discriminating test before adding ANY HLE — "if my reimplementation is
wrong, what happens?":

- "The game misbehaves / a recompiler bug stays hidden, unchecked" ⇒
  **forbidden** (fake-the-answer).
- "We diverge loudly from the oracle, or fall back to the faithful path and
  log it" ⇒ **permitted** (validated subsystem replacement, or the verified
  shadow below).

### Verified-enhancement shadow (the other permitted form)

A **verified-enhancement shadow** is a higher-fidelity reimplementation of a
guest subsystem (e.g. re-rendering an audio engine's voices in float, free of
the hardware's requantization) that runs *alongside* the faithful path and
never replaces it. Allowed only when ALL hold:

- The faithful (recompiled / hardware-modeled) path keeps running and remains
  both the authoritative output and the oracle. The shadow is never ground
  truth.
- The shadow is continuously, differentially checked against the faithful
  stream and substitutes only after a proven window.
- It reverts loudly (logs a DEGRADED-class signal) the instant it stops
  matching — never a silent guess.
- It is opt-in and present-time, off by default; with it off the output is
  byte-identical (ENHANCEMENTS.md Rule 1; a verified shadow self-polices while
  *on*).

Note the difference from a subsystem-HLE *replacement*: the shadow runs
*beside* the faithful path and the faithful path stays authoritative; the
replacement *is* the path for that subsystem, which is why a replacement must
clear the stricter "documented mechanism + general + oracle-validated" bar
above. Both are safe for the same reason the fake-the-answer kind is not: an
independent thing is always diffing them, so they cannot silently mask a bug.

## Runtime Boundaries

- Bus and memory primitives must be faithful and boring. Do not hide a control
  flow, stack, or lifecycle bug by dropping or rewriting arbitrary reads/writes
  in generic memory accessors.
- If a game needs stack normalization, mode-boundary repair, or save-state
  staging, put that behavior at the explicit dispatch/yield/load boundary and
  document the guest invariant being restored.

## Debug Loop

- Find the first divergence, not the final visible bug.
- Classify the failure: discovery/codegen, runtime/timing, memory/bus,
  input, audio/video device emulation, or game metadata.
- **First ask: is the value WRONG, or is the behavior MISSING?** They need
  opposite tactics, and conflating them is the classic time sink — you keep
  "tracing the writer" of a value that is actually correct, or chasing a width/
  flag/timing divergence, when nothing is mis-valued at all.
  - WRONG value → trace the writer (function + instruction + call path).
  - MISSING behavior (a write/effect/transition that should happen never does)
    → there is **no bad writer to find.** Trace why the PRODUCING code never
    ran: was its function reached, its task installed/scheduled, its handler
    enumerated in the HLE dispatcher? Census the install/dispatch/schedule table
    on both sides; the entry present on the oracle and absent on the recomp is
    the bug. If you find yourself re-poking the same wrong-value hypotheses
    (flag width, presentation, a data gate) across sessions, stop — you are
    likely on a MISSING-behavior bug and looking at the wrong half.
- Trace the executed edge for control-flow bugs. Trace device events with
  cycle/time stamps for audio/video bugs (e.g. a producer/consumer buffer that
  silently drops samples surfaces as a periodic tick, not a wrong note).
- After every run, check the project's equivalent of dispatch misses or
  unresolved dynamic calls — **including HLE dispatch fallbacks** — before
  deeper debugging.

## Using the Oracle When Boot Diverges

- Per-frame lockstep diffing assumes both sides are at the same game moment. If
  the recomp HLE-boots while the oracle real-boots (or you reach a scene by
  different paths), the same frame number is two different moments and a naive
  WRAM diff reports content mismatch, not a bug. Do not trust it there.
- Instead use the oracle as an **order + state + caller** reference: watch the
  same milestone PCs on both sides, capturing registers, the relevant RAM, and
  the **return address (caller)** on entry, and compare the SEQUENCE and the
  caller — not absolute frames. "Hardware runs A then B; recomp runs B then A"
  (or "hardware installs handler H; recomp never does") is a real, alignment-
  free signal. Extend the watch surface to capture the caller if it can't yet.

## The Differential Oracle Must Be Independent of the Code Under Test

A reference oracle is only as trustworthy as its **independence** from the thing
it judges. The classic trap: you build a recompiler AND an in-tree interpreter,
and they **share the same device/bus/timing/memory models** (same `src/` for the
PPU, DMA, timers, save hardware, open-bus, BIOS protection). That interpreter is
a fine oracle for *codegen* bugs — but it is **blind to any bug in the shared
layer**, because the recompiler and the interpreter diverge from real hardware
**identically**. They diff clean against each other while both are wrong.

Symptom that you are stuck here: the recomp and the interpreter agree
bit-for-bit (registers and RAM) right up to the failure, coverage is fully
static (no dispatch miss), yet the game still misbehaves. That is not "no bug" —
it is "both engines share the bug." Stop diffing recomp-vs-interpreter; you need
a **separately-authored emulator** as the oracle.

**The standalone-libretro-frontend pattern (`snesref` / `mdref` / `gbaref`).**
The independent oracle is a tiny (~350-line) SDL2 [libretro](https://libretro.com)
frontend that loads a known-good emulator **core DLL you supply at runtime**
(e.g. `snes9x_libretro.dll`, a Genesis core, `mgba_libretro.dll`), plays the ROM
with **recomp-matched keybinds**, and logs a **per-frame work-RAM-change trace**
in the *exact same JSON shape* the recomp's debug surface emits. Properties that
make it the right tool:

- **Independence:** the core is a different codebase from your recompiler and
  your interpreter, so it shares none of their device models. It is the only
  thing that can arbitrate a shared-layer (bus/DMA/timing/open-bus) bug.
- **Zero coupling / zero shipping cost:** the core is a runtime DLL, not built
  in; the shipping game carries none of it, and any libretro core for the
  platform can be swapped in. Commit only your frontend + `libretro.h`.
- **The recomp emits the matching trace too** (one env-gated per-frame RAM-diff
  to the same JSONL), so a small diff tool compares the two runs directly.

**Reproduce by PLAYING, not by aligning.** Do **not** try to align savestates
across two builds, and do **not** script input from a fresh boot to reach a
scene. Boot timing, RNG, and frame skew make both approaches *always eventually
fail* — they burn sessions. Instead a human **plays both sides to the same
scene** (save/load-state on each side to park at a hard-to-reach spot), and you
diff the traces by **value and order, not frame number** — the same
alignment-free discipline as "order + state + caller" above. "Reference settles
this object's field to X; recomp settles it to 0" is a real signal with no
frame alignment at all. Then classify WRONG-vs-MISSING and fix the shared layer
(a class fix + audit), never a per-game patch over a device-model bug.

## Windows Shell: Never Let a Path Reach ShellExecute

Symptom: a Windows **"How do you want to open this file?"** app-picker pops up
mid-build (Firefox / GIMP / Notepad / …). That dialog means a bare file path
leaked out of a command as its own token and Windows **ShellExecute**'d it — a
file with no association falls through to the picker. It is a quoting/redirection
bug, never a real "open" request, and it blocks the build behind a modal GUI.

The usual trigger is PowerShell 5.1 mangling `>` / `2>&1` **inside** a
`cmd /c "..."` string, or a command assembled by interpolating backtick-escaped
quotes (`` `"$x`" ``): the redirection tokens split off and a path becomes a
standalone argument that gets shell-executed.

Rules:
- Do **not** inline `>` / `2>&1` redirection inside a `cmd /c "..."` string
  invoked from PowerShell. PowerShell may reparse the redirection before `cmd`
  ever sees it.
- Put **all** redirection **inside** the `.bat` (`cmake ... >> "%LOG%" 2>&1`),
  then invoke the script as a single clean token: `cmd /c C:\path\build.bat` —
  no outer redirection, no surrounding quotes (use a path with no spaces).
- Or use `Start-Process cmd -ArgumentList '/c','C:\path\build.bat' -Wait
  -NoNewWindow -RedirectStandardOutput out.log -RedirectStandardError err.log`,
  which passes clean, separate argv tokens and redirects at the process layer.
- Never build a shell command line by string-interpolating backtick-escaped
  quotes — the quoting collapses unpredictably and a path escapes as a token.
- If the app-picker appears, **STOP**: a path was ShellExecute'd. Fix the
  invocation; do not retry the same line (and a blank exit code + missing log
  is the tell that the command never ran as written).

## Validation

- A fix is done only when the root cause is explained, the class of bug is
  addressed or audited, generated code is refreshed, the game builds, and a
  deterministic smoke or oracle comparison exercises the behavior.
- Keep smoke scripts and frame logs deterministic enough that the next session
  can rerun them without reconstructing your manual input path.
