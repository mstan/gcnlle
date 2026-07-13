// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ExpansionPak
//
// Per-opcode cycle table — transcribed from Dolphin PPCTables.cpp
// GekkoOPTemplate `num_cycles` (master @ 2026-07-12, fetched from
// raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/Core/
// PowerPC/PPCTables.cpp). Every case below cites the Dolphin `opname` its
// value was read from, table by table (s_primary_table, s_table4/_2/_3,
// s_table19, s_table31, s_table59, s_table63/_2).
//
// This is the ORACLE's own timing model, deliberately: Dolphin is the
// independent oracle for this project (PRINCIPLES.md), so these numbers are
// transcribed as-is, never "improved" from a 750CXe hardware datasheet or
// any other source. Most real instructions cost 1 cycle in Dolphin's model;
// the exceptions (mulli, mulhwx/mulhwux/mullwx, divwx/divwux, fdivx/fdivsx,
// ps_div, lmw/stmw, sc/rfi/tw, dcbz/dcbf/dcbst/dcbi, sync/icbi, mfsr/mfsrin,
// mtspr, mtfsb0x/mtfsb1x/mtfsfix/mtfsfx, ps_rsqrte) are called out per case.
//
// The switch intentionally has NO `default:` label for real opcodes, only
// explicit cases for the two sentinels (PPC_OP_UNKNOWN, PPC_OP_COUNT). With
// -Wall/-Wextra (-Wswitch) this means the compiler warns if a future
// decoder.h addition isn't given a case here — the desired failure mode is a
// build warning, not a silently-wrong cycle charge.

#include "ppc_cycles.h"

