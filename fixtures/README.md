# Cosim fixtures

Shared memory-card fixture for invariant-point cosimulation (docs/COSIM_DESIGN.md §5).
Generate with:

    tools/gcn_memcard/gcn_memcard format fixtures/slotA.raw --mbits 16

Expected SHA-256 (16 Mbit factory blank):
26de966bc3c951f80fe8fb676a46a45f9fa39ff6d8b7b1cc7b3225b2a14246f7

Feed to both sides: runtime GCN_MEMCARD_A=<path>; Dolphin -C Dolphin.Core.SlotA=1 -C Dolphin.Core.MemcardAPath=<path>.
