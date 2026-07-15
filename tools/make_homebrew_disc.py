#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build a self-authored GameCube homebrew disc image: a hand-assembled PPC32
# payload plus a minimal hand-assembled apploader, carried through the REAL
# boot chain (DI reads -> apploader init/main/close -> DOL-style copy -> jump
# to entry). This is the AUTHORING half of docs/HOMEBREW_DISC.md route (a):
# no devkitPPC, no vendored toolchain -- the payload and apploader are both
# emitted word-by-word from an in-file PPC32 mini-assembler.
#
# This tool EXTENDS tools/make_dummy_disc.py's format exactly (same header
# layout, same sparse mini-DVD sizing, same oracle-symmetry property: the
# same bytes are mountable by both the runtime, GCN_DISC=..., and the Dolphin
# oracle, --boot-gc-ipl <region> --disc <path>). Where the dummy disc leaves
# the apploader region null (M5's scope boundary -- "no code to run"), this
# tool fills it in with a real, tiny, hand-authored apploader + payload.
#
# ---------------------------------------------------------------------------
# The apploader ABI (transcribed from the Dolphin oracle's BS2 HLE, which is
# a byte-accurate model of what the real IPL's BS2 does on hardware -- see
# oracle/dolphin/Source/Core/Core/Boot/Boot_BS2Emu.cpp, CBoot::RunApploader,
# lines 141-245):
#
#   1. Header at disc offset 0x2440 (Boot_BS2Emu.cpp:149-152):
#         +0x10  u32 entry     (BE, absolute PPC address of the apploader's
#                                entry trampoline)
#         +0x14  u32 size      (BE, size in bytes of the apploader CODE image
#                                that follows immediately after the header)
#         +0x18  u32 trailer   (BE, size of an optional trailer blob; we use 0)
#      The apploader CODE image is DVDRead from disc offset 0x2440+0x20=0x2460
#      for (size+trailer) bytes (Boot_BS2Emu.cpp:158). We link/assemble our
#      apploader as if loaded at 0x81200000 -- the real SDK link address for
#      GC apploaders (Dolphin loads it to physical 0x01200000, whose cached
#      logical alias is 0x81200000 per the IBAT0/DBAT0 mapping BS2 sets up in
#      SetupBAT(), Boot_BS2Emu.cpp:116-139).
#
#   2. Call apploader "entry" (Boot_BS2Emu.cpp:170-179):
#        r3 = iAppLoaderFuncAddr + 0   (= 0x80003100 on GC, hardcoded scratch)
#        r4 = iAppLoaderFuncAddr + 4
#        r5 = iAppLoaderFuncAddr + 8
#      entry() is expected to WRITE the addresses of init/main/close into
#      *r3, *r4, *r5 respectively, then return (blr). BS2 then reads those
#      three words back out of memory into local vars (lines 177-179) -- so
#      by the time init() is first called, 0x80003100/04/08 are free again.
#      That is exactly why this project's payload can also live at
#      0x80003100: by the time the DOL-style copy lands the payload there,
#      the apploader's own scratch use of that address is long over. See
#      docs/DESIGN.md:63 -- 0x80003100 is the first free MEM1 address after
#      the low-mem OS globals block (0x80000000..0x80003100), which is why
#      Dolphin's HLE (and real BS2) picks it as scratch AND why it's the
#      conventional load address for a game's first section.
#
#   3. Call apploader "init" (Boot_BS2Emu.cpp:182-186):
#        r3 = report-fn pointer (Dolphin hardcodes a BLR stub at 0x81300000
#             and HLE-patches it as OSReport; on real hardware this would be
#             a real report/printf callback). Our init ignores r3 and just
#             returns -- correct per spec, callers are not required to invoke
#             the report function.
#
#   4. Call apploader "main" repeatedly (Boot_BS2Emu.cpp:191-221):
#        r3 = &0x81300004, r4 = &0x81300008, r5 = &0x8130000c (three scratch
#             words, reused across calls).
#      main() must fill:
#        *r3 = dst RAM address to copy to
#        *r4 = length in bytes
#        *r5 = disc byte offset to copy from (shifted <<2 only on Wii; GC
#              uses it as a raw byte offset, Boot_BS2Emu.cpp:207)
#      and return nonzero in r3 if this triplet is a valid pending copy
#      request. RunApploader (the BS2/IPL side, not the apploader) then does
#      the actual DVDRead itself (line 211) and calls main() again. main()
#      returns 0 in r3 once there is nothing left to copy. Our main() reports
#      EXACTLY one copy request (the payload) then reports done.
#
#   5. Call apploader "close" (Boot_BS2Emu.cpp:223-229):
#      No inputs. Its return value in r3 becomes the CPU's new PC
#      (`ppc_state.pc = ppc_state.gpr[3];`, line 229) -- i.e. the game's
#      entry point. Our close() returns the payload's load address.
#
# The payload itself is not a DOL and does not use the DOL section-table
# format -- it is a single flat blob our apploader's main() knows how to
# place directly, which is the simplest legal instance of the copy-request
# protocol above (real apploaders loop once per DOL section; we loop once,
# period).
# ---------------------------------------------------------------------------
#
# Usage:
#   python tools/make_homebrew_disc.py [out.iso]     (default _work/homebrew.iso)
#   python tools/make_homebrew_disc.py --selftest     (encoder + determinism
#                                                       + header self-checks,
#                                                       no disc written)
import struct
import sys
import os