u32 dr_ppc_num_cycles(PPCOpcode op) {
    switch (op) {
    /* Sentinels: never a real decoded instruction. */
    case PPC_OP_UNKNOWN:      return 1;

    /* s_primary_table */
    case PPC_OP_MULLI:        return 3;  /* mulli */
    case PPC_OP_SUBFIC:       return 1;  /* subfic */
    case PPC_OP_ADDI:         return 1;  /* addi */
    case PPC_OP_ADDIC:        return 1;  /* addic */
    case PPC_OP_ADDIC_DOT:    return 1;  /* addic_rc */
    case PPC_OP_ADDIS:        return 1;  /* addis */
    case PPC_OP_CMPI:         return 1;  /* cmpi */
    case PPC_OP_CMPLI:        return 1;  /* cmpli */
    case PPC_OP_ORI:          return 1;  /* ori */
    case PPC_OP_ORIS:         return 1;  /* oris */
    case PPC_OP_XORI:         return 1;  /* xori */
    case PPC_OP_XORIS:        return 1;  /* xoris */
    case PPC_OP_ANDI:         return 1;  /* andi_rc */
    case PPC_OP_ANDIS:        return 1;  /* andis_rc */
    case PPC_OP_LWZ:          return 1;  /* lwz */
    case PPC_OP_LWZU:         return 1;  /* lwzu */
    case PPC_OP_LBZ:          return 1;  /* lbz */
    case PPC_OP_LBZU:         return 1;  /* lbzu */
    case PPC_OP_STW:          return 1;  /* stw */
    case PPC_OP_STWU:         return 1;  /* stwu */
    case PPC_OP_STB:          return 1;  /* stb */
    case PPC_OP_STBU:         return 1;  /* stbu */
    case PPC_OP_LHZ:          return 1;  /* lhz */
    case PPC_OP_LHZU:         return 1;  /* lhzu */
    case PPC_OP_LHA:          return 1;  /* lha */
    case PPC_OP_LHAU:         return 1;  /* lhau */
    case PPC_OP_STH:          return 1;  /* sth */
    case PPC_OP_STHU:         return 1;  /* sthu */
    case PPC_OP_LMW:          return 11; /* lmw */
    case PPC_OP_STMW:         return 11; /* stmw */
    case PPC_OP_B:             return 1;  /* bx */
    case PPC_OP_BC:            return 1;  /* bcx */
    case PPC_OP_BCLR:          return 1;  /* bclrx (table19) */
    case PPC_OP_BCCTR:         return 1;  /* bcctrx (table19) */
    case PPC_OP_SC:            return 2;  /* sc */
    case PPC_OP_RFI:           return 2;  /* rfi (table19) */

    /* s_table19 (crops + isync + mcrf) */
    case PPC_OP_CRAND:         return 1;  /* crand */
    case PPC_OP_CRANDC:        return 1;  /* crandc */
    case PPC_OP_CREQV:         return 1;  /* creqv */
    case PPC_OP_CRNAND:        return 1;  /* crnand */
    case PPC_OP_CRNOR:         return 1;  /* crnor */
    case PPC_OP_CROR:          return 1;  /* cror */
    case PPC_OP_CRORC:         return 1;  /* crorc */
    case PPC_OP_CRXOR:         return 1;  /* crxor */
    case PPC_OP_MCRF:          return 1;  /* mcrf */
    case PPC_OP_ISYNC:         return 1;  /* isync */

    /* s_table31 (SPR/system/CR moves) */
    case PPC_OP_MFCR:          return 1;  /* mfcr */
    case PPC_OP_MTCRF:         return 1;  /* mtcrf */
    case PPC_OP_MFSPR:         return 1;  /* mfspr */
    case PPC_OP_MTSPR:         return 2;  /* mtspr */
    case PPC_OP_MFTB:          return 1;  /* mftb */
    case PPC_OP_CMP:           return 1;  /* cmp */
    case PPC_OP_CMPL:          return 1;  /* cmpl */
    case PPC_OP_TWI:           return 1;  /* twi (primary) */
    case PPC_OP_TW:            return 2;  /* tw (table31) */

    /* s_table31 integer arithmetic (all 1 cycle) */
    case PPC_OP_ADD:           return 1;  /* addx */
    case PPC_OP_ADDO:          return 1;  /* addox */
    case PPC_OP_ADDC:          return 1;  /* addcx */
    case PPC_OP_ADDCO:         return 1;  /* addcox */
    case PPC_OP_ADDE:          return 1;  /* addex */
    case PPC_OP_ADDEO:         return 1;  /* addeox */
    case PPC_OP_ADDME:         return 1;  /* addmex */
    case PPC_OP_ADDMEO:        return 1;  /* addmeox */
    case PPC_OP_ADDZE:         return 1;  /* addzex */
    case PPC_OP_ADDZEO:        return 1;  /* addzeox */
    case PPC_OP_SUBF:          return 1;  /* subfx */
    case PPC_OP_SUBFO:         return 1;  /* subfox */
    case PPC_OP_SUBFC:         return 1;  /* subfcx */
    case PPC_OP_SUBFCO:        return 1;  /* subfcox */
    case PPC_OP_SUBFE:         return 1;  /* subfex */
    case PPC_OP_SUBFEO:        return 1;  /* subfeox */
    case PPC_OP_SUBFME:        return 1;  /* subfmex */
    case PPC_OP_SUBFMEO:       return 1;  /* subfmeox */
    case PPC_OP_SUBFZE:        return 1;  /* subfzex */
    case PPC_OP_SUBFZEO:       return 1;  /* subfzeox */
    case PPC_OP_NEG:           return 1;  /* negx */
    case PPC_OP_NEGO:          return 1;  /* negox */
    case PPC_OP_AND:           return 1;  /* andx */
    case PPC_OP_ANDC:          return 1;  /* andcx */
    case PPC_OP_OR:            return 1;  /* orx */
    case PPC_OP_ORC:           return 1;  /* orcx */
    case PPC_OP_XOR:           return 1;  /* xorx */
    case PPC_OP_NAND:          return 1;  /* nandx */
    case PPC_OP_NOR:           return 1;  /* norx */
    case PPC_OP_EQV:           return 1;  /* eqvx */
    case PPC_OP_CNTLZW:        return 1;  /* cntlzwx */
    case PPC_OP_EXTSB:         return 1;  /* extsbx */
    case PPC_OP_EXTSH:         return 1;  /* extshx */
    case PPC_OP_SLW:           return 1;  /* slwx */
    case PPC_OP_SRW:           return 1;  /* srwx */
    case PPC_OP_SRAW:          return 1;  /* srawx */
    case PPC_OP_SRAWI:         return 1;  /* srawix */

    /* s_primary_table shift/rotate immediates (all 1 cycle) */
    case PPC_OP_RLWINM:        return 1;  /* rlwinmx */
    case PPC_OP_RLWNM:         return 1;  /* rlwnmx */
    case PPC_OP_RLWIMI:        return 1;  /* rlwimix */

    /* s_table31 indexed load/store (all 1 cycle) */
    case PPC_OP_LWZX:          return 1;  /* lwzx */
    case PPC_OP_LWZUX:         return 1;  /* lwzux */
    case PPC_OP_LBZX:          return 1;  /* lbzx */
    case PPC_OP_LBZUX:         return 1;  /* lbzux */
    case PPC_OP_LHZX:          return 1;  /* lhzx */
    case PPC_OP_LHZUX:         return 1;  /* lhzux */
    case PPC_OP_LHAX:          return 1;  /* lhax */
    case PPC_OP_LHAUX:         return 1;  /* lhaux */
    case PPC_OP_LWBRX:         return 1;  /* lwbrx */
    case PPC_OP_LHBRX:         return 1;  /* lhbrx */
    case PPC_OP_STWX:          return 1;  /* stwx */
    case PPC_OP_STWUX:         return 1;  /* stwux */
    case PPC_OP_STBX:          return 1;  /* stbx */
    case PPC_OP_STBUX:         return 1;  /* stbux */
    case PPC_OP_STHX:          return 1;  /* sthx */
    case PPC_OP_STHUX:         return 1;  /* sthux */
    case PPC_OP_STWBRX:        return 1;  /* stwbrx */
    case PPC_OP_STHBRX:        return 1;  /* sthbrx */
    case PPC_OP_LSWI:          return 1;  /* lswi */
    case PPC_OP_LSWX:          return 1;  /* lswx */
    case PPC_OP_STSWI:         return 1;  /* stswi */
    case PPC_OP_STSWX:         return 1;  /* stswx */
    case PPC_OP_LWARX:         return 1;  /* lwarx */
    case PPC_OP_STWCX:         return 1;  /* stwcxd */
    case PPC_OP_STFIWX:        return 1;  /* stfiwx */

    /* s_primary_table / s_table31 FP load/store (all 1 cycle) */
    case PPC_OP_LFS:           return 1;  /* lfs */
    case PPC_OP_LFSU:          return 1;  /* lfsu */
    case PPC_OP_LFD:           return 1;  /* lfd */
    case PPC_OP_LFDU:          return 1;  /* lfdu */
    case PPC_OP_STFS:          return 1;  /* stfs */
    case PPC_OP_STFSU:         return 1;  /* stfsu */
    case PPC_OP_STFD:          return 1;  /* stfd */
    case PPC_OP_STFDU:         return 1;  /* stfdu */
    case PPC_OP_LFSX:          return 1;  /* lfsx */
    case PPC_OP_LFSUX:         return 1;  /* lfsux */
    case PPC_OP_LFDX:          return 1;  /* lfdx */
    case PPC_OP_LFDUX:         return 1;  /* lfdux */
    case PPC_OP_STFSX:         return 1;  /* stfsx */
    case PPC_OP_STFSUX:        return 1;  /* stfsux */
    case PPC_OP_STFDX:         return 1;  /* stfdx */
    case PPC_OP_STFDUX:        return 1;  /* stfdux */

    /* s_table59 (single-precision FP) */
    case PPC_OP_FADDS:         return 1;  /* faddsx */
    case PPC_OP_FSUBS:         return 1;  /* fsubsx */
    case PPC_OP_FMULS:         return 1;  /* fmulsx */
    case PPC_OP_FDIVS:         return 17; /* fdivsx */
    case PPC_OP_FRES:          return 1;  /* fresx */
    case PPC_OP_FMADDS:        return 1;  /* fmaddsx */
    case PPC_OP_FMSUBS:        return 1;  /* fmsubsx */
    case PPC_OP_FNMADDS:       return 1;  /* fnmaddsx */
    case PPC_OP_FNMSUBS:       return 1;  /* fnmsubsx */

    /* s_table63 / s_table63_2 (double-precision FP) */
    case PPC_OP_FADD:          return 1;  /* faddx */
    case PPC_OP_FSUB:          return 1;  /* fsubx */
    case PPC_OP_FMUL:          return 1;  /* fmulx */
    case PPC_OP_FDIV:          return 31; /* fdivx */
    case PPC_OP_FRSQRTE:       return 1;  /* frsqrtex */
    case PPC_OP_FMADD:         return 1;  /* fmaddx */
    case PPC_OP_FMSUB:         return 1;  /* fmsubx */
    case PPC_OP_FNMADD:        return 1;  /* fnmaddx */
    case PPC_OP_FNMSUB:        return 1;  /* fnmsubx */
    case PPC_OP_FCTIW:         return 1;  /* fctiwx */
    case PPC_OP_FCTIWZ:        return 1;  /* fctiwzx */
    case PPC_OP_FMR:           return 1;  /* fmrx */
    case PPC_OP_FNEG:          return 1;  /* fnegx */
    case PPC_OP_FABS:          return 1;  /* fabsx */
    case PPC_OP_FNABS:         return 1;  /* fnabsx */
    case PPC_OP_FRSP:          return 1;  /* frspx */
    case PPC_OP_FSEL:          return 1;  /* fselx */
    case PPC_OP_FCMPU:         return 1;  /* fcmpu */
    case PPC_OP_FCMPO:         return 1;  /* fcmpo */
    case PPC_OP_MTFSB0:        return 3;  /* mtfsb0x */
    case PPC_OP_MTFSB1:        return 3;  /* mtfsb1x */
    case PPC_OP_MCRFS:         return 1;  /* mcrfs */
    case PPC_OP_MFFS:          return 1;  /* mffsx */
    case PPC_OP_MTFSF:         return 3;  /* mtfsfx */
    case PPC_OP_MTFSFI:        return 3;  /* mtfsfix */

    /* s_primary_table / s_table4_3 paired-single load/store (all 1 cycle) */
    case PPC_OP_PSQ_L:         return 1;  /* psq_l */
    case PPC_OP_PSQ_LU:        return 1;  /* psq_lu */
    case PPC_OP_PSQ_ST:        return 1;  /* psq_st */
    case PPC_OP_PSQ_STU:       return 1;  /* psq_stu */
    case PPC_OP_PSQ_LX:        return 1;  /* psq_lx */
    case PPC_OP_PSQ_LUX:       return 1;  /* psq_lux */
    case PPC_OP_PSQ_STX:       return 1;  /* psq_stx */
    case PPC_OP_PSQ_STUX:      return 1;  /* psq_stux */

    /* s_table4_2 (paired-single arithmetic) */
    case PPC_OP_PS_ADD:        return 1;  /* ps_add */
    case PPC_OP_PS_SUB:        return 1;  /* ps_sub */
    case PPC_OP_PS_MUL:        return 1;  /* ps_mul */
    case PPC_OP_PS_DIV:        return 17; /* ps_div */
    case PPC_OP_PS_RES:        return 1;  /* ps_res */
    case PPC_OP_PS_RSQRTE:     return 2;  /* ps_rsqrte */
    case PPC_OP_PS_MADD:       return 1;  /* ps_madd */
    case PPC_OP_PS_MSUB:       return 1;  /* ps_msub */
    case PPC_OP_PS_NMADD:      return 1;  /* ps_nmadd */
    case PPC_OP_PS_NMSUB:      return 1;  /* ps_nmsub */

    /* s_table4 (paired-single move/merge/compare) */
    case PPC_OP_PS_NEG:        return 1;  /* ps_neg */
    case PPC_OP_PS_ABS:        return 1;  /* ps_abs */
    case PPC_OP_PS_NABS:       return 1;  /* ps_nabs */
    case PPC_OP_PS_MR:         return 1;  /* ps_mr */
    case PPC_OP_PS_MERGE00:    return 1;  /* ps_merge00 */
    case PPC_OP_PS_MERGE01:    return 1;  /* ps_merge01 */
    case PPC_OP_PS_MERGE10:    return 1;  /* ps_merge10 */
    case PPC_OP_PS_MERGE11:    return 1;  /* ps_merge11 */
    case PPC_OP_PS_CMPU0:      return 1;  /* ps_cmpu0 */
    case PPC_OP_PS_CMPO0:      return 1;  /* ps_cmpo0 */
    case PPC_OP_PS_CMPU1:      return 1;  /* ps_cmpu1 */
    case PPC_OP_PS_CMPO1:      return 1;  /* ps_cmpo1 */

    /* s_table4_2 (paired-single, cont'd) */
    case PPC_OP_PS_SUM0:       return 1;  /* ps_sum0 */
    case PPC_OP_PS_SUM1:       return 1;  /* ps_sum1 */
    case PPC_OP_PS_MULS0:      return 1;  /* ps_muls0 */
    case PPC_OP_PS_MULS1:      return 1;  /* ps_muls1 */
    case PPC_OP_PS_MADDS0:     return 1;  /* ps_madds0 */
    case PPC_OP_PS_MADDS1:     return 1;  /* ps_madds1 */
    case PPC_OP_PS_SEL:        return 1;  /* ps_sel */

    /* s_table31 multiply/divide */
    case PPC_OP_MULLW:         return 5;  /* mullwx */
    case PPC_OP_MULLWO:        return 5;  /* mullwox */
    case PPC_OP_MULHW:         return 5;  /* mulhwx */
    case PPC_OP_MULHWU:        return 5;  /* mulhwux */
    case PPC_OP_DIVW:          return 40; /* divwx */
    case PPC_OP_DIVWO:         return 40; /* divwox */
    case PPC_OP_DIVWU:         return 40; /* divwux */
    case PPC_OP_DIVWUO:        return 40; /* divwuox */

    /* s_table31 / s_table4 cache management */
    case PPC_OP_DCBZ:          return 5;  /* dcbz (table31) */
    case PPC_OP_DCBZ_L:        return 1;  /* dcbz_l (table4) */
    case PPC_OP_DCBST:         return 5;  /* dcbst */
    case PPC_OP_DCBF:          return 5;  /* dcbf */
    case PPC_OP_DCBTST:        return 2;  /* dcbtst */
    case PPC_OP_DCBT:          return 2;  /* dcbt */
    case PPC_OP_DCBI:          return 5;  /* dcbi */
    case PPC_OP_ICBI:          return 4;  /* icbi */
    case PPC_OP_SYNC:          return 3;  /* sync */
    case PPC_OP_EIEIO:         return 1;  /* eieio (unused on GC per Dolphin comment) */

    /* s_table31 system/MMU registers */
    case PPC_OP_MCRXR:         return 1;  /* mcrxr */
    case PPC_OP_MFMSR:         return 1;  /* mfmsr */
    case PPC_OP_MTMSR:         return 1;  /* mtmsr */
    case PPC_OP_MFSR:          return 3;  /* mfsr */
    case PPC_OP_MFSRIN:        return 3;  /* mfsrin */
    case PPC_OP_MTSR:          return 1;  /* mtsr */
    case PPC_OP_MTSRIN:        return 1;  /* mtsrin */
    case PPC_OP_TLBIE:         return 1;  /* tlbie (unused on GC per Dolphin comment) */
    case PPC_OP_TLBSYNC:       return 1;  /* tlbsync (unused on GC per Dolphin comment) */
    case PPC_OP_ECIWX:         return 1;  /* eciwx (unused on GC per Dolphin comment) */
    case PPC_OP_ECOWX:         return 1;  /* ecowx (unused on GC per Dolphin comment) */

    /* Sentinel: one-past-last, never a real decoded instruction. */
    case PPC_OP_COUNT:         return 1;
    }

    /* Unreachable for any value in the PPCOpcode enum — every member is
     * handled above. This silences -Wreturn-type for values outside the
     * enum's defined range without adding a `default:` label (which would
     * defeat -Wswitch's protection against a future unmapped opcode). */
    return 1;
}
