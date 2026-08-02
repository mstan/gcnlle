# Generated-code compile pipeline

The development loop is sharded at two levels:

1. **Guest module** — immutable IPL/BS1, retail apploader, main DOL, and each
   relocated REL are separate content identities.
2. **Native object shard** — each module is split into small C translation
   units. The default is 1,024 Gekko instructions, exactly 4 KiB of guest
   address space.

The 4 KiB default intentionally matches the interpreter capture blob and
native-validity page. A captured page can therefore become one independently
cacheable object without regenerating an unrelated module.

`dolrecomp --chunk-instructions N` accepts powers of two from 64 through 4096.
The 1024 default is the development setting. Larger shards remain available
for measured release-performance experiments because they keep more direct
branches inside one host function.

## Stable dependency surface

Generated output has two headers:

- `generated_abi.h` contains only the CPU/runtime helper ABI used by chunk
  translation units. For a given CPU profile it is stable across module and
  shard-layout changes.
- `generated.h` contains the module's function list and dispatch tables.
  Adding a shard changes this small manifest but does not invalidate existing
  chunk objects.

The runtime and recompiler automatically use `ccache` when it is installed.
The runtime also provides two generated-code profiles:

- `GCN_GENERATED_BUILD_PROFILE=ITERATE` (default): `-O1 -g1` for short edit,
  capture, and fold cycles.
- `GCN_GENERATED_BUILD_PROFILE=RELEASE`: the existing `-O2` configuration and
  measured large-function inlining budgets.

Runtime/device-model code retains the normal build-type optimization in both
profiles.

## Local benchmark

Measured on the development host on 2026-08-01, with processes at below-normal
priority:

| Input | C size | Flags | Wall time |
|---|---:|---|---:|
| Existing 4096-instruction shard | 1,395,284 B | `-O1 -g1` | 17.95 s |
| New 1024-instruction shard | 308,130 B | `-O1 -g1` | 2.99 s |
| New 1024-instruction shard | release `-O2` budgets | 6.43 s |
| New shard, `ccache` fill | 308,130 B | `-O1 -g1` | 3.32 s |
| New shard, `ccache` hit | 308,130 B | `-O1 -g1` | 0.14 s |

Four page shards cover approximately one old shard. The representative cold
sequential cost therefore falls from about 18 seconds to about 12 seconds,
with larger practical wins from finer parallel scheduling and rebuilding only
the pages discovered in a new capture.

## Wind Waker whole-title measurement

The first combined Wind Waker build compiled 628 IPL shards and 822 main-DOL
shards at below-normal priority with two parallel jobs. A cold build and link
took 2,503.8 seconds (41.7 minutes). After a generated-module identity change,
the same pipeline reused cached objects and completed in 58.6 seconds. A
subsequent stable end-to-end invocation regenerated no files, Ninja reported
no work, and the wrapper completed in 7.011 seconds.

This is the intended recompilation loop: stable per-page source names and
generated contents preserve both Ninja dependency results and compiler-cache
keys. Newly harvested coverage should add or replace only affected shards,
not force a monolithic title rebuild.

## Build scheduling

Generated-code builds should run with conservative parallelism and below-normal
OS priority during interactive development. High parallelism over multi-megabyte
C functions causes memory pressure and makes the workstation unresponsive; it
does not improve the useful edit-to-result latency.