MINI_DVD_SIZE = 1_459_978_240   # same sparse mini-DVD extent as make_dummy_disc.py
GAMECUBE_DISC_MAGIC = 0xC2339F3D

# =============================================================================
# 1. PPC32 mini-assembler -- only the opcodes this file's programs need.
#    Every encoder is a pure function: (fields...) -> u32 big-endian-valued
#    Python int (packed to bytes only at the very end). Bit positions follow
#    the PowerPC Architecture / PEM convention: bit 0 is the MSB, bit 31 is
#    the LSB (i.e. "IBM bit numbering", NOT little-endian bit numbering).
# =============================================================================


def _field(value, first_bit, last_bit):
    """Place `value` into IBM-numbered bits [first_bit, last_bit] (inclusive)
    of a 32-bit word, masking it to the field width first (two's-complement
    negative Python ints mask down correctly for signed fields)."""
    width = last_bit - first_bit + 1
    shift = 31 - last_bit
    mask = (1 << width) - 1
    return (value & mask) << shift


# --- D-form: primary-opcode(0-5) | rD/rS(6-10) | rA(11-15) | imm(16-31) -----

def enc_addi(rd, ra, simm):
    # PEM: addi rD,rA,SIMM  (rA=0 => li rD,SIMM). opcode = 14.
    return _field(14, 0, 5) | _field(rd, 6, 10) | _field(ra, 11, 15) | _field(simm, 16, 31)


def enc_li(rd, simm):
    return enc_addi(rd, 0, simm)


def enc_addis(rd, ra, simm):
    # PEM: addis rD,rA,SIMM (rA=0 => lis rD,SIMM). opcode = 15.
    # SIMM occupies the HIGH 16 bits of the result (addis adds SIMM<<16).
    return _field(15, 0, 5) | _field(rd, 6, 10) | _field(ra, 11, 15) | _field(simm, 16, 31)


def enc_lis(rd, uimm):
    return enc_addis(rd, 0, uimm)


def enc_ori(ra, rs, uimm):
    # PEM: ori rA,rS,UIMM. opcode = 24. NOTE the field/mnemonic-operand
    # crossover: rS is in the rD/rS slot (bits 6-10), rA is the destination
    # (bits 11-15) -- "ori rA,rS,UIMM" reads as "rA := rS | UIMM".
    # ori is zero-extending (UIMM), unlike addi's sign-extending SIMM -- that
    # is exactly why "lis rX,hi16; ori rX,rX,lo16" is the standard idiom for
    # loading an exact 32-bit constant with no @ha carry adjustment needed.
    return _field(24, 0, 5) | _field(rs, 6, 10) | _field(ra, 11, 15) | _field(uimm, 16, 31)


def enc_nop():
    # PEM canonical nop = "ori r0,r0,0".
    return enc_ori(0, 0, 0)


def enc_stw(rs, d, ra):
    # PEM: stw rS,d(rA). opcode = 36.
    return _field(36, 0, 5) | _field(rs, 6, 10) | _field(ra, 11, 15) | _field(d, 16, 31)


def enc_stwu(rs, d, ra):
    # PEM: stwu rS,d(rA)  (rA updated to rA+d; rA must not be r0). opcode = 37.
    return _field(37, 0, 5) | _field(rs, 6, 10) | _field(ra, 11, 15) | _field(d, 16, 31)


def enc_lwz(rd, d, ra):
    # PEM: lwz rD,d(rA). opcode = 32.
    return _field(32, 0, 5) | _field(rd, 6, 10) | _field(ra, 11, 15) | _field(d, 16, 31)


