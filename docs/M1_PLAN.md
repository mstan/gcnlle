# M1 Plan — Real BS1 + In-CPU Descrambler

Status: **scoping investigation, read-only**. No code changed. Everything below
is evidence-first: file/line citations, hex dumps, and a decoded oracle trace,
not speculation. Where I could not get to ground truth I say so explicitly.

## 0. TL;DR

1. **`bios/ipl.bin` contains BOTH BS1 and BS2**, back to back, under the *same*
   segher keystream. BS1 = descrambled file offset `[0x100, 0x800)` (~0x618
   bytes of real code, Dolphin reserves 0x700), loaded at guest `0x81200000`.
   BS2 = descrambled file offset `[0x820, 0x820+0x1AFE00)`, loaded at guest
   `0x81300000`. These are **two separate copies at addresses ~1 MB apart**,
   not one contiguous blob — confirmed both from Dolphin's own C++ source
   (`Boot.cpp:437-444`) and by disassembling our own recompiled BS1, which
   ends in a `mtlr r4 (=0x81300000); ...; blr` (guest `0x81200614`, raw
   instruction `0x4e800020`) — a real, decoded PowerPC branch, not an
   assumption.
2. **The current M0 pipeline already solved this exact problem once**, for the
   *offline-descrambled* case: `runtime/generate_postdma.sh` runs stage-1 for
   real (through the runtime's own EXI model) against an offline-descrambled
   ROM, dumps post-DMA MEM1, and recompiles *that* — so `func_81300000` in
   `runtime/generated/` already holds the true BS2 bytes. This is documented
   in `docs/DYNAMIC-CODE.md` §1/§10/§11 as the "stage 1 DMA-copies stage 2"
   problem and its "modify-before-recomp / Layer B" fix. **M1 is this same
   mechanism, run one level down**: instead of feeding the EXI model an
   offline-descrambled ROM, feed it the **raw scrambled** `bios/ipl.bin`, let
   BS1's *own* recompiled code do the real hardware bring-up + EXI reads, and
   let the runtime's EXI/ROM device model perform the descramble transparently
   (see §4 for why that's the faithful split, not an HLE shortcut).
3. **Disassembly of BS1's body (`0x81200000`–`0x81200614`, ~389 instructions,
   fully walked) shows NO software LFSR loop** — it's hardware bring-up
   (HID0/MSR/BAT/segment-register setup), a `0xAAAAAAAA`/`0x55555555` MEM1
   self-test, a PI-register wait, an EXI/AD16 probe, and a chunked
   (1024-byte) EXI DMA-read loop copying ROM → `0x81300000`-ish, ending in
   the `mtlr`+`blr` above. No XOR/shift-accumulator pattern appears anywhere
   in that span. This matches the community-sourced claim that descrambling
   is a **transparent, on-the-fly hardware function of reading the boot-ROM
   chip** ("MX chip... decryption on the fly", "Gekko load 32-byte bursts...
   with real-time decryption" — gc-forever Bootrom wiki, ogamespec/gc-ipl
   README, both fetched live and cited in §2). If that's right, M1's
   "in-CPU descrambler" is mostly a **device-model** change (EXI ROM reads
   return already-descrambled bytes, faithfully, because that's what the
   silicon did), not new BS1-side software logic — BS1's own recompiled code
   doesn't need to change at all once fed real EXI transactions.
4. **The exact Dolphin-oracle boundary is now pinned by evidence, not
   estimate**: the M0 commit's "full oracle-window lockstep (all 20,627 MMIO
   events)" is **exactly** the MMIO-event count up to Dolphin trace `seq
   122006` (verified by direct count against `oracle/traces/dolphin_ipl_usa_300k.trace`)
   — and `seq 122006` **is** the BS1→BS2 `blr`. So M0's own documented "first
   divergence" already sits at the true BS1/BS2 boundary; there is no hidden
   slack to find there. Dolphin's oracle coverage for M1 purposes is: **zero
   for BS1's first ~0x150 bytes** (Dolphin fakes those via `SetupMSR`/
   `SetupHID`/`SetupBAT`/`SetupGCMemory` in C++, never executes them),
   **full/exact for BS1's remaining ~0x4C4 bytes and everything BS2-onward**
   (this is the already-proven M0 lockstep).
5. **`runtime/src/memory.c`'s `gcn_mem_resolve` has no case for
   `0xFFF00000-0xFFFFFFFF`** — the true reset-vector ROM window is currently
   unbacked. `cpu_glue.c`'s `exception_vector_address` already redirects to
   `0xFFF00000 + vector` when `MSR[IP]=1` (line 76), so the *routing* is
   ready; only the *backing store* for that physical range is missing. This
   is the one clear runtime gap, and it's mechanical to close.

---

## 1. `bios/ipl.bin` layout (evidence)

Read directly with `xxd`:

- Bytes `[0x000, 0x0F4)`: ASCII `"(C) 1999-2001 Nintendo. All rights reserved.(C) 1999 ArtX Inc. All rights reserved."`, zero-padded to `0x100`. **Plaintext, unscrambled** — confirms `tools/ipl_descramble/descramble_core.h`'s `IPL_SCRAMBLE_START = 0x100`.
- Bytes `[0x100, 0x1AFF00)`: high-entropy scrambled data (segher LFSR keystream XOR), per `IPL_SCRAMBLE_END = 0x1AFF00` — both constants in `tools/ipl_descramble/descramble_core.h:27-28`.
- `bios/ipl.bin` is 2,097,152 bytes (0x200000), matches `bios/README.md`'s recorded CRC32 `6D740AE7` (NTSC-U 1.0).

After applying `ipl_descramble` (verbatim segher algorithm, `descramble_core.c` — cross-checked byte-for-byte against `ogamespec/gc-ipl` and `FIX94/Nintendont` per `tools/ipl_descramble/README.md`), I dumped the full descrambled image and read two offsets directly:

```
descrambled[0x820:0x830) = 48 00 00 99 48 00 01 71 38 00 ff ff 94 21 ff f8   (real BS2 code)
descrambled[0x100100:0x100110) = e7 7d 1f e1 64 24 2d 3b 8b ef 53 ec 8e 64 4b d0   (still noise)
```

`0x820` decodes as `bl +0x98` then `bl +0x170` — a plausible C-runtime prologue.
`0x100100` (what a naive single-contiguous `--base 0x81200000` copy would put
at guest `0x81300000`) is garbage. **This directly confirms BS1 and BS2 are
two separate copies, not one linear blob** — see §3 for the proof that our
own recompiled/executed image nonetheless has the *correct* bytes at
`0x81300000` (via the existing post-DMA-dump pipeline, not via this naive
mapping).

## 2. External corroboration (community sources, fetched live)

I do not treat these as ground truth on their own (Tool Skepticism), but they
corroborate the CPU-decoded evidence in §3 and fill in the parts Dolphin's
code doesn't need to explain:

- **gc-forever Bootrom wiki** (`gc-forever.com/wiki/index.php?title=Bootrom`):
  "BS1 ... about 0x700 bytes ... mapped to the hardware reset vector
  `0xfff00100`"; "`0xFFFxxxxx` maps to the first megabyte of bootrom via
  Flipper's memory interface"; "Bootrom scrambler is decrypting data on the
  fly" during EXI DMA; BS1 sequence = Flipper/Gekko hardware init → AD16 probe
  → memory self-test with `0xAAAAAAAA`/`0x55555555` → EXI-DMA BS2 from
  bootrom offset `0x800` to `0x012FFFE0` in 1024-byte chunks → jump to
  `0x81300000`.
- **`ogamespec/gc-ipl` README**: BS1 at `0xfff00100`, "decryption is done by
  MX chip during block reading of bootrom data", "Gekko load 32-Byte bursts
  in instruction cache" with real-time decrypt; BS2 entry `0x81300000`,
  "written in C using early Dolphin SDK."

Every one of these claims is independently reproduced by the CPU-level
evidence in §3 below (the HID0 setup, the `0xAAAAAAAA`/`NOR` self-test loop,
the chunked EXI DMA, the final jump to `0x81300000`) — so I treat the
"decryption on the fly during block reading" claim as the best current
explanation for why BS1's own body shows no explicit descramble loop (§3.4).

## 3. BS1 disassembly — full walk of the executed path (`0x81200000`–`0x81200618`)

Source: `runtime/generated/chunks/chunk_0000_ipl0_81200000.c` (already
recompiled by the existing M0 pipeline; DolRecomp's own decode, which I
cross-read as ground truth for opcodes, and independently spot-checked
against the raw bytes and against `oracle/traces/dolphin_ipl_usa_300k.trace`
retired-instruction records — see §5). All addresses guest, all instructions
directly decoded, none inferred.

**3.1 Hardware bring-up (`0x81200000`–`0x812000FC`)**
`lis/addi r4,17,3172; mthid0 r4` (HID0 = 0x00110C64-ish) → `mtmsr`
(0x00002000ish, likely just IR/DR-adjacent bits) → PI-register writes at
`0xCC003000`+ (`li r5,67/156; sth ...`) → more HID0 read-modify-write
(`mfhid0`/`ori 0xC000`/`mthid0`/`isync`) → **zero all 8 DBATs and all 8 IBATs**
(`li r4,0; mtdbat0u..mtibat3u; isync`) → **zero all 16 segment registers**
(`mtsr 0..15`) → program `DBAT0`/`IBAT0` U/L = `0x80001FFF/0x00000002`
(BEPI `0x80000000`, 24 MB block, cached RW — the main MEM1 mapping) → program
`DBAT1` U/L = `0xC0001FFF/0x0000002A` (BEPI `0xC0000000`, same 24 MB block,
different WIMG/PP bits — the uncached RAM alias) → program `DBAT3`/`IBAT3`
U/L = `0xFFF0001F/0xFFF00001` (BEPI `0xFFF00000`, small block — **the
reset-vector ROM window itself, BAT-mapped and kept mapped**) → `mfmsr; ori 0x0030 (IR|DR); mtmsr; isync`.

This is a textbook from-reset CPU bring-up: exactly what a real BS1 must do
before C code (BS2) can run with paging on. The `DBAT3`/`IBAT3` = `0xFFF00000`
mapping is itself evidence that **the CPU keeps the ROM window BAT-mapped
through the rest of boot**, consistent with continuing to fetch/read from
`0xFFFxxxxx` afterward.

**3.2 The `0x81200150` Dolphin-HLE seam.** Dolphin's `Load_BS2` (see §5) sets
`ppc_state.pc = 0x81200150` directly and *never executes* `0x81200000`–
`0x8120014C` — it substitutes `SetupMSR`/`SetupHID`/`SetupBAT`/
`SetupGCMemory` (`Boot.cpp:72-282`) for that stretch. That is the **one
window Dolphin cannot oracle at all**, not even by proxy — Dolphin's own
values (e.g. `HID0=0x0011c464`, `Boot.cpp:456`) are Dolphin's independently
reverse-engineered stand-ins, useful as corroboration but not as a checker.

**3.3 PI wait, EXI/AD16 probe, memory self-test (`0x81200150`–`0x8120043C`)**
`0x81200150`: PI base `0xCC003000` — a control-register write + a busy-wait
measured via `mftb`/`mftb`/`subf`/`cmplwi 0x1124`/`bc` (a fixed-tick hardware
settle delay). `0x81200190`: `r14 = 0xCC006400` (SI base), zero one SI
register. `0x812001A0`: `r2 = 0xCC006800` (**EXI0 base**, confirmed against
`runtime/include/exi/exi.h:38` `GCN_EXI_BASE 0xCC006800`) — a CSR write
(`r22=0x00BA`) then a poll loop (`lwz`/`and.`/`bc 12,1,...` on offset `52(r2)`,
matching the CSR-bit-set/clear idiom) — an **EXI transaction**, then another
CSR write (`0x31`) and poll — this matches the community-documented "AD16
diagnostic hardware" probe. `0x81200304`–`0x8120043C`: a subroutine
(`0x812002A4`, called repeatedly via `bl` with varying `lis r15,<n>` delay
counts) plus a 32-byte-stride store loop (`stw r26,0/4/8/.../28(r23);
addi r23,r23,32; bc 16,0,...`) writing `r26` across a memory range, then a
readback/compare loop (`lwz r15,0(r23); cmplw r15,r26; ...`) building failure
bitmasks in `r17/r18/r19`. `0x81200464`–`0x81200478`: `r26 = 0xAAAAAAAA` then
`nor r26,r26,r26` (`= 0x55555555`) — **the two-pattern MEM1 self-test**,
exactly as the community source describes, now confirmed at the instruction
level.

**3.4 No LFSR anywhere in this span.** I walked every instruction from
`0x81200204` to `0x8120051C` (the full body between the EXI/AD16 probe and
the final DMA loop) looking specifically for the XOR/shift/accumulate/8-bit
counter shape of `tools/ipl_descramble/descramble_core.c`'s `ipl_descramble()`
(three coupled 16-bit LFSRs, an 8-bit accumulator, `data[it++] ^= acc`). It
is not there — only integer compares, `rlwinm`/`slw`/`or` bitmask building for
the self-test, and register/MMIO setup. This is the basis for §0 item 3: the
descramble is not BS1 software, it's a property of *reading the ROM
device* — a faithful device-model, not an HLE fake (see §4).

**3.5 The BS2 DMA + jump (`0x81200500`–`0x81200614`)**
`0x81200500`–`0x8120051C`: sets up `r8=1` (poll mask), `r9≈3` (DMA-start
control value), `r10=0`, `r11=0x400` (**1024-byte chunk size**), `r12`
(source stride), `r3≈0x20000`/`r4≈0x012FFFE0`-ish, `r13` (remaining-byte
counter) — matches the community-cited "1024-byte chunks" and "`0x012FFFE0`"
destination almost exactly. `0x81200544`–`0x812005D0`: the chunk loop —
`stw` to EXI CSR/MAR/LEN-shaped offsets `0/4/8/12/16(r2)` where `r2 =
0xCC006800`, poll-and-clear via the same `and./bc 12,1,...` idiom seen in
§3.3, advance `r3`/`r4` by the chunk stride, decrement `r13`, loop. **This is
BS1 performing real, decoded EXI-channel-0 DMA transactions** — not
inferred, the register base (`0xCC006800`) and the CSR-poll idiom are
identical to the EXI protocol already modeled in `runtime/src/exi.c`.
`0x812005D4`: `lis r4,-32464 (=0x8130); ori r4,r4,0 (=0x81300000); mtlr r4`.
`0x812005E8`–`0x81200610`: a couple more EXI/SI register writes and a
low-mem write (`stw r3(=0),244(r4=0x80000000)` → physical `0x800000F4`).
`0x81200614`: **`blr`** — raw instruction `0x4e800020`.

## 4. Is descrambling BS1-software or hardware-transparent? (open item, both paths are viable)

I could not find ground truth strong enough to close this with certainty in
read-only time, but the evidence points one way:

- **For:** no LFSR shape anywhere in BS1's fully-walked body (§3.4); the
  community sources explicitly describe hardware/MX-chip transparent
  decrypt "during block reading of bootrom data" (§2); Dolphin's own model
  (one upfront `Descrambler()` call over the whole file, §5) is *consistent*
  with "every read from this ROM comes back descrambled" and requires no
  BS1-side algorithm.
- **Against (a residual doubt):** I did not walk BS1 instructions past
  `0x81200614` (there are none — that's the `blr`), and I did not
  independently disassemble the *real, unmodified-by-Dolphin's-hack*
  reset-vector code at physical `0xFFF00100`, because that code does not
  exist anywhere in this repo (see §6.1) — only the RAM-staged copy Dolphin
  and our own pipeline execute at `0x81200000` does. It is conceivable the
  true `0xFFF00100`-resident code contains a few descramble-priming
  instructions before reaching what's copied to `0x81200000`, invisible to
  every source available here.

**Recommendation:** treat "the EXI/ROM device model applies the segher
keystream transparently to every byte it serves from the scrambled chip" as
the working design (it is a hardware-faithful, general, oracle-checkable
device behavior per PRINCIPLES — not a per-game hack, not "faking the
answer": if the keystream constants were wrong, BS1's own self-test and jump
would visibly fail against the byte-identical M0 lockstep already proven).
Revisit only if BS1-side execution (once wired to a *truly scrambled* ROM
backing) faults or produces wrong HID0/BAT values — that would be the signal
the descramble needs to move into CPU-executed code instead.

## 5. Dolphin ground truth + the exact oracle boundary

`oracle/dolphin/Source/Core/Core/Boot/Boot.cpp:360-476` (`CBoot::Load_BS2`,
the path used for our "GameCube Main Menu" oracle boot):

```cpp
// Run the descrambler over the encrypted section containing BS1/BS2
if (data.size() > 0x100)
    ExpansionInterface::CEXIIPL::Descrambler(data.data() + 0x100, min(size-0x100, 0x1AFE00));
