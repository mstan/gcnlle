# ModernGekko GPL experiment

Branch: `experiment/moderngekko-gpl`

The pre-integration LLE baseline is commit
`0b4e42c20dec9de046b7f6f9af5d158743b2b94a`.

This branch may use GPL implementation work from ModernGekko and its
RecompCore/DolRecomp dependencies. It is not a clean-room branch and must not
be represented as suitable for later relicensing.

## Imported design

Reference revisions:

- ModernGekko: `dda273bddf486063df0b9c3c8dc2ca479f8d0180`
- RecompCore-ModernGekko: `e13ab348f13cd67879f6db6e9d7185410f8f62c6`
- DolRecomp: `93b881c8f73df1d64a88491f2aa50c7c9ed2384d`

The content-hashed chunk table and invalidate/reverify lifecycle in
`runtime/src/aot_module.c` and
`runtime/tools/gen_title_module_tables.py` are adapted from RecompCore's
`StaticRecompCore_SMC.cpp` and `module-template/gen_module_tables.py`.

## LLE boundary

ModernGekko's Dolphin chassis is not linked. The existing runtime retains
exclusive ownership of reset, BS1/BS2/IPL execution, MMIO, DI, EXI, SI, DSP,
GX, timing, retail apploader execution, and guest memory writes. A title chunk
becomes native-eligible only after its expected immutable input hashes to the
bytes that the LLE path placed in live guest RAM. A mismatch falls through to
the loud interpreter and append-only capture journal.

## First title result

An authentic reset-to-disc run reached Wind Waker's main DOL entry at
`0x80003140`. The title module verified 177 chunks with zero mismatches and
served 4,466,899 native dispatches before the 42-million-block run budget.
The loud fallback executed 2,643,922 instructions and recorded 2,954 unique
misses, concentrated in the retail apploader and dynamically installed
low-memory exception vectors.

The linked executable contains 628 IPL shards and 822 independently compiled
main-DOL shards. The main DOL does not replace or shortcut the BIOS path; its
shards remain dormant until the LLE-loaded memory identity matches.
