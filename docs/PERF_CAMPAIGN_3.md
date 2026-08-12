# Perf campaign 3 — correctness-preserving CPU + GPU architecture to 60 FPS

Status: **historical campaign record** (2026-07-14), superseded by the
2026-08-09 checkpoint in `../ISSUES.md`. Its 42.89-DrawDone/s result and ranked
next steps predate the page-CRC, general-TEV, snapshot/resume, and texture-COW
work; do not use them as current performance or priority claims.

Current accepted cadence is **42.89 FPS** (1,291 `GXSetDrawDone` completions in
30.10 seconds), up from the reproduced 21.27 FPS baseline. The first CPU-only
candidate measured 41.83 FPS before the renderer-boundary rebuild. The 60 FPS
goal is not yet met.

## Recovered baseline

The previous campaign did not reach a real interactive 60 FPS. Its accepted
current-source PGO executable measured 9.55 firmware `GXSetDrawDone`
completions/s in throttled uniform mode and 20.31/s in unthrottled derived
mode. A fresh 30-second unthrottled derived run on the same accepted PGO
executable and this host's RTX 3080 Ti measured 638 completions, **21.27 FPS**.
That reproduces the old result closely enough to use it as this campaign's
starting point.

The most recent derived-mode dispatcher attribution at 11,534,336 blocks is:

| Bucket | Share |
|---|---:|
| Generated PPC execution | 42.5% |
| GX (overlapped pipeline accounting) | 33.1% |
| DSP LLE | 17.7% |
| VI | 3.1% |
| AI | 1.8% |
| DI | 1.8% |

This changes the burndown in one decisive way: a GPU renderer is necessary but
not sufficient. Even treating the 33.1% GX bucket as strictly serial and making
it free gives an Amdahl ceiling of about 1.49x, or 31.7 FPS from the reproduced
21.27 FPS. The 60 FPS target needs both the renderer architecture and the
generated-CPU/DSP residuals. No claim that “OpenGL is present” may stand in for
this: the existing `GCN_GL` path only uploads the already software-rendered XFB
to a presentation texture.

## Ecosystem audit: what can be borrowed without changing the project

### ModernGekko (local snapshot `4b94e358`)

ModernGekko's new standalone GPU surface provides useful *shapes*:

- `GpuBackend` receives CP, XF, and BP writes plus draw packets;
- `GxStateBackend` keeps a canonical CP/XF/BP register image;
- `GxRenderDevice` separates decoded draws, EFB copies, and XFB presentation;
- `GxPipelineKey` derives immutable pipeline identity from topology and the
  state which determines rendering behavior;
- textures are described separately from pipeline state.

It does **not** contain a concrete `GxRenderDevice` implementation. Tests are
the only subclass in the audited tree. Its production path delegates rendering
to full Dolphin, so ModernGekko cannot be imported as the missing fast renderer.
The interface/state-key ideas are worth independently reproducing; its runtime,
device models, and vendored Dolphin closure remain rejected for the reasons in
`MODERNGEKKO_AUDIT.md`.

### Dolphin (local oracle snapshot `6de526c6`)

Dolphin confirms the mature hardware-renderer decomposition:

- VideoCommon owns GX interpretation, render state, texture/EFB semantics,
  shader identities, and cache policy;
- `AbstractGfx` owns API resources, pipelines, draw/dispatch, synchronization,
  and presentation;
- OpenGL and Vulkan are thin(er) realizations of that common contract;
- the EFB stays GPU-resident, with explicit staging/readback only at observable
  synchronization points;
- specialized shaders and ubershaders trade compilation latency against steady
  execution, and pipeline/shader caches keep compilation out of frame cadence;
- texture-cache invalidation and EFB/XFB synchronization are correctness
  mechanisms, not optional speed switches.

Importing VideoCommon or either backend is rejected. The dependency closure is
large, it would make the runtime substantially be Dolphin, and it would make
the independent Dolphin visual oracle share implementation with the code under
test. We borrow the decomposition and synchronization rules, not its device
code or shader generator.

### recomp-template constraint

GPU acceleration is a platform-wide Flipper renderer replacement on top of the
existing LLE baseline. It does not skip the IPL, synthesize firmware results, or
replace guest CPU/device behavior. The software renderer remains the exact
fallback and differential reference. A GPU path may become default only after:

