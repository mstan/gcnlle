# Dolphin technique audit for the 60-FPS campaign (2026-08-03)

Source: dolphin-emu @ 35925ceb, clone at `F:\Projects\_ref-dolphin`. Two
sweeps: a rendering deep-dive and a holistic speed audit. Policy (user,
2026-08-03): lossy approaches are allowed **only** as configurable opt-ins,
**only** on the rendering/frame path, and **only** sourced from Dolphin (not
ModernGekko); the LLE baseline is never architected out — opting out is a
runtime choice. CPU/AOT/device/audio emulation stays exact.

## Ranked adoption plan

### CPU side (all exact, attacks the ~55% block-exec slice)

1. **Register caching → C-local promotion in `dr_backend`**
   (`Jit64/RegCache`). Keep PPC GPR/FPR values in C locals inside a
   translated block; flush only at observable points (block exit, calls,
   potential-fault accesses). Dolphin's invariant: consistent at every
   observable boundary. Highest payoff / lowest risk in the audit.
2. **Fastmem guard-mapped arena** (`Memmap.cpp:InitFastmemArena`,
   `MemTools.cpp` fault handler). Map guest physical+logical space into
   host VA so generated-C loads/stores are raw derefs; SEH handler is the
   exact DSI slow path. Recurs in nearly every PPC instruction.
3. **MMIO per-address handler table + downcount/heap event scheduler**
   (`MMIO.h` UniqueID dispatch; `CoreTiming.cpp` downcount/slice + binary
   heap). Two small exact wins in dispatcher overhead.
4. **Constant propagation + bl/blr inlining in AOT codegen**
   (`ConstantPropagation.cpp`, `PPCAnalyst.cpp` branch-following).
5. **DSP AOT + batched flush** (DSPLLE cycle batching as existence proof);
   host-output-only resampling (`Mixer.cpp:MixerFifo`) — never touches the
   authoritative PCM.

Rejected without a formal exactness proof: idle-loop skipping (Dolphin needs
a `sync_on_skip_idle` escape hatch — not proven exact), dual-core CPU/GPU
thread split and SyncGPU distance throttles (ordering hazards; only the
`deterministic_gpu_thread` variant is worth studying), CPU/VI overclock
knobs, `MAIN_SKIP_IPL`/`EmulatedBS2`/`HLE::Patch` (the fake-boot
anti-pattern PRINCIPLES.md exists to forbid). `MAIN_EMULATION_SPEED`-style
host pacing is safe. `MAIN_FAST_DISC_SPEED` touches DVD-timing-sensitive
game logic — CPU-adjacent, not render-adjacent; opt-in only if ever.

### Rendering side

1. **Exact-integer general TEV program ("ubershader")** — Dolphin proves
   the full TEV state machine fits one shader with integer math
   (`UberShaderPixel.cpp`; state packing `PixelShaderGen.cpp:322-392`
   `PSBlock`, per-stage raw cc/ac words + konst lookup). Our version goes
   into our own compute rasterizer (`gx_draw_f.comp`), so Dolphin's
   GPU-fixed-function raster caveats don't apply; arithmetic contract is
   OUR `tev_draw`, gated by corun + XFB hash. Dolphin's two unresolved
   rounding TODOs (divide-by-2 bias, D sign-extension) are resolved by
   matching our software model, not theirs. Replaces per-shape hand folds
   (programs A..AD) as the coverage strategy.
2. **Vertex-loader specialization** (`VertexLoaderUID` raw-bitfield key +
   dirty-bit front cache + specialize-once; JIT in Dolphin, specialized C
   paths for us). Exact; both surveys ranked it high for our CPU-bound
   profile.
3. **Async specialize-and-swap** (`ShaderCache::GetPipelineForUidAsync`
   pattern): general path always live, specialized programs swap in when
   ready. Pure scheduling, no exactness cost.
4. **EFB→RAM tiled encode**: the 3-tap copy-filter collapse
   (top=c0+c1, mid=c2+c3+c4, bottom=c5+c6, `>>6`, overflow mask `&0x1ff`)
   is verified bit-exact between Dolphin's GPU shader and software
   reference — reuse the algorithm, but implement integer end-to-end (their
   shader round-trips through normalized floats). Feeds our resident
   EFB→texture copy design (states 0x01023B/0x010263, fmt RG8/RGBA8).
5. **EFB peek/poke**: blocking-peek/fire-and-forget-poke split + tiled
   readback cache (`EFBInterface.cpp`, `FramebufferManager.cpp` tile
   cache).

Do NOT copy: EFB→VRAM scaled copies (self-admittedly inexact), half_scale
GPU bilinear, sampled texture hashing (`Hash.cpp` strides skip bytes at
any samples>0 — full-content hash only), TMEM cached-skip-rehash heuristic
(explicitly game-tuned). Dolphin's own software renderer omits dithering
entirely — our software raster is stricter than their oracle on that axis.

### Opt-in lossy menu (render-path only, default off, class-3 per ENHANCEMENTS.md)

Dolphin's production concessions eligible for mirroring behind knobs, each
with its verify-mode divergence quantified before shipping:
skip-EFB-copy-to-RAM / skip-XFB-copy-to-RAM (GPU-resident copies),
defer-EFB-copies, immediate/early XFB presentation, skip-duplicate XFBs,
copy-EFB-scaled, ignore-EFB-format-changes, disable-EFB-CPU-access,
fast depth calc, fast texture sampling, no-fog, no-mip. Each breaks a
specific guest-visible contract (noted in Dolphin's GraphicsSettings) —
document per-knob what is sacrificed.