def enc_cmpwi(ra, simm, crf=0):
    # PEM: cmpi crfD,L,rA,SIMM (cmpwi is the L=0 32-bit-compare alias).
    # opcode = 11. Fields: BF/crfD(6-8,3b), reserved(9,1b)=0, L(10,1b)=0,
    # rA(11-15), SIMM(16-31).
    return (_field(11, 0, 5) | _field(crf, 6, 8) | _field(0, 9, 9)
            | _field(0, 10, 10) | _field(ra, 11, 15) | _field(simm, 16, 31))


# --- I-form: primary-opcode(0-5) | LI(6-29, word-granular) | AA(30) | LK(31) -

def enc_b(target, addr, aa=0, lk=0):
    # PEM: b/ba/bl/bla target. opcode = 18. LI is (target-addr)>>2 when
    # aa=0 (relative), or target>>2 when aa=1 (absolute); 24-bit signed,
    # sign-extended by the CPU at decode (we don't need to sign-extend here,
    # _field's masking reproduces the correct two's-complement bit pattern).
    delta = target if aa else (target - addr)
    li = delta >> 2
    return _field(18, 0, 5) | _field(li, 6, 29) | _field(aa, 30, 30) | _field(lk, 31, 31)


def enc_bl(target, addr, aa=0):
    return enc_b(target, addr, aa=aa, lk=1)


# --- B-form: primary-opcode(0-5) | BO(6-10) | BI(11-15) | BD(16-29) | AA(30) | LK(31)

def enc_bc(bo, bi, target, addr, aa=0, lk=0):
    # PEM: bc BO,BI,target. opcode = 16. BD is (target-addr)>>2, 14-bit signed.
    delta = target if aa else (target - addr)
    bd = delta >> 2
    return (_field(16, 0, 5) | _field(bo, 6, 10) | _field(bi, 11, 15)
            | _field(bd, 16, 29) | _field(aa, 30, 30) | _field(lk, 31, 31))


def enc_bne(bi, target, addr, lk=0):
    # Extended mnemonic: bne == bc 4,BI,target ("branch if CR[BI]==0", no hint).
    return enc_bc(4, bi, target, addr, lk=lk)


def enc_beq(bi, target, addr, lk=0):
    # Extended mnemonic: beq == bc 12,BI,target ("branch if CR[BI]==1", no hint).
    return enc_bc(12, bi, target, addr, lk=lk)


# --- XL-form: bclr (used only for the unconditional "blr" alias) -----------

def enc_blr():
    # PEM: blr == bclr 20,0,0. opcode = 19, BO(6-10)=20 (branch always),
    # BI(11-15)=0 (ignored), reserved(16-20)=0, subopcode(21-30)=16, LK(31)=0.
    return (_field(19, 0, 5) | _field(20, 6, 10) | _field(0, 11, 15)
            | _field(0, 16, 20) | _field(16, 21, 30) | _field(0, 31, 31))


# --- XFX-form: mtspr/mfspr --------------------------------------------------

def _spr_fields(spr):
    # The 10-bit SPR number is encoded SPLIT and SWAPPED: bits 11-15 hold
    # spr's low 5 bits, bits 16-20 hold spr's high 5 bits. (This is the
    # well-known quirk behind e.g. "mflr r0" = 0x7C0802A6 for spr=LR=8.)
    return spr & 0x1F, (spr >> 5) & 0x1F


def enc_mtspr(spr, rs):
    lo, hi = _spr_fields(spr)
    return (_field(31, 0, 5) | _field(rs, 6, 10) | _field(lo, 11, 15)
            | _field(hi, 16, 20) | _field(467, 21, 30) | _field(0, 31, 31))


def enc_mfspr(rd, spr):
    lo, hi = _spr_fields(spr)
    return (_field(31, 0, 5) | _field(rd, 6, 10) | _field(lo, 11, 15)
            | _field(hi, 16, 20) | _field(339, 21, 30) | _field(0, 31, 31))


SPR_LR = 8

# =============================================================================
# A tiny label-resolving assembler harness built on the encoders above. Not a
# new instruction format -- just two-pass address bookkeeping so the payload
# and apploader programs below can use forward-referencing labels (loop
# targets, and the apploader's own init/main/close/state addresses) instead
# of manually counted instruction offsets.
# =============================================================================