...
if (data.size() > 0x100)
    memory.CopyToEmu(0x01200000, data.data() + 0x100, min(size-0x100, 0x700));      // BS1, 0x700 B
if (data.size() > 0x820)
    memory.CopyToEmu(0x01300000, data.data() + 0x820, min(size-0x820, 0x1AFE00));   // BS2
...
ppc_state.pc = 0x81200150;   // TODO comment: "Execution is supposed to start at
                              //  0xFFF00000... copying to 0x81200000 is a hack."
```

I decoded `oracle/traces/dolphin_ipl_usa_300k.trace` (Dolphin's own producer,
`producer=1`, 327,872 records) with a small offline script (record layout
from `oracle/trace_format.h`, no re-run of anything — this is an
already-captured ring/trace file, consistent with the project's
always-consume-don't-arm discipline):

```
pc range covered: 0x81200150 .. 0x81339ee4
first PC seen in region 0x81200000: seq 0
first PC seen in region 0x81300000: seq 122007
the BS1->BS2 branch:  seq 122006  pc=0x81200614  insn=0x4e800020 (blr)  npc=0x81300000
MMIO events with seq<=122006: 20627   <- exactly the M0 commit's "all 20,627 MMIO events"
retired instructions with seq<=122006: 98434
```

And cross-checking the trace's *actual retired instruction words* at
`0x81300000`/`0x81300004` against our own `runtime/generated/chunks/
chunk_0064_ipl0_81300000.c`'s decode:

```
trace: pc=0x81300000 insn=0x48000099 npc=0x81300098   (bl +0x98)
trace: pc=0x81300004 insn=0x48000171 npc=0x81300174   (bl +0x170)
our generated C:  "81300000: bl 0x81300098" ; "81300004: bl 0x81300174"
```

**They match exactly** — confirming the *currently checked-in*
`runtime/generated/` (built via `runtime/generate_postdma.sh`'s
run-stage-1-for-real-then-dump-and-recompile pipeline, not the naive
single-copy `generate.sh`) already has correct BS2 bytes at `0x81300000`, for
the offline-descrambled case. This is the existing, working mechanism M1
needs to extend to the scrambled case.

**Net result for the M1 validation strategy:**

| Window | Dolphin oracle coverage |
|---|---|
| `0x81200000`–`0x8120014C` (first ~84 instrs: HID0/MSR/BAT/segment init) | **None** — Dolphin substitutes C++ (`SetupMSR`/`SetupHID`/`SetupBAT`/`SetupGCMemory`), never executes real BS1 there |
| `0x81200150`–`0x81200614` (PI/EXI/AD16/self-test/DMA/jump) | **Full, exact** — already proven, this is M0's 20,627-MMIO / 122,006-instruction lockstep |
| `0x81300000` onward (real BS2) | **Full, exact**, continuing past the current window once re-diffed (`DYNAMIC-CODE.md §11`'s stated next step) |
| The EXI *mechanics* of BS1's own ROM-read DMA (CSR/MAR/LEN sequence) | **None** — Dolphin does this via a bare `CopyToEmu`, no EXI transaction exists on Dolphin's side to diff against |

So M1's honest validation posture is: **Dolphin oracles everything except
(a) the first ~0x150 bytes of BS1, and (b) the mechanics (not the result) of
BS1's own DMA read.** For those two, use:

- **(a)**: cross-check against YAGCD/gc-forever/gc-ipl (independent
  documentation, §2), and the fact that our recompiled code, once it reaches
  Dolphin's `0x81200150` landing point, must produce the *same* HID0/BAT/MSR
  state Dolphin's HLE hardcodes (`Boot.cpp:456-461`) — a real, if indirect,
  corroboration (Dolphin's constants were independently reverse-engineered,
  not derived from our code).
- **(b)**: a **final-state check, not a mechanism check** — after our real
  EXI DMA completes, diff the resulting MEM1 bytes at `0x81300000` against
  (i) Dolphin's memory at the same address (available even though Dolphin
  got there differently — "order+state+caller, not mechanism" per
  PRINCIPLES) and (ii) `tools/ipl_descramble`'s independently-vendored
  offline descramble of the same file range. Two independent checks on the
  *result*, none on the *how*, which is exactly appropriate since Dolphin has
  no "how" to compare against here.

## 6. What's missing on each side

**6.1 The recompiler.** Already generic enough: `--gamecube-ipl` takes an
arbitrary `--base`/`--entry` and an arbitrary list of `--segment <base>:<file>`
extras recompiled into the same dispatch table (`recompiler/src/app/cli.c:20-24,
159-204`) — this is precisely the mechanism `generate_postdma.sh` already
uses for the low-mem exception-handler segment (`--segment
0x80000000:lowmem.bin`, `generate_postdma.sh:50-51`). No recompiler change is
needed to add a `0xFFF00000`-based segment for true reset-vector execution —
it is just another `--segment` (or `--base`) argument. **Verified, not
assumed**: I found no RAM-range assumption in `dr_backend`/`dr_analysis` baked
into codegen (grepped for `0x80000000`/`GC_RAM_BASE`-shaped constants in
`recompiler/src/` — the only hard-coded RAM range lives in `recompiler/src/
cpu/cpu.c`'s *own* interpreter/cross-check harness, `GC_RAM_BASE 0x80000000`
in `cpu.h:7`, which is test-harness code, not the emitter).

**6.2 The runtime — the one clear gap.** `runtime/src/memory.c`'s
`gcn_mem_resolve` (lines 117-155) handles: cached RAM (`0x80000000+`),
uncached RAM alias, physical real-mode RAM (`< ram_size`), and Wii MEM2. **No
case for `0xFFF00000`–`0xFFFFFFFF`.** A fetch or load/store there today
returns `NULL` (`avail=0`). This must be added: a read-only backing for the
boot-ROM window, at minimum covering the BS1 body (and, per §3.1's
`DBAT3`/`IBAT3 = 0xFFF00000/0x1FFF` mapping, plausibly the same window BS1
itself keeps BAT-mapped through the rest of boot — worth re-checking whether
BS2 ever touches it again, low risk, mechanical to verify with a grep of the
generated BS2 chunks for `0xFFF0` constants).

`runtime/src/cpu_glue.c:75-77`'s `exception_vector_address` already computes
`0xFFF00000 + vector` when `MSR[IP]` is set — this is the *routing*, already
correct and unrelated to this gap; it only matters that whatever it routes
*to* is now backed.

`runtime/src/exi.c`'s EXI model (`ipl_rom_read`, `gcn_exi_set_rom`,
lines 85-165) is **already structurally ready**: it serves ROM bytes from
whatever buffer `gcn_exi_set_rom` points at, driven by the real CSR/MAR/LEN
protocol BS1 issues (§3.5) — the exact mechanism M1 needs. Today
(`runtime/src/boot.c:123-142`) that buffer is either an offline-descrambled
full image (`GCN_IPL_ROM` env var) or the already-descrambled BS2 payload
(`gcn_seed_read_file` on the *plaintext* slice). **For M1, this buffer must
become the raw scrambled `bios/ipl.bin` content, with the descramble applied
at the device-model boundary** (§4) — either by pre-applying the keystream
once into a backing buffer at device-init time (mathematically identical to
"transparent per-read hardware decrypt," since the keystream is a pure
stateless function of byte offset — this is not synthesizing an answer, it's
precomputing a deterministic hardware function once instead of per-byte;
fully consistent with PRINCIPLES' bar for a faithful device model) or, if a
stronger fidelity bar is wanted, applying it lazily per read in
`ipl_rom_read`. Either is faithful; the precomputed form is simpler and
should be the default.

**6.3 A residual protocol-level unknown (small, mechanical to close).**
Dolphin registers the mask ROM as `EXIDeviceType::MaskROM` at **channel 0,
device slot 1** (`oracle/dolphin/Source/Core/Core/HW/EXI/EXI.cpp:147`). Our
own `runtime/src/exi.c`'s `csr_chip_select` treats the CSR chip-select field
as one-hot (`1=dev0, 2=dev1, 4=dev2`, `exi.c:18-20`). The CSR value BS1
writes before its DMA loop (`0xBA`, `0x812001AC`/`0x812001D4` — see §3.3) has
chip-select bits `= 0x80` (dev0 in that one-hot scheme), which doesn't
obviously line up with Dolphin's "device slot 1" placement. This is a small,
mechanical thing to nail down while wiring the scrambled ROM into the EXI
model (it may just mean the mask ROM should be registered on our `dev_present[0]`/slot-0 path rather
than slot 1, or that Dolphin's internal slot index and the hardware CS-bit
position aren't the same numbering) — flagging it now so it isn't
rediscovered the hard way mid-implementation.

## 7. Step-by-step implementation plan

1. **(mechanical)** Add a read-only `0xFFF00000`–`0xFFFFFFFF` backing case to
   `gcn_mem_resolve` in `runtime/src/memory.c`, sized to at least cover the
   BS1 body (documented range `[0x100,0x800)` file-relative → map physical
   `0xFFF00100`.. to descrambled-ROM bytes starting at file offset `0x100`).
   Confirm via a chunk scan (§6.2) whether BS2 ever re-touches this window
   before deciding the exact backed size (start with the minimum proven
   range, extend only if a real access outside it is observed — never a
   speculative over-large mapping).
2. **(mechanical)** Change the EXI ROM device init path
   (`runtime/src/boot.c:123-142`, and the equivalent in the main
   `gcn_runtime` seed path) to load the **raw scrambled** `bios/ipl.bin`
   directly (no `ipl_descramble` CLI step first), and apply the segher
   keystream once at device-init time into the backing buffer served by
   `gcn_exi_set_rom` — reusing `tools/ipl_descramble/descramble_core.c`'s
   algorithm verbatim (link it into the runtime, or port the ~40-line
   function; do not reimplement it a second time — one algorithm, two
   call sites, matching PRINCIPLES' "wrapper: all call sites switch"
   discipline).
3. **(research, small)** Resolve §6.3's chip-select numbering before wiring
   the mask-ROM device onto the EXI model's device-present table.
4. **(mechanical)** Recompile BS1 as a `--base 0xFFF00100 --entry 0xFFF00100`
   (or `--segment 0xFFF00100:<bs1_slice>`) target from the **raw scrambled**
   bytes — this requires the recompiler to read code through the *same*
   transparent-descramble boundary as reads at runtime, i.e. **recompile from
   the offline-descrambled bytes** (as today) but **execute from the
   physical ROM window that the runtime backs with the same descrambled
   buffer** — so the static recompile and the runtime's served bytes are
   guaranteed identical by construction (both derive from one
   `ipl_descramble` call over the one vendored algorithm), which is exactly
   the jump-time integrity check the M1 task asked about (§8).
5. **(mechanical, reusing existing infra)** Extend
   `runtime/generate_postdma.sh`'s pattern one level down: run the
   *reset-vector* build (BS1 now recompiled at `0xFFF00100`, backed by the
   real scrambled ROM) through the runtime for real, let it perform its own
   EXI DMA of BS2 into `0x81300000`, dump post-DMA MEM1, recompile that dump
   for BS2 exactly as today. The stage-1/stage-2 split
   (`docs/DYNAMIC-CODE.md`) does not change shape — only the *input* to
   stage 1 changes from "offline-descrambled" to "raw scrambled + a real
   EXI-level transparent-descramble device model."
6. **(mechanical)** Update `tools/ipl_descramble/main.c`'s usage string and
   `tools/ipl_descramble/README.md` / `bios/README.md` (stale: they still cite
   the pre-oracle-correction `0x820`/`0x81300000` constants for the `--bs2`
   single-slice extraction path, `main.c:4-7,20-25` — the macros were updated
   2026-07-09 but the prose wasn't; harmless today since the tool uses the
   macros correctly, but worth fixing before it misleads someone mid-M1, per
   Tool Skepticism / Ground Truth).
7. **(research)** Re-run the Dolphin trace-tap comparison for BS1's first
   ~0x150 bytes: even though Dolphin can't execute them, the project already
   captures Dolphin's substituted HID0/BAT/MSR *values* as C++ constants
   (`Boot.cpp:456-461`) — encode these as a fixture-level cross-check
   (assert our real BS1, once it reaches the `0x81200150`-equivalent PC,
   produced the identical values) even though it's not a live trace diff.
8. **(mechanical)** Extend the existing trace/diff window past
   `0x81300000` (already flagged as the literal next step in
   `docs/DYNAMIC-CODE.md §11`) once BS1 is real, to confirm the *result* of
   our own EXI DMA (not Dolphin's fake copy) lands byte-identical.

## 8. The jump-time integrity check (as asked)

The M1 task hypothesized: recompile BS1 + keep BS2 recompiled from the known
descrambled image, with the runtime verifying at jump time that descrambled
RAM bytes match what BS2 was recompiled from. Given §3/§4/§7, the concrete
form this takes here:

- BS1 is **not self-modifying** — every byte it touches as data (the EXI DMA
  destination, `0x81300000`+) is *data*, never re-executed as BS1's own code
  (BS1 itself never branches into that region before its final `blr`,
  confirmed by the full instruction walk in §3, which stays within
  `0x81200000`-`0x81200614` throughout). So this is squarely the
  "modify-before-recomp" case DYNAMIC-CODE.md already models, not true SMC.
- The integrity check is: **CRC32 (or the project's existing content-hash
  convention from DYNAMIC-CODE.md §3) over MEM1 `[0x81300000,
  0x81300000+0x1AFE00)` immediately after BS1's `blr`, compared against the
  same CRC over `tools/ipl_descramble -o`'s independently-produced offline
  descramble of the same file range.** A mismatch means either the runtime's
  transparent-descramble device model or BS1's DMA-loop translation is
  wrong — and it fails loud (an assertion / ring event), never a silent
  fallback, matching PRINCIPLES' "HLE misses trap loudly" spirit applied to
  a static-recompile integrity gate.

## 9. Risks

- **§4's open item** (hardware-transparent vs. BS1-software descramble) is
  the one genuine unknown; low risk because it's cheaply falsifiable (BS1
  either produces a correct HID0/self-test/DMA sequence against a
  transparently-descrambled ROM buffer, or it visibly doesn't).
- **§6.3's chip-select numbering** could cost a debugging session if wired
  wrong; flagged explicitly so it isn't a surprise.
- **Font data past `0x1AFF00`** (`bios/ipl.bin`'s tail, Shift-JIS/Windows-1252
  glyphs) is outside the scrambled range entirely (Dolphin loads it
  unencrypted, `EXI_DeviceIPL.cpp:112,129`) — not an M1 concern, flagging only
  so it isn't conflated with the scrambled-region work.
- **Doc drift** (§7 item 6) is low-risk but should be fixed in the same pass
  to avoid re-deriving §1's layout from stale prose next session.

## 10. Effort estimate

**Mechanical** (clear existing pattern to extend, per §7): steps 1, 2, 4, 5,
6, 8 — this is "run the already-proven post-DMA pipeline one level down",
not new architecture.

**Research** (needs a decision or a small investigation before coding): step
3 (chip-select numbering) — an afternoon; step 7 (encoding Dolphin's
substituted values as a fixture) — small. §4 (hardware vs. software
descramble) is the only item that could expand scope, and only if the
"transparent hardware descramble" hypothesis turns out to be wrong once BS1
runs against a truly scrambled backing — in which case the fallback (BS1
itself must run the LFSR, meaning it lives somewhere in the small unwalked
gap, or in code I haven't located) is still bounded, since the *entire* BS1
body is only ~0x618 bytes and has already been fully walked once.
