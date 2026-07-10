#!/usr/bin/env python3
"""
oracle/make_sample_traces.py — fabricate two sample trace files for a diff.py
demo. They share an identical prefix, then diverge at a KNOWN index so the demo
proves diff.py flags the FIRST divergence (not a later one).

Writes sample_runtime.trace and sample_oracle.trace next to this script.

The two streams are byte-identical for records 0..4. At record 5 the two
"retired instruction" records have the same PC / NPC / opcode (control flow is
identical, so Tier 1 passes) but the runtime side has a WRONG value in r3 —
exactly the case Tier 2 (full-register comparison) exists to catch. Records
after 5 also differ, to confirm diff.py stops at the first divergence.
"""
import os
import struct

import diff  # reuse the exact layout constants / formats

HERE = os.path.dirname(os.path.abspath(__file__))


def pad_payload(fmt, *vals):
    raw = struct.pack(fmt, *vals)
    assert len(raw) <= 180, f"payload {len(raw)} > 180"
    return raw + b"\x00" * (180 - len(raw))


def record(type_, seq, payload180, tb=0, cpu=0):
    hdr = struct.pack(diff.HDR_FMT, type_, diff.RECORD_SIZE, cpu, seq, tb)
    rec = hdr + payload180
    assert len(rec) == diff.RECORD_SIZE
    return rec


def retired(seq, pc, insn, gpr3, lr=0x81300000, npc=None):
    if npc is None:
        npc = pc + 4
    gpr = [0] * 32
    gpr[1] = 0x817FE8F0  # stack pointer
    gpr[3] = gpr3
    # fmt: insn,pc,npc,lr,ctr,cr,xer,msr,fpscr, gpr[32], ps_hash, flags, pad
    payload = pad_payload(
        diff.RETIRED_FMT,
        insn, pc, npc, lr, 0, 0x20000000, 0, 0x00003030, 0,
        *gpr, 0, 0, 0,
    )
    return record(diff.TR_RETIRED, seq, payload)


def mem(seq, pc, addr, value, size=4):
    payload = pad_payload(diff.MEM_FMT, pc, addr, value, 0, size)
    return record(diff.TR_MEM, seq, payload)


def mmio(seq, pc, addr, value, is_write=1, size=4, block=2):
    payload = pad_payload(diff.MMIO_FMT, pc, addr, value, size, is_write, block)
    return record(diff.TR_MMIO, seq, payload)


def exi(seq, pc, channel, device, select, cmd_len, resp_len, cmd_crc, resp_crc):
    payload = pad_payload(diff.EXI_FMT, pc, channel, device, select, cmd_len,
                          resp_len, min(cmd_len, 16), min(resp_len, 16), 0,
                          cmd_crc, resp_crc, b"\x00" * 16, b"\x00" * 16)
    return record(diff.TR_EXI, seq, payload)


def file_header(producer):
    return struct.pack(diff.FHDR_FMT, diff.MAGIC, 1, diff.FHDR_SIZE,
                       diff.RECORD_SIZE, 0, producer, 0)


def build(diverge_r3):
    """Return the list of records for one side; diverge_r3 sets r3 at seq 5."""
    recs = []
    # --- shared prefix, records 0..4 (identical on both sides) ---
    recs.append(retired(0, 0x81300000, 0x3860_0001, 0x00000001))  # li r3,1
    recs.append(mmio(1, 0x81300004, 0xCC003000, 0x0000_0000, block=3))  # PI
    recs.append(exi(2, 0x81300100, 0, 1, 1, 4, 4, 0xDEADBEEF, 0xCAFEBABE))       # EXI RTC read
    recs.append(retired(3, 0x81300008, 0x9061_0008, 0x00000001))  # stw r3,8(r1)
    recs.append(mem(4, 0x81300008, 0x817FE8F8, 0x00000001))       # the store lands
    # --- record 5: control flow identical, r3 value differs ---
    recs.append(retired(5, 0x8130000C, 0x3863_0002, diverge_r3))  # addi r3,r3,2
    # --- records after the divergence also differ (proves first-stop) ---
    recs.append(mem(6, 0x8130000C, 0x817FE8FC, diverge_r3))
    recs.append(retired(7, 0x81300010, 0x4E80_0020, diverge_r3))  # blr
    return recs


def main():
    runtime = build(diverge_r3=0x00000003)   # runtime computes r3 = 1 + 2 = 3 (correct-looking)
    oracle = build(diverge_r3=0x00000099)     # oracle shows r3 = 0x99  -> the divergence at seq 5

    for name, producer, recs in (
        ("sample_runtime.trace", 0, runtime),
        ("sample_oracle.trace", 1, oracle),
    ):
        path = os.path.join(HERE, name)
        with open(path, "wb") as fp:
            fp.write(file_header(producer))
            for r in recs:
                fp.write(r)
        print(f"wrote {path} ({len(recs)} records)")

    print("\nDivergence is engineered at record index 5 (seq 5):")
    print("  both sides: retired  pc=8130000C insn=38630002 (addi r3,r3,2)")
    print("  runtime r3=00000003, oracle r3=00000099  -> Tier-2 register divergence")


if __name__ == "__main__":
    main()