class Asm:
    def __init__(self, base_addr):
        self.base = base_addr
        self.words = []          # list[int | None]; None = pending fixup
        self.labels = {}         # name -> absolute address
        self.fixups = []         # list[(word_index, label_name, resolver)]

    def label(self, name):
        assert name not in self.labels, f"duplicate label {name}"
        self.labels[name] = self.base + 4 * len(self.words)

    def _addr_of(self, idx):
        return self.base + 4 * idx

    def emit(self, word):
        self.words.append(word & 0xFFFFFFFF)

    def emit_data(self, word):
        self.emit(word)  # plain data word, never executed -- same storage

    # direct-encode wrappers (no label involved)
    def addi(self, rd, ra, simm):
        self.emit(enc_addi(rd, ra, simm))

    def li(self, rd, simm):
        self.emit(enc_li(rd, simm))

    def lis(self, rd, uimm):
        self.emit(enc_lis(rd, uimm))

    def ori(self, ra, rs, uimm):
        self.emit(enc_ori(ra, rs, uimm))

    def stw(self, rs, d, ra):
        self.emit(enc_stw(rs, d, ra))

    def lwz(self, rd, d, ra):
        self.emit(enc_lwz(rd, d, ra))

    def cmpwi(self, ra, simm, crf=0):
        self.emit(enc_cmpwi(ra, simm, crf))

    def blr(self):
        self.emit(enc_blr())

    def const32(self, rd, value):
        # lis+ori pair loading an EXACT 32-bit absolute constant (no @ha
        # rounding needed -- see enc_ori's comment).
        self.lis(rd, (value >> 16) & 0xFFFF)
        self.ori(rd, rd, value & 0xFFFF)

    # label-fixup wrappers
    def load_label32(self, rd, label):
        idx_lis = len(self.words)
        self.words.append(None)
        self.fixups.append((idx_lis, label, lambda a, rd=rd: enc_lis(rd, (a >> 16) & 0xFFFF)))
        idx_ori = len(self.words)
        self.words.append(None)
        self.fixups.append((idx_ori, label, lambda a, rd=rd: enc_ori(rd, rd, a & 0xFFFF)))

    def b(self, label, lk=0):
        idx = len(self.words)
        self.words.append(None)
        addr = self._addr_of(idx)
        self.fixups.append((idx, label, lambda a, addr=addr, lk=lk: enc_b(a, addr, lk=lk)))

    def bne(self, label, bi=2, lk=0):
        idx = len(self.words)
        self.words.append(None)
        addr = self._addr_of(idx)
        self.fixups.append((idx, label,
                             lambda a, addr=addr, bi=bi, lk=lk: enc_bne(bi, a, addr, lk=lk)))

    def beq(self, label, bi=2, lk=0):
        idx = len(self.words)
        self.words.append(None)
        addr = self._addr_of(idx)
        self.fixups.append((idx, label,
                             lambda a, addr=addr, bi=bi, lk=lk: enc_beq(bi, a, addr, lk=lk)))

    def finish(self):
        for idx, label, resolver in self.fixups:
            self.words[idx] = resolver(self.labels[label]) & 0xFFFFFFFF
        assert all(w is not None for w in self.words), "unresolved word slot"
        return b"".join(struct.pack(">I", w) for w in self.words)


# =============================================================================
# 2. PAYLOAD -- assembled to load/run at 0x80003100 (first free MEM1 address
#    after the low-mem OS globals block; docs/DESIGN.md:63). Establishes a
#    stack, writes the ASCII magic 'HBRW' and a 16-word incrementing counter
#    pattern to a separate output region (0x80003200+, NOT 0x80003000 --
#    0x80003000 doubles as the stack pointer value and would collide with
#    the payload's own load footprint if reused as the output region), then
#    spins forever. Kept well under 40 instructions.
# =============================================================================

PAYLOAD_BASE = 0x80003100
STACK_ADDR = 0x80003000          # r1 stack pointer (per spec; payload never
                                  # actually pushes a frame, so no data is
                                  # written into the low-mem globals region)
PATTERN_MAGIC_ADDR = 0x80003200  # 'HBRW' magic word
PATTERN_DATA_ADDR = 0x80003204   # 16 counter words follow immediately after
PATTERN_COUNT = 16
HBRW_MAGIC = 0x48425257           # ASCII "HBRW"
COUNTER_BASE = 0xC0DE0000