1. every unsupported state falls back loudly after synchronizing EFB state;
2. uniform and derived XFB goldens remain byte-identical;
3. ordered PE token/finish and MMIO oracle counts remain unchanged;
4. a shadow mode compares GPU and software EFB/XFB at copy boundaries;
5. screenshots verify the actual interactive states;
6. default-off/fallback behavior remains available for driver failures.

This is a correctness-preserving implementation of the same GX device, not an
enhancement that guesses a visually similar answer.

## Architecture selected

The implementation boundary stays below the existing FIFO decoder and above
raster execution:

```
real IPL / recompiled PPC
        |
CP + gather pipe + FIFO timing       (unchanged, authoritative)
        |
gx.c opcode/BP/CP/XF decode          (unchanged, authoritative)
        |
immutable draw/copy state snapshot
        |
        +-- software raster          (exact fallback + shadow reference)
        |
        `-- GPU raster backend
              - API-neutral command/resource contract
              - Vulkan first for headless compute + explicit sync
              - OpenGL presentation remains independent
              - GPU-resident 640x528 color/depth EFB
              - explicit EFB copy/readback/clear boundary
              - content-validated texture cache
```

Vulkan is the first implementation target on this Windows host because the SDK
and shader compiler are installed, compute/headless operation does not require
sharing the window thread's WGL context, and explicit barriers make the
guest-visible ordering contract reviewable. OpenGL remains a viable second
backend once the API-neutral contract is proven. Traditional host rasterization
may be used only if it passes exact edge/interpolation/blend goldens; otherwise
the native EFB path uses compute so the current integer edge, TEV, dither,
blend, and packed-EFB rules can be transcribed exactly.

The GPU path must initially preserve submission order strictly: one draw's EFB
effects complete before a later draw can observe them, and an EFB copy is a hard
join/readback point. Batching which reorders guest texture reads or EFB effects
remains forbidden. Optimizations can remove redundant host/API state changes,
not guest-visible operations.

## CPU work in parallel

The generated PPC bucket cannot be left for later. The prior profile found
82.29% of derived dispatcher invocations entering the three-instruction loop at
`0x813388DC`:

```
lwz r0,-31112(r13); cmplwi r0,0; beq 0x813388DC
```

That is real guest work and is not collapsed. The first campaign-3 codegen
increment recognizes the general `lwz; cmplwi; backward beq` class. In derived
mode it hoists only the invariant RAM-window/alignment classification, performs
an atomic host load for **every** guest `lwz`, charges all three guest cycles per
iteration, updates the final GPR/CR state exactly, and yields at the same cycle
deadline. Uniform mode, non-RAM/unaligned addresses, and dispatch into either
interior instruction retain the original emitted path. The transform is based
only on decoded instruction/data-flow shape, never on the IPL PC.

This increment is accepted. Its dedicated generated-code execution test proves
fast/fallback PC, GPR, CR/SO, and cycle parity across a deadline yield. All 13
recompiler tests and all three runtime tests pass. The full XFB matrix remains
byte-identical, and the MMIO oracle remains uniform `19355/3` and derived
`19354/4` with only the standing terminal PI-order divergence.

A fair cross-binary PGO/hybrid `BASE,CAND,CAND,BASE` sequence repeated three
times at 12M derived blocks measured 3.055 s baseline versus 2.584 s candidate
median: **1.182x / 18.2% faster**. The four changed chunks discard the stale
PGO counters on coverage mismatch; every unchanged object retains the accepted
PGO build, so this is conservative until profiles are retrained. A preserved
older plain executable was rejected as a comparator because it faults at the
BS1->BS2 handoff against the restored merged image.

The post-increment derived attribution at 23,068,672 blocks is:

| Bucket | Share |
|---|---:|
| Generated PPC execution | 30.2% |
| DSP LLE | 28.5% |
| GX | 30.2% |
| VI | 4.0% |
| AI | 3.5% |
| DI | 3.6% |

GX synchronous detail is still decisive: draw is 92.8% of GX time, EFB
copy/clear 6.3%, FIFO/decode 0.9%; triangle/pixel is 93.5% of draw. Making GX
free at the profiled 41.83-FPS point had a ceiling of roughly 1.43x, about 59.9
FPS, so GPU draw offload must be followed by even a modest generated-CPU or DSP
improvement.

## Renderer boundary increment

`gx_render` now owns an API-neutral ordered draw/copy boundary below the
existing FIFO decoder. The default and authoritative implementation is still
the exact software rasterizer. `GCN_GX_BACKEND=vulkan` refuses loudly and falls
back because GPU draw/copy is not yet accepted; it never presents a partial
backend as acceleration.

`GCN_GX_BACKEND=vulkan-shadow` is a real capability/resource gate. On this host
it creates a headless Vulkan 1.4 device on the RTX 3080 Ti, a queue and command
pool, and two GPU-resident 640x528 `R32_UINT` EFB images for packed color and
depth. Software remains authoritative, but the shadow now executes two
independent compute passes at every ordered copy boundary: exact EFB-to-XFB
vertical filtering/YUV422 encoding before clear, followed by the packed
color/depth clear. It reads the results back and compares XFB rows against
guest RAM and both EFB planes against software. Uniform 5M passed 130/130 XFB
and EFB comparisons; derived 24M passed 745/745 of each. The corresponding
pinned hashes remain `fee16a8b6143e82698169b9bfba77801` and
`bea0d67fb1dbba07be06c815c5c4d451`. The deliberately synchronous full-readback
shadow is a differential validator, not a performance configuration.

The exact current software-default executable completed 1,291 frames in 30.10
seconds (**42.89 FPS**) with a clean debug-port quit. A three-round
`BASE,CAND,CAND,BASE` interface check showed no default-path regression (the
candidate median was faster, but that is conservatively treated as stale-PGO
recompile/noise rather than a claimed benefit from one extra call boundary).

## Exact resident Vulkan increment

The Vulkan backend is now an authoritative GPU-resident EFB path for every IPL
state observed in this campaign. FIFO/BP/CP/XF decode, transform, clipping, and
state snapshots remain the LLE implementation; compute performs the exact
packed-EFB triangle scan for fused programs A--K, exact EFB clear, and exact
YUYV XFB encode. Textures use a full-content-validated cache. Unsupported state
first synchronizes the GPU EFB back to the software planes and then falls back
loudly. No fallback occurred in the accepted uniform or derived workloads.

Draw shadow validation passed 128 full-plane comparisons for each A--F in both
uniform and derived runs. Derived 24M also passed 745 ordered EFB and 745 XFB
copy-boundary comparisons. The resident path preserves the pinned uniform 5M
and derived 24M hashes exactly while processing 16,693/72,868 post-clip
triangles respectively.

The full resident hash matrix also passes: uniform 8M remains
`a94db4e05555e87a03704c79def96005` and derived 56M remains
`5227ee9c3f4ddd0f7486c2ad508b3e7a`. A late-state census replaced the old
140,541-triangle synchronized fallback with exact programs G--K. G/H/J reuse
already-proven two-stage folds; I adds exact I4/I8/RGBA8 tiled texture decode;
K performs ordered early `LEQUAL` depth test/update on the resident depth
plane. G, H, I, and J each passed 128 full-color-plane comparisons. K passed
128 full color **and depth** plane comparisons. At derived 56M all 253,391
post-clip triangles are resident and synchronized fallback is zero.

The accepted performance work is:

- 16x16 tile lists replace one dispatch per triangle with one ordered dispatch
  per copy interval;
- upload/job/texture staging stays host-coherent and fast for the GPU, while a
  separate host-cached readback buffer receives XFB bytes through a GPU copy;
- XFB filtering evaluates the shared odd source pixel once, uses an exact
  register-256 1x-scale specialization, and uses subgroup shuffle reuse only
  when the selected compute device reports relative-shuffle support; the same
  source builds a portable exact fallback;
- resident completion uses a reusable fence with a bounded pause-poll before
  its blocking fallback, rather than a device-wide `vkQueueWaitIdle`;
- readback barriers and buffer copies cover the exact encoded XFB byte count,
  not the 640x528 maximum allocation. This is especially important for the
  IPL's 592x2 third copy each frame.

Interleaved preserved-executable comparisons accepted each of the last three
changes. Three-filter XFB beat four-filter in all three pairs (up to 49.5 FPS);
fence completion beat queue-idle in all three pairs; exact-size readback beat
maximum-size readback in all three pairs. Host samples remain noisy, with the
accepted path commonly in the high 40s/low 50s before the compact ring and
late-state shader expansion. Those figures are retained as historical A/B
evidence rather than the final acceptance rate.

`GCN_GX_GPU_STATS=1` now records Vulkan timestamps. At derived 24M it measured
497 draw batches in 31--33 ms, 745 XFB shaders in about 26 ms, and 745 clears
in 2--3 ms total: only about 0.25 ms of measured compute work per frame. The
remaining renderer wall was submission/completion/readback latency. The
accepted compact ring retains the lean allocation and uses four command/XFB
slots inside it. Each copy is submitted promptly without waiting; guest RAM is
published only when the ring fills or at a real PE/token/explicit-drain
observation boundary. Draw packets use a bounded arena in the existing staging
buffer, and texture mutation drains first so the GPU never observes overwritten
host data. GPU-stat mode deliberately keeps eager publication because its query
pool is diagnostic, not the release path. Uniform 5M and derived 24M remain
byte-exact with 132/748 prompt submissions and zero synchronized fallback.
High-priority paired runs favored the compact ring in three of four pairs, with
about a 6.5% median gain; it is therefore retained.

After this change the derived dispatcher attribution is 44.1% generated PPC,
30.6% DSP LLE, and 12.2% GX (AI/VI/DI are the remainder). DSP still executes
the real ROM: about 31.8 million interpreted instructions at 24M, with roughly
91% of calls stopping at the ROM idle PC. Raising the exact DSP flush threshold
from 96 to 384 cycles passed the hash gate but lost all four paired cadence
runs, so 96 remains the accepted setting.

The current-source hybrid PGO build reuses the trained counters for all 158
unchanged generated PPC chunks and the DSP interpreter. Materially changed GX
control flow is compiled normally rather than forcing stale counters. It keeps
the uniform 5M and derived 24M hashes exact and beat the plain compact-ring
binary in all four alternating high-priority pairs: 41.696 versus 36.924 FPS
median on the thermally loaded host, a 12.9% relative gain. Absolute steady-
state samples after host cooldown were 68.08, 63.17, and 65.70 FPS.

The final continuous gate ran 400M derived blocks for 63.220 wall seconds. It
produced 4,288 `GXSetDrawDone` completions (**67.827 FPS**), captured 5,301,228
draws / 2,204,658 post-clip triangles, and reported zero synchronized fallback.
This is one continuing boot/UI execution, not a loop of short favorable runs.

Rejected experiments retained as evidence, not source behavior: a cached
upload buffer (GPU wait regression), a two-pass vertical-filter image (42.2
FPS), a workgroup-shared XFB rewrite, and a single late three-copy command
buffer. All were exact where noted, but slower.

## Burndown

- [x] Reconstruct accepted performance state and correct the “interactive 60
  FPS already met” record.
- [x] Reproduce current unthrottled derived cadence (21.27 FPS).
- [x] Attribute the renderer: triangle/pixel work is ~94% of GX draw time;
  presentation GL is not the renderer.
- [x] Audit ModernGekko and Dolphin at the architecture boundary.
- [x] Select the fidelity contract and Vulkan-first backend boundary.
- [x] Accept/reject the exact PPC poll-loop codegen increment through full XFB,
  oracle, ctest, and cross-binary ABBA measurement.
- [x] Land an API-neutral GX renderer interface with software as the zero-risk
  default and no measurable software-path regression.
- [x] Add a Vulkan capability probe and headless EFB resource lifecycle.
- [x] Implement exact clear and EFB-copy/readback first; require byte-identical
  XFB before draw acceleration.
- [x] Implement draw-state snapshots and a content-validated texture cache.
- [x] Port the exact fused IPL TEV programs (A--K) plus exact edge/blend/depth
  rules to the GPU shadow; loud synchronized software fallback for all other
  state.
- [x] Expand the observed boot/UI surface through I4/I8/RGBA8 textures and
  early depth test/update; the continuous gate has zero fallback.
- [x] Re-profile CPU/DSP after GPU offload; reject the slower DSP batching
  increment and accept current-profile PGO through exact hashes and paired
  measurement.
- [x] Demonstrate >=60 `GXSetDrawDone`/s for 60 seconds unthrottled.
- [ ] Verify paced window presentation, screenshots, memory cards, RTC, disc
  path, both timing modes, and uncapped behavior.

## Acceptance scoreboard

Pinned pre-campaign XFB hashes remain:

- uniform 5M: `fee16a8b6143e82698169b9bfba77801`;
- uniform 8M: `a94db4e05555e87a03704c79def96005`;
- derived 24M: `bea0d67fb1dbba07be06c815c5c4d451`;
- derived 56M: `5227ee9c3f4ddd0f7486c2ad508b3e7a`.

Oracle counts remain uniform `19355/3` and derived `19354/4`, with only the
standing terminal PI-order divergence. No campaign-3 increment is accepted by
documentation or a same-binary toggle alone: it needs a pre-change executable,
hash-checked artifacts, interleaved ABBA wall measurement, and the complete
correctness matrix.
