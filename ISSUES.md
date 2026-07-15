# Known Issues and Validation Status

Status as of 2026-07-15. This is an early-development checkpoint, not a claim
that the IPL runtime is production-ready.

## Rendered-window flicker

The original frequent flicker, split frames, red frames, and intermittently
stretched objects were reproduced with frame-by-frame desktop captures. Several
concrete races and ordering problems have been fixed:

- VI now snapshots XFB memory under a reader/writer lock shared with both EFB
  copy implementations.
- VI publishes only a new, completed `GXSetDrawDone` generation instead of
  exposing intermediate EFB copies.
- The resident Vulkan renderer no longer submits asynchronously and then reuses
  its single mapped draw/staging arena while the GPU may still be reading it.
- Frame-batched Vulkan copies now include the missing clear-to-following-draw
  shader memory barrier.
- The WGL presenter owns a stable window DC, uses double buffering and vsync by
  default, and completes the small upload/quad workload before swapping.

The last valid like-for-like WGL capture, taken before the completion-before-
swap change, contained one partial host frame in a 60-second run. A subsequent
GDI control run showed no unmistakable corruption, but it was on the main menu
rather than the exact inner Game Play scene. The final WGL capture was invalid
because another desktop window covered the capture region. Therefore the final
WGL change is **not yet validated**, and this issue remains open.

Next validation:

1. Run a clean, unobstructed 60-second-or-longer WGL capture on the inner Game
   Play screen.
2. Extract and inspect the largest temporal deltas, not just the aggregate
   score.
3. Repeat while resizing and while audio is enabled.

## Frame pacing and audio

The interactive path now defaults to the resident Vulkan renderer, WGL vsync,
field pacing, and event-driven WASAPI playback. Audio and GX producer threads
have explicit scheduling priorities, and the window consumer coalesces stale
frames.

The latest renderer changes have not yet had a clean audio-enabled endurance
run. Smooth rendered pacing and zero sustained WASAPI underruns must still be
verified together. Desktop capture itself delivered roughly 42--48 captured
frames per second under load; that measurement includes GDI desktop-capture and
encoder overhead and is not, by itself, the emulated VI frame rate.

## Window resizing

Interactive edge resizing constrains the client area to the active display
aspect ratio. Maximize and programmatic resizes use centered letterboxing or
pillarboxing rather than stretching. The implementation is present, but the
final resize stress test is pending.

## Memory cards

Slots A and B load persistent raw images, and the current test setup includes an
empty Slot B image. In-menu save copy/delete behavior still needs an explicit
interactive acceptance pass after renderer validation.

## RTC host-time initialization

`GCN_RTC_HOST=1` samples local host time once during boot and then advances from
emulated Gekko cycles. It does not continuously substitute host time. Startup
logs confirmed this behavior in the current runtime.

## Build reproducibility

The existing `runtime/build-pgo` cache mixed MSYS and native-Windows paths. A
CMake regeneration removed its intermediate `gcn_boot` objects, so the latest
local executable was produced by recompiling the changed runtime objects in the
working native build tree and relinking them with the existing PGO libraries.
Build products are ignored and are not part of this checkpoint. A fresh,
from-scratch release/PGO build remains required before the next release.

All runtime, capture, Ninja, and compiler processes were stopped at this
checkpoint.