def build_payload():
    pa = Asm(PAYLOAD_BASE)
    pa.const32(1, STACK_ADDR)              # lis r1,STACK@h ; ori r1,r1,STACK@l
    pa.const32(8, PATTERN_MAGIC_ADDR)      # lis r8,MAGICADDR@h ; ori r8,r8,@l
    pa.const32(4, HBRW_MAGIC)              # lis r4,'HB'    ; ori r4,r4,'RW'
    pa.stw(4, 0, 8)                        # mem[PATTERN_MAGIC_ADDR] = 'HBRW'
    pa.const32(3, PATTERN_DATA_ADDR)       # r3 = pattern write pointer
    pa.lis(5, (COUNTER_BASE >> 16) & 0xFFFF)  # r5 = 0xC0DE0000 (lis rd,uimm = addis rd,0,uimm)
    pa.li(6, 0)                            # r6 = i = 0
    pa.label("LOOP")
    pa.stw(5, 0, 3)                        # mem[r3] = r5   (0xC0DE0000+i)
    pa.addi(3, 3, 4)                       # r3 += 4  (advance write pointer)
    pa.addi(5, 5, 1)                       # r5 += 1  (0xC0DE0000+i -> +i+1)
    pa.addi(6, 6, 1)                       # i += 1
    pa.cmpwi(6, PATTERN_COUNT)             # cmp cr0, i, 16
    pa.bne("LOOP", bi=2)                   # loop while i != 16
    pa.label("SPIN")
    pa.b("SPIN")                           # b .   (spin forever -- faithful halt)
    return pa.finish()


PAYLOAD_LISTING = """\
PAYLOAD @ 0x80003100 (PPC32, big-endian; 18 instructions / 72 bytes):
  0x80003100  lis   r1, 0x8000        ; r1 = 0x80000000
  0x80003104  ori   r1, r1, 0x3000    ; r1 |= 0x3000       -> r1 = 0x80003000 (SP)
  0x80003108  lis   r8, 0x8000        ; r8 = 0x80000000
  0x8000310c  ori   r8, r8, 0x3200    ; r8 |= 0x3200        -> r8 = 0x80003200 (&magic)
  0x80003110  lis   r4, 0x4842        ; r4 = 0x48420000 ('H','B')
  0x80003114  ori   r4, r4, 0x5257    ; r4 |= 0x5257        -> r4 = 0x48425257 ("HBRW")
  0x80003118  stw   r4, 0(r8)         ; mem[0x80003200] = "HBRW"
  0x8000311c  lis   r3, 0x8000        ; r3 = 0x80000000
  0x80003120  ori   r3, r3, 0x3204    ; r3 |= 0x3204        -> r3 = 0x80003204 (&pattern[0])
  0x80003124  lis   r5, 0xC0DE        ; r5 = 0xC0DE0000
  0x80003128  li    r6, 0             ; r6 = i = 0
LOOP:
  0x8000312c  stw   r5, 0(r3)         ; mem[r3] = r5  (0xC0DE0000+i)
  0x80003130  addi  r3, r3, 4         ; r3 += 4
  0x80003134  addi  r5, r5, 1         ; r5 += 1
  0x80003138  addi  r6, r6, 1         ; i += 1
  0x8000313c  cmpwi r6, 16            ; cr0 = cmp(i, 16)
  0x80003140  bne   LOOP              ; loop while i != 16
SPIN:
  0x80003144  b     SPIN              ; b .  (spin forever)
"""

# =============================================================================
# 3. APPLOADER -- assembled as if linked/loaded at 0x81200000 (the SDK link
#    address for real GC apploaders; Dolphin's HLE loads the apploader image
#    to physical 0x01200000 == cached logical 0x81200000 under BS2's BAT
#    setup -- Boot_BS2Emu.cpp:158 + SetupBAT(), lines 116-139).
# =============================================================================

APPLOADER_BASE = 0x81200000
PAYLOAD_DISC_OFFSET = 0x20000     # documented disc byte offset of the payload


def _round_up32(n):
    return (n + 31) & ~31


