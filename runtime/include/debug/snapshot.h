/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SNAPSHOT_RESUME (docs/SNAPSHOT_RESUME.md) — SAVE (pass A) and RESTORE
 * (pass B). Pass A captures the full guest-visible machine state at a
 * parked dispatcher-boundary checkpoint (debug_server.c's GCN_CHECKPOINT_PC
 * mechanism) to a versioned binary blob. Pass B (GCN_SNAPSHOT_LOAD=<path> in
 * boot.c) overlays that blob onto a freshly device-constructed process and
 * resumes gcn_dispatch_run from the restored PC.
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
#include "debug/debug_server.h"   /* GcnDebugCtx — gcn_snapshot_load's device registry param */

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
    GCN_SNAPSHOT_ERR_IO = 2,            /* fopen/fwrite/fread failed          */
    GCN_SNAPSHOT_ERR_NO_DSP = 3,        /* dsp_lle_save_state found no core   */
    GCN_SNAPSHOT_ERR_ALLOC = 4,         /* out of memory building the blob    */
    GCN_SNAPSHOT_ERR_FORMAT = 5,        /* bad magic/version/footer CRC       */
    GCN_SNAPSHOT_ERR_IDENTITY = 6,      /* disc-size identity mismatch, no
                                          * GCN_SNAPSHOT_FORCE               */
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

/* Restore the full SAVE-side machine state from `path`, overlaying it onto
 * `cpu` and every device boot.c has ALREADY constructed and MMIO-registered
 * (boot.c's normal device-construction sequence must run first, unmodified —
 * this only overlays state on top of it, it never constructs a device or
 * wires a callback itself). `ctx` is boot.c's OWN GcnDebugCtx — the exact
 * struct it builds to hand to gcn_debug_server_start — passed DIRECTLY
 * rather than looked up via gcn_debug_server_ctx(), because that registry is
 * only populated when GCN_DEBUG_PORT is set (gcn_debug_server_start returns
 * early otherwise) and a restore must work with or without the debug server
 * enabled. After a GCN_SNAPSHOT_OK return, cpu->pc is the restored PC and
 * the caller should proceed straight into gcn_dispatch_run — no BS1/BS2
 * seeding or further setup is needed.
 *
 * Refusal gates (loud, no partial overlay attempted): magic/version
 * mismatch or footer CRC32 mismatch always refuse (GCN_SNAPSHOT_ERR_FORMAT —
 * the blob is structurally suspect, force cannot help). A disc-size
 * identity mismatch (the DI section's captured disc_size vs. the disc
 * boot.c actually just mounted via GCN_DISC) refuses unless
 * GCN_SNAPSHOT_FORCE=1 is set (GCN_SNAPSHOT_ERR_IDENTITY) — documented as
 * an iteration-tier override, never appropriate for the production tier
 * (see docs/SNAPSHOT_RESUME.md's two-tier split). This is a provisional,
 * disc-SIZE-only identity check (see the content_identity_crc32 doc comment
 * above) pending real DOL/disc-hash infrastructure.
 *
 * Also performs the RESET-side host-memo fixups a restore needs that a
 * fresh process boot doesn't (native-code/AOT re-invalidation, ring/
 * census/texel-cache resets — see snapshot.c for the exact call list and
 * hazard citations); callers do not need to invoke those separately. */
int gcn_snapshot_load(const char* path, CPUState* cpu, const GcnDebugCtx* ctx,
                      char* why, size_t why_size);

#ifdef __cplusplus
}
#endif

#endif /* GCN_DEBUG_SNAPSHOT_H */
