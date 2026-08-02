#!/usr/bin/env python3
"""Emit live-byte validation tables for a DolRecomp DOL module.

Adapted from ModernGekko/RecompCore module-template/gen_module_tables.py at
ModernGekko dda273bddf486063df0b9c3c8dc2ca479f8d0180. GPL-3.0-or-later.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

FNV64_OFFSET = 0xCBF29CE484222325
FNV64_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1


def fnv1a64(data: bytes) -> int:
    value = FNV64_OFFSET
    for byte in data:
        value = ((value ^ byte) * FNV64_PRIME) & MASK64
    return value


def dol_reader(path: Path):
    image = path.read_bytes()

    def be32(offset: int) -> int:
        return int.from_bytes(image[offset : offset + 4], "big")

    sections = []
    for index in range(7):
        file_offset = be32(index * 4)
        address = be32(0x48 + index * 4)
        size = be32(0x90 + index * 4)
        if file_offset and address and size:
            sections.append((address, address + size, file_offset))

    def read(start: int, end: int) -> bytes:
        for section_start, section_end, file_offset in sections:
            if section_start <= start and end <= section_end:
                begin = file_offset + start - section_start
                return image[begin : begin + end - start]
        raise ValueError(
            f"range [0x{start:08X},0x{end:08X}) is not within a DOL text section"
        )

    return read


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--generated-header", type=Path, required=True)
    parser.add_argument("--dol", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    header = args.generated_header.read_text(encoding="utf-8")
    code_ranges = sorted(
        {
            (int(start, 16), int(end, 16))
            for start, end in re.findall(
                r"address >= (0x[0-9A-Fa-f]+)u && "
                r"address < (0x[0-9A-Fa-f]+)u",
                header,
            )
        }
    )
    starts = sorted(
        {
            int(address, 16)
            for address in re.findall(
                r"void func_([0-9A-Fa-f]{8})\(CPUState\* ctx\);", header
            )
        }
    )
    if not code_ranges or not starts:
        raise SystemExit("generated header contains no dispatch coverage")

    chunks = []
    for index, start in enumerate(starts):
        containing = next(
            ((low, high) for low, high in code_ranges if low <= start < high),
            None,
        )
        if containing is None:
            raise SystemExit(f"func_{start:08X} is outside all code ranges")
        end = containing[1]
        if index + 1 < len(starts) and starts[index + 1] < end:
            end = starts[index + 1]
        chunks.append((start, end))

    read_dol = dol_reader(args.dol)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as output:
        output.write("/* Generated content-identity table; do not edit. */\n")
        output.write("static const GcnAotRange s_title_chunks[] = {\n")
        for start, end in chunks:
            output.write(f"    {{0x{start:08X}u, 0x{end:08X}u}},\n")
        output.write("};\n")
        output.write("static const u64 s_title_hashes[] = {\n")
        for start, end in chunks:
            output.write(f"    0x{fnv1a64(read_dol(start, end)):016X}ull,\n")
        output.write("};\n")
        output.write(f"#define GCN_TITLE_CHUNK_COUNT {len(chunks)}u\n")
    print(f"title module: {len(chunks)} content-validated chunks -> {args.output}")


if __name__ == "__main__":
    main()