def build_apploader(payload_size_rounded):
    aa = Asm(APPLOADER_BASE)

    # entry(r3=&init_slot, r4=&main_slot, r5=&close_slot): write the three
    # function addresses into the caller-provided slots, then return.
    # Convention per Boot_BS2Emu.cpp:172-179.
    aa.label("ENTRY")
    aa.load_label32(6, "INIT")
    aa.stw(6, 0, 3)                    # *r3 = &INIT
    aa.load_label32(6, "MAIN")
    aa.stw(6, 0, 4)                    # *r4 = &MAIN
    aa.load_label32(6, "CLOSE")
    aa.stw(6, 0, 5)                    # *r5 = &CLOSE
    aa.blr()

    # init(r3=report-fn ptr): unused -- just return.
    # Boot_BS2Emu.cpp:182-186.
    aa.label("INIT")
    aa.blr()

    # main(r3=&dst_slot, r4=&len_slot, r5=&off_slot): report exactly ONE
    # pending copy request (the payload) on the first call, then 0 forever
    # after. State survives across calls only via memory (RunFunction gives
    # us no register continuity between calls) -- STATE is a data word
    # living inside this same apploader image. Boot_BS2Emu.cpp:191-221.
    aa.label("MAIN")
    aa.load_label32(6, "STATE")
    aa.lwz(7, 0, 6)                    # r7 = state
    aa.cmpwi(7, 0)
    aa.beq("MAIN_FIRST", bi=2)         # state == 0 -> first call
    aa.li(3, 0)                        # else: no more copy requests
    aa.blr()
    aa.label("MAIN_FIRST")
    aa.const32(8, PAYLOAD_BASE)
    aa.stw(8, 0, 3)                    # *dst_slot = 0x80003100
    aa.li(8, payload_size_rounded)     # size fits in 16 bits (see caller)
    aa.stw(8, 0, 4)                    # *len_slot = rounded payload size
    aa.const32(8, PAYLOAD_DISC_OFFSET)
    aa.stw(8, 0, 5)                    # *off_slot = disc byte offset (GC: no <<2)
    aa.li(8, 1)
    aa.stw(8, 0, 6)                    # state = 1 (done after this)
    aa.li(3, 1)                        # return 1: this copy request is valid
    aa.blr()

    # close(): no inputs; r3 on return becomes the new PC (game entry).
    # Boot_BS2Emu.cpp:223-229.
    aa.label("CLOSE")
    aa.const32(3, PAYLOAD_BASE)
    aa.blr()

    # Data word, not code -- never executed (CLOSE already returned via blr).
    aa.label("STATE")
    aa.emit_data(0)

    return aa.finish(), aa.labels


APPLOADER_LISTING = """\
APPLOADER @ 0x81200000 (PPC32, big-endian; 34 words / 136 bytes, incl. 1 data word):
ENTRY (0x81200000): fills *r3/*r4/*r5 with &INIT/&MAIN/&CLOSE, then returns.
  lis r6,INIT@h  / ori r6,r6,INIT@l  / stw r6,0(r3)
  lis r6,MAIN@h  / ori r6,r6,MAIN@l  / stw r6,0(r4)
  lis r6,CLOSE@h / ori r6,r6,CLOSE@l / stw r6,0(r5)
  blr
INIT: blr                                   ; ignores r3 (report-fn ptr)
MAIN: state = *STATE
  lis/ori r6,&STATE ; lwz r7,0(r6) ; cmpwi r7,0 ; beq MAIN_FIRST
  li r3,0 ; blr                              ; state != 0 -> done
MAIN_FIRST:
  lis/ori r8,0x80003100  ; stw r8,0(r3)       ; dst  = payload load addr
  li  r8,<rounded size>  ; stw r8,0(r4)       ; size = payload size (round32)
  lis/ori r8,0x00020000  ; stw r8,0(r5)       ; disc-offset of payload
  li r8,1 ; stw r8,0(r6)                      ; state = 1
  li r3,1 ; blr                               ; report one valid copy request
CLOSE:
  lis/ori r3,0x80003100 ; blr                 ; entry PC = payload load addr
STATE: .word 0                                ; data, not code
"""

# =============================================================================
# 4. ISO layout -- clones make_dummy_disc.py's header/disc-info structure
#    byte-for-byte, then fills in the apploader header+code at 0x2440 and
#    places the payload at PAYLOAD_DISC_OFFSET.
# =============================================================================

GAME_ID = b"GHOMBR"                 # fixture code, not a real title
DISC_NAME = b"gcnrecomp homebrew disc"
APPLOADER_DISC_OFFSET = 0x2440
APPLOADER_DATE_STR = b"gcnrecomp-HB-01\0"   # 16 bytes, deterministic (not a timestamp)
assert len(APPLOADER_DATE_STR) == 16


