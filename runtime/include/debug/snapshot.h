/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SNAPSHOT_RESUME pass A (docs/SNAPSHOT_RESUME.md) — SAVE-side only. Captures
 * the full guest-visible machine state at a parked dispatcher-boundary
 * checkpoint (debug_server.c's GCN_CHECKPOINT_PC mechanism) to a versioned
 * binary blob. Restore (pass B) is a separate, not-yet-implemented pass; this
 * header/module intentionally exposes no load/restore entry point yet.
 *
 * Format (host-native byte order — this blob is written and read back by the
 * same process/build, never cross-endian, so there is no reason to pay the
 * BE-swap cost the guest MMIO layer pays for real Flipper fidelity):
 *
 *   Header:
 *     u32 magic              = GCN_SNAPSHOT_MAGIC ("GCNS")
 *     u32 format_version     = GCN_SNAPSHOT_FORMAT_VERSION
 *     char framework_commit[GCN_SNAPSHOT_COMMIT_LEN]  (null-padded; the
 *         GCN_BUILD_GIT_SHA compile definition CMake stamps onto gcn_boot,
 *         or "unknown" if git wasn't available at configure time)
 *     u32 content_identity_crc32  (see the field's own doc comment below —
 *         pass A has no DOL/apploader identity to hash yet, see the
 *         SNAPSHOT_RESUME survey finding #14; this is a provisional stand-in)
 *     u32 iteration_liberties_flags (GCN_SNAPSHOT_FLAG_*)
 *     u32 section_count
 *     [section_count] x { u32 tag; u64 offset; u64 length; }   (TOC)
 *   Sections (back to back, each ALSO self-length-prefixed, redundant with
 *   the TOC by design — a reader can walk them linearly without the TOC):
 *     u32 tag; u64 length; u8 payload[length];
 *   Footer:
 *     u32 crc32   (gcn_crc32 over every byte from offset 0 up to, but not
 *                  including, this field)
 *
 * content_identity_crc32: the spec calls for "a content hash of the
 * disc/DOL identity the snapshot was taken under" so restore can refuse
 * loudly on a mismatch. This runtime has no DOL/apploader-load path yet
 * (confirmed by survey: boot.c/seed.c only implement the BS1->BS2 handoff,
 * dispatch.c:609 — there is no 0x80003140-class DOL entry anywhere). Pass A
 * therefore hashes the captured MEM1 image itself as a PROVISIONAL identity
 * proxy (recorded for bookkeeping); pass B's restore-refusal gate will need
 * a real disc/DOL identity hash once that loading path exists — flagged
 * here rather than silently treated as equivalent.
 */
#ifndef GCN_DEBUG_SNAPSHOT_H
#define GCN_DEBUG_SNAPSHOT_H

#include "cpu/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GCN_SNAPSHOT_MAGIC           0x474E4353u  /* "GCNS" little-endian-in-file */
#define GCN_SNAPSHOT_FORMAT_VERSION  1u
#define GCN_SNAPSHOT_COMMIT_LEN      64u

/* Iteration-tier liberties recorded in the header per the spec's "Validation
 * gates" section ("liberties allowed... documented, not validated beyond
 * basic route equivalence"). Bit 0 is the only one pass A actually takes:
 * GcnSi.input (the debug-injected controller report) is reset to the
 * power-on-neutral report rather than saved/restored verbatim — task
 * instruction, matching the SAVE-side survey's own recommendation that
 * input injection is a design decision, not automatic SAVE. */
#define GCN_SNAPSHOT_FLAG_SI_INPUT_RESET_NEUTRAL  0x00000001u

enum {
    GCN_SNAPSHOT_OK = 0,
    GCN_SNAPSHOT_ERR_NOT_DRAINED = 1,   /* gcn_gx_confirm_drained failed      */
    GCN_SNAPSHOT_ERR_IO = 2,            /* fopen/fwrite failed                */
    GCN_SNAPSHOT_ERR_NO_DSP = 3,        /* dsp_lle_save_state found no core   */
    GCN_SNAPSHOT_ERR_ALLOC = 4,         /* out of memory building the blob    */
};

/* Capture the full SAVE-side machine state to `path`. MUST be called only at
 * a parked dispatcher-boundary checkpoint (cpu->pc stable, no guest
 * instruction mid-execution) — see debug_server.c's
 * gcn_debug_server_checkpoint_before_block, the only current caller.
 * Internally: gcn_gx_pipeline_drain() -> gcn_gx_confirm_drained() (refusing
 * loudly, no file written, on any failure) -> serialize every device's SAVE
 * fields (docs/SNAPSHOT_RESUME.md survey) -> write the blob. On any non-OK
 * return, no (or no complete) file is left at `path`.
 *
 * `why` (may be NULL) receives a human-readable failure reason (e.g. the
 * exact drain-assert condition that failed), snprintf-truncated to
 * `why_size`. */
int gcn_snapshot_save(const char* path, CPUState* cpu, char* why, size_t why_size);

#ifdef __cplusplus
}
#endif

#endif /* GCN_DEBUG_SNAPSHOT_H */
