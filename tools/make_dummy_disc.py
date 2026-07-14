#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generate a minimal, deterministic GameCube disc-image FIXTURE for M5 (the
# disc-load screen milestone). This is a test asset, not emulation fakery:
# the SAME image is mounted by BOTH the runtime (GCN_DISC) and the Dolphin
# oracle (--boot-gc-ipl <region> --disc <path>), so the value+order oracle
# diff over the DI interaction remains a like-for-like comparison — neither
# side is fed bytes the other can't see. Layout per Dolphin DiscIO
# (oracle/dolphin/Source/Core/DiscIO/DiscUtils.h + VolumeGC.cpp):
#   0x000  game ID (6): "GDUE00"  (fixture code, NTSC-U region byte E)
#   0x006  disc number 0, revision 0
#   0x01C  GAMECUBE_DISC_MAGIC 0xC2339F3D (big-endian)
#   0x020  internal name: "gcnrecomp M5 dummy disc"
#   0x420  apploader/DOL/FST offsets — left ZERO on purpose: M5's scope ends
#          at the disc-load SCREEN; the IPL reading a null apploader is the
#          loud, natural boundary where "no game loading yet" begins.
#   0x458  BI2 region: 1 (NTSC-U)
# The file is written SPARSE at MINI_DVD_SIZE (1,459,978,240 bytes) so DI
# end-of-disc bounds behave exactly like a real mini-DVD without storing
# 1.4 GB of zeros (NTFS sparse; the generator sets the length only).
#
# Usage: python tools/make_dummy_disc.py [out.iso]   (default _work/dummy.iso)
import struct
import sys

MINI_DVD_SIZE = 1_459_978_240
GAMECUBE_DISC_MAGIC = 0xC2339F3D
GAME_ID = b"GDUE00"
BI2_REGION_NTSC_U = 1

out = sys.argv[1] if len(sys.argv) > 1 else "_work/dummy.iso"

hdr = bytearray(0x2440)
hdr[0x000:0x006] = GAME_ID
hdr[0x006] = 0            # disc number
hdr[0x007] = 0            # revision
hdr[0x01C:0x020] = struct.pack(">I", GAMECUBE_DISC_MAGIC)
name = b"gcnrecomp M5 dummy disc"
hdr[0x020:0x020 + len(name)] = name
hdr[0x458:0x45C] = struct.pack(">I", BI2_REGION_NTSC_U)

with open(out, "wb") as f:
    f.write(hdr)
    f.truncate(MINI_DVD_SIZE)   # sparse tail: real mini-DVD extent, no bulk

print(
    f"wrote {out} ({MINI_DVD_SIZE} bytes sparse, id {GAME_ID.decode()}, "
    f"BI2 region {BI2_REGION_NTSC_U}, magic {GAMECUBE_DISC_MAGIC:#010x})"
)