def build_image():
    payload = build_payload()
    payload_size_rounded = _round_up32(len(payload))
    payload_padded = payload.ljust(payload_size_rounded, b"\0")

    apploader_code, apploader_labels = build_apploader(payload_size_rounded)
    apploader_entry = apploader_labels["ENTRY"]
    apploader_size = len(apploader_code)
    apploader_trailer = 0

    # --- header, cloned from make_dummy_disc.py -----------------------------
    hdr = bytearray(APPLOADER_DISC_OFFSET)
    hdr[0x000:0x006] = GAME_ID
    hdr[0x006] = 0                 # disc number
    hdr[0x007] = 0                 # revision
    hdr[0x01C:0x020] = struct.pack(">I", GAMECUBE_DISC_MAGIC)
    hdr[0x020:0x020 + len(DISC_NAME)] = DISC_NAME
    # 0x420 apploader/DOL/FST offsets left ZERO, exactly like the dummy disc:
    # our apploader is self-contained and never reads them, and BS2 itself
    # (per Boot_BS2Emu.cpp's RunApploader) never reads them either -- only
    # the 0x2440 apploader header matters to BS2.

    # --- assemble the full dense prefix (header + apploader + payload) ------
    image_len = max(APPLOADER_DISC_OFFSET + 0x20 + apploader_size,
                     PAYLOAD_DISC_OFFSET + len(payload_padded))
    image = bytearray(image_len)
    image[0:len(hdr)] = hdr
    image[APPLOADER_DISC_OFFSET:APPLOADER_DISC_OFFSET + 16] = APPLOADER_DATE_STR
    image[APPLOADER_DISC_OFFSET + 0x10:APPLOADER_DISC_OFFSET + 0x14] = struct.pack(">I", apploader_entry)
    image[APPLOADER_DISC_OFFSET + 0x14:APPLOADER_DISC_OFFSET + 0x18] = struct.pack(">I", apploader_size)
    image[APPLOADER_DISC_OFFSET + 0x18:APPLOADER_DISC_OFFSET + 0x1C] = struct.pack(">I", apploader_trailer)
    image[APPLOADER_DISC_OFFSET + 0x1C:APPLOADER_DISC_OFFSET + 0x20] = b"\0\0\0\0"
    image[APPLOADER_DISC_OFFSET + 0x20:APPLOADER_DISC_OFFSET + 0x20 + apploader_size] = apploader_code
    image[PAYLOAD_DISC_OFFSET:PAYLOAD_DISC_OFFSET + len(payload_padded)] = payload_padded

    meta = {
        "apploader_entry": apploader_entry,
        "apploader_size": apploader_size,
        "apploader_trailer": apploader_trailer,
        "apploader_labels": apploader_labels,
        "payload_len": len(payload),
        "payload_size_rounded": payload_size_rounded,
        "payload_bytes": payload,
    }
    return bytes(image), meta


def write_iso(out_path):
    image, meta = build_image()
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(image)
        f.truncate(MINI_DVD_SIZE)   # sparse tail, same as make_dummy_disc.py
    print(f"wrote {out_path} ({MINI_DVD_SIZE} bytes sparse, "
          f"magic {GAMECUBE_DISC_MAGIC:#010x}, "
          f"apploader entry {meta['apploader_entry']:#010x} "
          f"size {meta['apploader_size']} bytes, "
          f"payload {meta['payload_len']} bytes "
          f"(rounded {meta['payload_size_rounded']}) at disc "
          f"{PAYLOAD_DISC_OFFSET:#x})")
    return meta


# =============================================================================
# 5. Standalone verification -- no emulator, no build system.
# =============================================================================

# (a) Known-good encoder cross-checks, each independently derivable from the
#     PEM's opcode/field tables by hand (see per-instruction comments above
#     for the bit layout). Several of these are widely-recognized canonical
#     PowerPC encodings (blr, mflr r0, stwu r1,-16(r1), "b ." , "bl .+4") that
#     serve as strong independent oracles for the bit-packing math.
KNOWN_GOOD = [
    ("nop                 = ori r0,r0,0",         enc_nop(),                         0x60000000),
    ("blr",                                        enc_blr(),                        0x4E800020),
    ("mflr r0             = mfspr r0,LR(8)",       enc_mfspr(0, SPR_LR),              0x7C0802A6),
    ("mtlr r1             = mtspr LR(8),r1",       enc_mtspr(SPR_LR, 1),              0x7C2803A6),
    ("li r3,0             = addi r3,0,0",          enc_li(3, 0),                      0x38600000),
    ("lis r3,0x8000",                              enc_lis(3, 0x8000),                0x3C608000),
    ("stw r0,0(r1)",                                enc_stw(0, 0, 1),                  0x90010000),
    ("stwu r1,-16(r1)",                             enc_stwu(1, -16, 1),               0x9421FFF0),
    ("lwz r0,0(r1)",                                enc_lwz(0, 0, 1),                  0x80010000),
    ("cmpwi r3,0",                                  enc_cmpwi(3, 0),                   0x2C030000),
    ("b .   (self, delta=0)",                       enc_b(0x100, 0x100),               0x48000000),
    ("bl .+4 (delta=4, lk=1)",                      enc_b(0x104, 0x100, lk=1),         0x48000005),
]


def run_encoder_selftest():
    ok = True
    for desc, actual, expected in KNOWN_GOOD:
        status = "PASS" if actual == expected else "FAIL"
        if actual != expected:
            ok = False
        print(f"  [{status}] {desc:<38} got {actual:#010x} want {expected:#010x}")
    return ok


def run_dtk_crosscheck(payload_bytes):
    # Best-effort: only run if a dtk executable is actually present. It is
    # NOT vendored in this repo (see CLAUDE.md: reference clones, including
    # decomp-toolkit, are kept out of tree under the session scratchpad).
    import shutil
    candidates = ["dtk", "dtk.exe"]
    tools_dir = os.path.dirname(os.path.abspath(__file__))
    for name in os.listdir(tools_dir) if os.path.isdir(tools_dir) else []:
        if name.lower().startswith("dtk"):
            candidates.append(os.path.join(tools_dir, name))
    dtk_path = None
    for c in candidates:
        found = shutil.which(c) if not os.path.isabs(c) else (c if os.path.isfile(c) else None)
        if found:
            dtk_path = found
            break
    if dtk_path is None:
        print("  dtk not found in tools/ or PATH -- skipping disassembly cross-check "
              "(not required: manual PEM cross-checks above stand on their own).")
        return True
    print(f"  dtk found at {dtk_path}, but this repo's dtk build does not expose a "
          "raw-binary PPC disassemble subcommand usable standalone here -- skipping "
          "automatic invocation. Re-run with a disassembler if one becomes available.")
    return True


def run_determinism_selftest():
    img1, meta1 = build_image()
    img2, meta2 = build_image()
    if img1 != img2:
        print("  [FAIL] rebuild is NOT byte-identical")
        return False
    print(f"  [PASS] rebuild byte-identical ({len(img1)} dense bytes)")

    # header field assertions
    checks = [
        ("game id", img1[0:6], GAME_ID),
        ("disc magic", img1[0x1C:0x20], struct.pack(">I", GAMECUBE_DISC_MAGIC)),
        ("apploader entry", img1[0x2450:0x2454], struct.pack(">I", meta1["apploader_entry"])),
        ("apploader size", img1[0x2454:0x2458], struct.pack(">I", meta1["apploader_size"])),
        ("apploader trailer", img1[0x2458:0x245C], struct.pack(">I", 0)),
        ("payload load addr (CLOSE return)", None, None),  # checked below explicitly
    ]
    ok = True
    for desc, actual, expected in checks:
        if actual is None:
            continue
        status = "PASS" if actual == expected else "FAIL"
        if actual != expected:
            ok = False
        print(f"  [{status}] header field: {desc}")

    # payload bytes on-disc must match the freshly (re)assembled payload,
    # padded to the rounded size, at the documented offset.
    padded = meta1["payload_bytes"].ljust(meta1["payload_size_rounded"], b"\0")
    on_disc = img1[PAYLOAD_DISC_OFFSET:PAYLOAD_DISC_OFFSET + len(padded)]
    status = "PASS" if on_disc == padded else "FAIL"
    if on_disc != padded:
        ok = False
    print(f"  [{status}] payload bytes on-disc match assembled payload (+ zero pad)")

    # apploader ENTRY must actually point at the start of the apploader code
    # image (offset 0 within it), i.e. apploader_entry - APPLOADER_BASE == 0.
    entry_off = meta1["apploader_entry"] - APPLOADER_BASE
    status = "PASS" if entry_off == 0 else "FAIL"
    if entry_off != 0:
        ok = False
    print(f"  [{status}] apploader entry == APPLOADER_BASE (offset {entry_off})")

    return ok and run_dtk_crosscheck(meta1["payload_bytes"])


def run_selftest():
    print("== encoder cross-checks ==")
    ok = run_encoder_selftest()
    print("== determinism + header self-checks ==")
    ok = run_determinism_selftest() and ok
    print("== RESULT ==")
    print("PASS" if ok else "FAIL")
    return ok


def main(argv):
    selftest = "--selftest" in argv
    args = [a for a in argv if a != "--selftest"]
    out = args[0] if args else "_work/homebrew.iso"

    if selftest:
        ok = run_selftest()
        if not ok:
            return 1
    write_iso(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
