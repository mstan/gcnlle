/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Disc Interface (DI) model (impl). See include/di/di.h for scope and the exact
 * Dolphin DVDInterface functions/lines every register, bit and command response
 * is transcribed from. ROADMAP M5: a disc can now be mounted (gcn_di_set_disc);
 * IsDiscInside() reflects the live mount state instead of being hard-wired
 * false, and Read Sector / Read DiscID perform real seek+fread transfers from
 * the mounted image.
 */
#include "di/di.h"
#include "debug/rings.h"
#include "memory/memory.h"   /* gcn_mem_resolve — direct-into-guest-RAM disc reads */
#include "cpu/native_code.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* The dispatch loop ticks one DI (like the one VI); registered at init. */
static GcnDi* s_di = NULL;
static FILE* s_di_journal = NULL;
static u64 s_di_command_seq = 0;

#define GCN_DI_CORE_CLOCK_HZ          486000000ull
#define GCN_DI_MIN_COMMAND_LATENCY_US 300ull
#define GCN_DI_READ_COMMAND_LATENCY_US 600ull
#define GCN_DI_DVD_SECTOR_SIZE        0x800ull
#define GCN_DI_DVD_ECC_BLOCK_SIZE     (16ull * GCN_DI_DVD_SECTOR_SIZE)
#define GCN_DI_STREAMING_BUFFER_SIZE  (1024ull * 1024ull)
#define GCN_DI_BUFFER_TRANSFER_RATE   (32ull * 1024ull * 1024ull)
#define GCN_DI_BUFFER_BACK_SEEK_LIMIT (GCN_DI_DVD_ECC_BLOCK_SIZE * 2ull)

static u64 di_us_to_cycles(u64 us) {
    return us * (GCN_DI_CORE_CLOCK_HZ / 1000000ull);
}

static u64 di_align_down_u64(u64 v, u64 align) {
    return v - (v % align);
}

static u64 di_align_up_u64(u64 v, u64 align) {
    u64 rem = v % align;
    return rem ? v + (align - rem) : v;
}

/* Optional, append-only command ledger for title bring-up. This is intentionally
 * below the DI register interface: it records what the real IPL/apploader
 * programmed, but never changes command execution or timing. GCN_DI_LOG=1 also
 * mirrors each compact record to stderr for an interactive run. */
static void di_log_command(const GcnDi* di, const CPUState* cpu, u8 opcode,
                           u8 subcmd, u8 intr) {
    const char* journal = getenv("GCN_DI_JOURNAL");
    const char* loud = getenv("GCN_DI_LOG");
    u64 seq = ++s_di_command_seq;
    u64 dvd_offset = 0;
    u32 dvd_length = 0;
    if (opcode == GCN_DI_CMD_READ) {
        if (subcmd == 0x40u) {
            dvd_length = 0x20u;
        } else if (subcmd == 0x00u) {
            dvd_offset = (u64)di->cmdbuf[1] << 2;
            dvd_length = di->cmdbuf[2];
        }
    }

    if (journal && *journal && !s_di_journal)
        s_di_journal = fopen(journal, "ab");
    if (s_di_journal) {
        fprintf(s_di_journal,
                "{\"seq\":%llu,\"pc\":%u,\"opcode\":%u,\"subcmd\":%u,"
                "\"cmdbuf1\":%u,\"cmdbuf2\":%u,\"dvd_offset\":%llu,"
                "\"dvd_length\":%u,\"dma_addr\":%u,\"dma_length\":%u,"
                "\"interrupt\":%u}\n",
                (unsigned long long)seq, cpu ? cpu->pc : 0u,
                (u32)opcode, (u32)subcmd, di->cmdbuf[1], di->cmdbuf[2],
                (unsigned long long)dvd_offset, dvd_length, di->dimar,
                di->dilength, (u32)intr);
        fflush(s_di_journal);
    }
    if (loud && *loud && *loud != '0') {
        fprintf(stderr,
                "[di-command] #%llu pc=%08X op=%02X/%02X disc=%08llX+%X "
                "dma=%08X+%X intr=%u\n",
                (unsigned long long)seq, cpu ? cpu->pc : 0u,
                (u32)opcode, (u32)subcmd, (unsigned long long)dvd_offset,
                dvd_length, di->dimar, di->dilength, (u32)intr);
        fflush(stderr);
    }
}

void gcn_di_set_irq(GcnDi* di, GcnDiIrqFn fn, void* user) {
    di->irq = fn;
    di->irq_user = user;
}

/* DVDInterface::IsDiscInside() == DVDThread::HasDisc() (DVDInterface.cpp:
 * 433-436). ROADMAP M5: reflects the live mount state (di.h's disc_present,
 * flipped by gcn_di_set_disc/gcn_di_eject_disc) instead of being hard-wired
 * false — the single switch that turns the whole no-disc command path into a
 * real drive. */
static int di_disc_inside(const GcnDi* di) { return di->disc_present; }

/* ---- interrupt line (DVDInterface.cpp UpdateInterrupts:632-642) ----
 * Level = OR of the four (status & mask) pairs across DISR and DICVR. Pushed to
 * the PI DI cause line on EVERY evaluation (not just edges): PI INTSR is W1C, so
 * an acked-but-still-asserted level must re-appear — exactly as in Dolphin. */
static void di_update_interrupts(GcnDi* di) {
    int level =
        ((di->disr  & GCN_DI_SR_DEINT)  && (di->disr  & GCN_DI_SR_DEINTMASK))  ||
        ((di->disr  & GCN_DI_SR_TCINT)  && (di->disr  & GCN_DI_SR_TCINTMASK))  ||
        ((di->disr  & GCN_DI_SR_BRKINT) && (di->disr  & GCN_DI_SR_BRKINTMASK)) ||
        ((di->dicvr & GCN_DI_CVR_CVRINT)&& (di->dicvr & GCN_DI_CVR_CVRINTMASK));

    if (di->irq)
        di->irq(di->irq_user, level);
    if (level != di->irq_level) {
        gcn_ring_event(level ? GCN_EV_IRQ_RAISE : GCN_EV_IRQ_CLEAR,
                       /*source*/ 2u /* PI cause bit index of INT_CAUSE_DI=0x4 */,
                       0u, 0u);
        di->irq_level = level;
    }
}

/* DVDInterface.cpp GenerateDIInterrupt:644-663 — latch a status bit + re-eval. */
static void di_generate_interrupt(GcnDi* di, u8 type) {
    switch (type) {
    case GCN_DI_INT_DEINT:  di->disr  |= GCN_DI_SR_DEINT;   break;
    case GCN_DI_INT_TCINT:  di->disr  |= GCN_DI_SR_TCINT;   break;
    case GCN_DI_INT_BRKINT: di->disr  |= GCN_DI_SR_BRKINT;  break;
    case GCN_DI_INT_CVRINT: di->dicvr |= GCN_DI_CVR_CVRINT; break;
    }
    di_update_interrupts(di);
}

/* DVDInterface.cpp SetLidOpen:529-535. CVR: 0=closed (disc present), 1=open
 * (no disc) — the same "cover open == no disc" approximation di_disc_inside's
 * doc comment notes Dolphin uses. An EDGE (open<->closed) raises CVRINT; this
 * is the interrupt the menu's disc-screen reaction polls for on a hot
 * insert/eject (debug_server.c "insert_disc"/"eject_disc"). */
static void di_set_lid_open(GcnDi* di) {
    u32 old = di->dicvr & GCN_DI_CVR_CVR;
    u32 want = di_disc_inside(di) ? 0u : GCN_DI_CVR_CVR;
    di->dicvr = (di->dicvr & ~GCN_DI_CVR_CVR) | want;
    if (want != old)
        di_generate_interrupt(di, GCN_DI_INT_CVRINT);
}

/* DVDInterface.cpp CheckReadPreconditions:706-738. With no disc the very first
 * check fires (MediumNotPresent); the rest are transcribed for M5 fidelity. */
static int di_check_read_preconditions(GcnDi* di) {
    if (!di_disc_inside(di)) {        /* implies CoverOpened or NoMediumPresent */
        di->error_code = GCN_DI_ERR_MEDIUM_NOT_PRESENT;
        return 0;
    }
    if (di->drive_state == GCN_DI_STATE_DISC_CHANGE) {
        di->error_code = GCN_DI_ERR_MEDIUM_CHANGED;
        return 0;
    }
    if (di->drive_state == GCN_DI_STATE_MOTOR_STOPPED) {
        di->error_code = GCN_DI_ERR_MOTOR_STOPPED;
        return 0;
    }
    if (di->drive_state == GCN_DI_STATE_DISC_ID_NOT_READ) {
        di->error_code = GCN_DI_ERR_NO_DISC_ID;
        return 0;
    }
    return 1;
}

static double di_disc_physical_position(u64 offset) {
    const double wii_layer_size = 0x118240000ull;
    const double inner_radius = 0.024;
    const double wii_outer_radius = 0.058;
    const u64 two_layers = 0x118240000ull * 2ull;

    offset %= two_layers;
    if (offset > 0x118240000ull)
        offset = two_layers - offset;

    return sqrt((double)offset / wii_layer_size *
                    (wii_outer_radius * wii_outer_radius - inner_radius * inner_radius) +
                inner_radius * inner_radius);
}

static double di_seek_time(u64 offset_from, u64 offset_to) {
    const double short_seek_max_distance = 0.001;
    const double short_seek_constant = 0.035;
    const double short_seek_velocity_inverse = 50.0;
    const double long_seek_constant = 0.075;
    const double long_seek_velocity_inverse = 4.5;

    double distance = fabs(di_disc_physical_position(offset_from) -
                           di_disc_physical_position(offset_to));
    if (distance < short_seek_max_distance)
        return distance * short_seek_velocity_inverse + short_seek_constant;
    return distance * long_seek_velocity_inverse + long_seek_constant;
}

static double di_rotational_latency(u64 offset, double time) {
    const double track_pitch = 0.00000074;
    const double rotations_per_second = 28.5; /* GameCube disc */
    const double target_angle = fmod(di_disc_physical_position(offset) / track_pitch, 1.0);
    const double start_angle = fmod(time * rotations_per_second, 1.0);
    const double angle_diff = fmod(target_angle + 1.0 - start_angle, 1.0);
    return angle_diff / rotations_per_second;
}

static double di_raw_disc_read_time(u64 offset, u64 length) {
    const double inner_radius = 0.024;
    const double gc_outer_radius = 0.038;
    const double inner_speed = 1024.0 * 1024.0 * 2.1;
    const double outer_speed = 1024.0 * 1024.0 * 3.325;
    double physical_offset = di_disc_physical_position(offset + length / 2ull);
    double speed = (physical_offset - inner_radius) / (gc_outer_radius - inner_radius) *
                       (outer_speed - inner_speed) +
                   inner_speed;
    if (speed < 1.0)
        speed = 1.0;
    return (double)length / speed;
}

static u64 di_seconds_to_cycles(double seconds) {
    if (seconds <= 0.0)
        return 0;
    return (u64)(seconds * (double)GCN_DI_CORE_CLOCK_HZ);
}

static u64 di_schedule_read_cycles(GcnDi* di, u64 offset, u32 length) {
    u64 current_time = di->now_cycles;
    int seek = 0;
    u64 head_position = 0;
    u64 buffer_start = 0;
    u64 buffer_end = 0;
    u64 dvd_offset = di_align_down_u64(offset, GCN_DI_DVD_ECC_BLOCK_SIZE);
    const u64 first_block = dvd_offset;
    u64 ticks_until_completion = di_us_to_cycles(GCN_DI_READ_COMMAND_LATENCY_US);

    if (di->read_buffer_start_time != di->read_buffer_end_time) {
        buffer_start = di->read_buffer_end_offset > GCN_DI_STREAMING_BUFFER_SIZE ?
                           di->read_buffer_end_offset - GCN_DI_STREAMING_BUFFER_SIZE :
                           0;

        if (current_time >= di->read_buffer_end_time) {
            buffer_end = di->read_buffer_end_offset;
        } else {
            u64 elapsed = current_time > di->read_buffer_start_time ?
                              current_time - di->read_buffer_start_time :
                              0;
            u64 duration = di->read_buffer_end_time - di->read_buffer_start_time;
            u64 span = di->read_buffer_end_offset - di->read_buffer_start_offset;
            u64 available = duration ? (elapsed * span / duration) : 0;
            buffer_end = di->read_buffer_start_offset +
                         di_align_down_u64(available, GCN_DI_DVD_ECC_BLOCK_SIZE);
        }
        head_position = buffer_end;
        if (dvd_offset < buffer_start)
            buffer_start = buffer_end = 0;
    }

    do {
        u64 next_boundary = di_align_up_u64(offset + 1ull, GCN_DI_DVD_ECC_BLOCK_SIZE);
        u32 chunk_length = (u32)(next_boundary - offset);
        if (chunk_length > length)
            chunk_length = length;

        if (dvd_offset >= buffer_start && dvd_offset < buffer_end) {
            ticks_until_completion +=
                (u64)chunk_length * GCN_DI_CORE_CLOCK_HZ / GCN_DI_BUFFER_TRANSFER_RATE;
        } else {
            if (dvd_offset != head_position) {
                seek = 1;
                ticks_until_completion += di_seconds_to_cycles(di_seek_time(head_position, dvd_offset));
                {
                    double time_after_seek =
                        (double)(current_time + ticks_until_completion) /
                        (double)GCN_DI_CORE_CLOCK_HZ;
                    ticks_until_completion +=
                        di_seconds_to_cycles(di_rotational_latency(dvd_offset, time_after_seek));
                }
            } else {
                ticks_until_completion +=
                    di_seconds_to_cycles(di_raw_disc_read_time(dvd_offset,
                                                               GCN_DI_DVD_ECC_BLOCK_SIZE));
            }
            head_position = dvd_offset + GCN_DI_DVD_ECC_BLOCK_SIZE;
        }

        offset += chunk_length;
        length -= chunk_length;
        dvd_offset += GCN_DI_DVD_ECC_BLOCK_SIZE;
    } while (length > 0);

    {
        const u64 last_block = dvd_offset;
        if (!(last_block - buffer_start <= GCN_DI_BUFFER_BACK_SEEK_LIMIT &&
              buffer_start != buffer_end)) {
            if (last_block >= buffer_end)
                di->read_buffer_start_offset = last_block;
            else
                di->read_buffer_start_offset = buffer_end;

            di->read_buffer_end_offset =
                last_block + GCN_DI_STREAMING_BUFFER_SIZE - GCN_DI_BUFFER_BACK_SEEK_LIMIT;
            if (seek) {
                u64 min_end = first_block + GCN_DI_STREAMING_BUFFER_SIZE;
                if (di->read_buffer_end_offset < min_end)
                    di->read_buffer_end_offset = min_end;
            }

            di->read_buffer_start_time = current_time + ticks_until_completion;
            di->read_buffer_end_time =
                di->read_buffer_start_time +
                di_seconds_to_cycles(di_raw_disc_read_time(
                    di->read_buffer_start_offset,
                    di->read_buffer_end_offset - di->read_buffer_start_offset));
        }
    }

    return ticks_until_completion ? ticks_until_completion : 1;
}

/* ExecuteReadCommand:741-777. The caller has already run CheckReadPreconditions.
 * Validate and schedule the read now, but do not expose data or TCINT until the
 * Dolphin-style drive timing expires in gcn_di_tick. */
static u8 di_prepare_read(GcnDi* di, CPUState* cpu, u64 dvd_offset, u32 dvd_length) {
    u32 output_length = di->dilength;
    if (dvd_length > output_length)          /* ExecuteReadCommand:757-762 clamp */
        dvd_length = output_length;

    /* Error #001 (ExecuteReadCommand:764-773): a read that runs past the end
     * of the disc. m_disc_end_offset there is the disc's own size for a mini-
     * DVD (DiscUtils.h MINI_DVD_SIZE); we use the mounted image's real size,
     * the equivalent fact for whatever's actually mounted. */
    if (dvd_offset + dvd_length > di->disc_size) {
        di->error_code = GCN_DI_ERR_BLOCK_OOB;
        return GCN_DI_INT_DEINT;
    }
    if (dvd_length == 0)
        return GCN_DI_INT_TCINT;             /* nothing to transfer; trivially done */

    if (!di->disc_file) {
        /* di_disc_inside() said yes but the handle is gone: unreachable in
         * practice (eject clears disc_present before closing the file) — loud
         * rather than a silent zero-fill if it ever happens. */
        fprintf(stderr, "gcn di: BUG — disc marked present with no open file\n");
        di->error_code = GCN_DI_ERR_MEDIUM_NOT_PRESENT;
        return GCN_DI_INT_DEINT;
    }

    u32 avail = 0;
    u8* dst = gcn_mem_resolve(cpu, di->dimar, &avail);
    if (!dst) {
        fprintf(stderr, "gcn di: DIMAR 0x%08X is not RAM-backed — disc read dropped\n",
                di->dimar);
        return GCN_DI_INT_DEINT;
    }
    (void)dst;
    if (avail < dvd_length) {
        fprintf(stderr, "gcn di: DIMAR 0x%08X + 0x%X exceeds MEM1 (avail 0x%X) — clamping\n",
                di->dimar, dvd_length, avail);
        dvd_length = avail;
    }

    di->pending_read_active = 1;
    di->pending_read_offset = dvd_offset;
    di->pending_read_length = dvd_length;
    di->pending_read_addr = di->dimar;
    di->pending_cycles = di_schedule_read_cycles(di, dvd_offset, dvd_length);
    return GCN_DI_INT_TCINT;
}

static void di_commit_pending_read(GcnDi* di, CPUState* cpu) {
    if (!di->pending_read_active)
        return;
    di->pending_read_active = 0;

    u64 dvd_offset = di->pending_read_offset;
    u32 dvd_length = di->pending_read_length;
    u32 dma_addr = di->pending_read_addr;
    if (!dvd_length)
        return;

    if (!di->disc_file) {
        fprintf(stderr, "gcn di: BUG â€” pending read with no open disc file\n");
        return;
    }

    u32 avail = 0;
    u8* dst = gcn_mem_resolve(cpu, dma_addr, &avail);
    if (!dst) {
        fprintf(stderr, "gcn di: DIMAR 0x%08X is not RAM-backed at completion â€” disc read dropped\n",
                dma_addr);
        return;
    }
    if (avail < dvd_length)
        dvd_length = avail;

    _fseeki64(di->disc_file, (long long)dvd_offset, SEEK_SET);
    size_t got = fread(dst, 1, dvd_length, di->disc_file);
    if (got < (size_t)dvd_length)
        /* Real hardware/Dolphin never reads short inside disc_size; the only
         * way we land here is the file being shorter than `disc_size` said
         * (shouldn't happen — disc_size IS the file's own on-open size) or a
         * host I/O error. Zero-fill rather than leave stale RAM, and say so. */
        memset(dst + got, 0, dvd_length - got);
    gcn_native_code_invalidate(dma_addr, dvd_length);

    /* M5 SCOPE BOUNDARY (di.h GCN_DI_APPLOADER_OFFSET): a real disc read just
     * landed the apploader in RAM. Loud, ONE-TIME, general — fires for any
     * disc's apploader read, not special-cased to the dummy disc — never a
     * silent "modeled further" fake. What the IPL does next (jump to the
     * apploader's own entry point, run it, load the game) is out of M5's
     * scope by design; expect a loud divergence from here on a null/dummy
     * apploader, not a silent hang. */
    if (dvd_offset == GCN_DI_APPLOADER_OFFSET) {
        static int noted = 0;
        if (!noted) {
            noted = 1;
            fprintf(stdout,
                "gcn di: NOTE — apploader read (fixed GC disc offset 0x2440) "
                "landed in guest RAM at 0x%08X (%u bytes). Continuing through "
                "the retail apploader and title as ordinary guest code. If "
                "this is the dummy disc (tools/make_dummy_disc.py — entry "
                "point 0 by construction), it still diverges loudly.\n",
                dma_addr, dvd_length);
            fflush(stdout);
        }
    }
}

/* DVDInterface.cpp ExecuteCommand:782-1234, restricted to the GameCube-retail
 * command set the IPL issues. Runs immediate register effects and returns the
 * interrupt type to raise on delayed completion. Read commands schedule a
 * Dolphin-style DVDThread delay and commit bytes in gcn_di_tick. */
static u8 di_execute_command(GcnDi* di, CPUState* cpu) {
    u8 intr = GCN_DI_INT_TCINT;
    u8 cmd  = (u8)(di->cmdbuf[0] >> 24);

    /* RequestError needs the error code the previous command set, so only
     * non-RequestError commands reset it here (ExecuteCommand:803-805). */
    if (cmd != GCN_DI_CMD_REQUEST_ERROR)
        di->error_code = GCN_DI_ERR_NONE;

    switch (cmd) {
    case GCN_DI_CMD_INQUIRY:                 /* ExecuteCommand:810-820 */
        /* Drive firmware ID (shuffle2's Wii dump). Faithful DMA into DIMAR. */
        mem_write32(cpu, di->dimar + 0, 0x00000002u);   /* revision / device code */
        mem_write32(cpu, di->dimar + 4, 0x20060526u);   /* release date */
        mem_write32(cpu, di->dimar + 8, 0x41000000u);   /* version */
        break;

    case GCN_DI_CMD_READ:                    /* ExecuteCommand:839-882 */
        switch (di->cmdbuf[0] & 0xFFu) {
        case 0x00: {                         /* Read Sector: ExecuteCommand:842-858 */
            u64 dvd_offset = (u64)di->cmdbuf[1] << 2;   /* DVDInterface.cpp:844 */
            if (di->drive_state == GCN_DI_STATE_READY_NO_READS_MADE)
                di->drive_state = GCN_DI_STATE_READY;
            if (!di_check_read_preconditions(di))   /* no disc/id/motor => DEINT */
                intr = GCN_DI_INT_DEINT;
            else
                intr = di_prepare_read(di, cpu, dvd_offset, di->cmdbuf[2]);
            break;
        }
        case 0x40:                           /* Read DiscID: ExecuteCommand:860-876 */
            if (di->drive_state == GCN_DI_STATE_DISC_ID_NOT_READ)
                di->drive_state = GCN_DI_STATE_READY_NO_READS_MADE;
            else if (di->drive_state == GCN_DI_STATE_READY_NO_READS_MADE)
                di->drive_state = GCN_DI_STATE_READY;
            if (!di_check_read_preconditions(di))   /* no disc => DEINT */
                intr = GCN_DI_INT_DEINT;
            else
                intr = di_prepare_read(di, cpu, 0, 0x20u);   /* fixed 0x20-byte disc header */
            break;
        default:                             /* unknown read subcommand: logged, TCINT */
            break;
        }
        break;

    case GCN_DI_CMD_SEEK:                     /* ExecuteCommand:885-889 (ignored, TCINT) */
        break;

    case GCN_DI_CMD_REQUEST_ERROR: {          /* ExecuteCommand:982-994 */
        u32 state = (di->drive_state == GCN_DI_STATE_READY)
                        ? 0u : (u32)di->drive_state - 1u;
        di->diimmbuf = (state << 24) | di->error_code;
        di->error_code = GCN_DI_ERR_NONE;
        break;
    }

    case GCN_DI_CMD_AUDIO_STREAM:             /* ExecuteCommand:1001-1064 */
    case GCN_DI_CMD_AUDIO_STATUS:             /* ExecuteCommand:1067-1119 */
        /* Both begin with CheckReadPreconditions (no disc/id/motor => DEINT),
         * then require enable_dtk (set by AudioBufferConfig below) or DEINT
         * with NoAudioBuf (ExecuteCommand:1009-1018/1076-1082). Past that
         * handshake Dolphin just flips m_stream / reports position — the
         * actual ADPCM streaming data path is NOT modeled (di.h DELIBERATELY
         * DEFERRED: out of M5 scope, needs a loaded game's audio content), so
         * we stop at the handshake and report the honest TCINT/DEINT the
         * handshake itself would produce, never fabricated stream data. */
        if (!di_check_read_preconditions(di)) {
            intr = GCN_DI_INT_DEINT;
        } else if (!di->enable_dtk) {
            di->error_code = GCN_DI_ERR_NO_AUDIO_BUF;
            intr = GCN_DI_INT_DEINT;
        }
        break;

    case GCN_DI_CMD_STOP_MOTOR:               /* ExecuteCommand:1122-1148 */
        if (di->drive_state == GCN_DI_STATE_READY ||
            di->drive_state == GCN_DI_STATE_READY_NO_READS_MADE ||
            di->drive_state == GCN_DI_STATE_DISC_ID_NOT_READ)
            di->drive_state = GCN_DI_STATE_MOTOR_STOPPED;
        /* Software-eject / auto-disc-change (force_eject / AutoChangeDisc,
         * ExecuteCommand:1134-1146) not modeled: multi-disc UX, irrelevant to
         * the single dummy disc M5 mounts (di.h DELIBERATELY DEFERRED). */
        break;

    case GCN_DI_CMD_AUDIO_CONFIG:             /* ExecuteCommand:1152-1178 */
        /* Requires a disc + not-yet-Ready; no disc => DEINT via the check,
         * Ready (a read already happened) => InvalidPeriod/DEINT
         * (ExecuteCommand:1166-1173) — this command is only valid immediately
         * after the first Read DiscID, before any other read. */
        if (!di_check_read_preconditions(di)) {
            intr = GCN_DI_INT_DEINT;
        } else if (di->drive_state == GCN_DI_STATE_READY) {
            di->error_code = GCN_DI_ERR_INVALID_PERIOD;
            intr = GCN_DI_INT_DEINT;
        } else {
            /* AudioBufferConfig(enable, len) DVDInterface.cpp:1273-1281. Only
             * the enable bit gates AUDIO_STREAM/AUDIO_STATUS above; the buffer
             * length byte has no modeled consumer (DTK data path deferred). */
            di->enable_dtk = (u8)((di->cmdbuf[0] >> 16) & 1u);
        }
        break;

    default:                                  /* ExecuteCommand default:1218-1224 */
        di->error_code = GCN_DI_ERR_INVALID_COMMAND;
        intr = GCN_DI_INT_DEINT;
        break;
    }

    return intr;
}

/* DVDInterface.cpp FinishExecutingCommand:1306-1353, ReplyType::Interrupt. On a
 * successful transfer (TCINT) DIMAR advances by DILENGTH and DILENGTH clears;
 * then, iff TSTART is still set, clear it and raise the interrupt. */
void gcn_di_tick(CPUState* cpu, u32 core_cycles) {
    GcnDi* di = s_di;
    if (di)
        di->now_cycles += core_cycles;
    if (!di || !di->cmd_pending)
        return;
    if (di->pending_cycles > core_cycles) {
        di->pending_cycles -= core_cycles;
        return;
    }
    di->pending_cycles = 0;
    di->cmd_pending = 0;

    if (di->pending_intr == GCN_DI_INT_TCINT)
        di_commit_pending_read(di, cpu);

    if (di->pending_intr == GCN_DI_INT_TCINT) {
        di->dimar += di->dilength;
        di->dilength = 0;
    }
    if (di->dicr & GCN_DI_CR_TSTART) {
        di->dicr &= ~GCN_DI_CR_TSTART;
        di_generate_interrupt(di, di->pending_intr);
    }
}

/* DVDInterface.cpp ResetDrive:306-346. Only the fields this model tracks are
 * transcribed (drive_state/error_code/enable_dtk); the stream/audio-position/
 * read-buffer fields Dolphin also clears here are not modeled (DTK data path
 * deferred, di.h). Shared by gcn_di_init (via spinup=0, mirroring Init's own
 * ResetDrive(false) call with no disc yet), gcn_di_set_disc/eject_disc (always
 * spinup=0, mirroring SetDisc:430), and the PI_RESET_CODE hook (spinup=1, see
 * di.h "drive re-spin trigger" and gcn_di_reset_drive_spinup below). */
void gcn_di_reset_drive(GcnDi* di, int spinup) {
    if (!di_disc_inside(di))
        di->drive_state = GCN_DI_STATE_COVER_OPENED;         /* :319-327 */
    else if (!spinup)
        di->drive_state = GCN_DI_STATE_DISC_CHANGE;           /* :328-333 */
    else
        di->drive_state = GCN_DI_STATE_DISC_ID_NOT_READ;      /* :334-337 */
    di->error_code = GCN_DI_ERR_NONE;                          /* :339 */
    di->enable_dtk = 0;                                        /* :316 */
    di->cmd_pending = 0;
    di->pending_intr = GCN_DI_INT_TCINT;
    di->pending_cycles = 0;
    di->pending_read_active = 0;
    di->read_buffer_start_offset = 0;
    di->read_buffer_end_offset = 0;
    di->read_buffer_start_time = 0;
    di->read_buffer_end_time = 0;
}

/* No-arg trampoline for pi.c's reset_drive_hook (see di.h). */
void gcn_di_reset_drive_spinup(void) {
    if (s_di)
        gcn_di_reset_drive(s_di, 1);
}

void gcn_di_init(GcnDi* di) {
    memset(di, 0, sizeof *di);
    s_di_command_seq = 0;
    /* DVDInterface::Init:261-302 — ASSERT(!IsDiscInside()) (:263): the console
     * never has a disc mounted yet at this point, ALWAYS. m_DICVR.Hex=1 is set
     * unconditionally (:268) before ResetDrive(false) even runs (:279), i.e.
     * before disc_present could matter here anyway. s_di is set up front so
     * gcn_di_reset_drive (called immediately below) can read disc_present via
     * di_disc_inside. */
    di->dicvr = GCN_DI_CVR_CVR;
    di->irq_level = 0;
    s_di = di;
    gcn_di_reset_drive(di, 0);   /* Init:279 ResetDrive(false); no disc => CoverOpened */
}

/* ROADMAP M5 — DVDInterface.cpp SetDisc:385-431, restricted to the real-disc
 * branch (no auto-disc-change path list, no Wii partition handling — GameCube
 * retail only, matching every other command in this file). Opens `path`
 * read-only; NEVER reads it into RAM (di.h's pread-style contract) — only its
 * size is queried up front, for the Error #001/BlockOOB bound in di_prepare_read. */
int gcn_di_set_disc(GcnDi* di, const char* path) {
    if (!di || !path || !*path) return 0;

    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "gcn di: cannot open disc image '%s'\n", path);
        return 0;
    }
    if (_fseeki64(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "gcn di: cannot seek disc image '%s'\n", path);
        fclose(f);
        return 0;
    }
    long long sz = _ftelli64(f);
    if (sz <= 0) {
        fprintf(stderr, "gcn di: disc image '%s' is empty/unreadable\n", path);
        fclose(f);
        return 0;
    }
    _fseeki64(f, 0, SEEK_SET);

    if (di->disc_file)   /* SetDisc replaces any previously-mounted image */
        fclose(di->disc_file);
    di->disc_file = f;
    di->disc_size = (u64)sz;
    di->disc_present = 1;

    di_set_lid_open(di);          /* :428 — CVR edge -> CVRINT */
    gcn_di_reset_drive(di, 0);    /* :430 — ResetDrive(false) => DiscChangeDetected */
    gcn_ring_event(GCN_EV_DI_MOUNT, 1u, 0u, 0u);

    fprintf(stdout, "gcn di: disc mounted: %s (%lld bytes)\n", path, sz);
    return 1;
}

void gcn_di_eject_disc(GcnDi* di) {
    if (!di) return;
    if (di->disc_file) { fclose(di->disc_file); di->disc_file = NULL; }
    di->disc_size = 0;
    di->disc_present = 0;

    di_set_lid_open(di);          /* EjectDiscCallback:443-446 -> SetDisc(nullptr) */
    gcn_di_reset_drive(di, 0);    /* => no disc: CoverOpened */
    gcn_ring_event(GCN_EV_DI_MOUNT, 0u, 0u, 0u);
}

void gcn_di_free(GcnDi* di) {
    if (!di) return;
    if (di->disc_file) { fclose(di->disc_file); di->disc_file = NULL; }
    if (s_di_journal) { fclose(s_di_journal); s_di_journal = NULL; }
}

/* ---- MMIO (DVDInterface.cpp RegisterMMIO:547-630) ---- */

static void di_warn_size_once(u32 addr, u8 size, int write) {
    static int warned = 0;
    if (!warned) {
        fprintf(stderr, "gcn di: %d-bit %s at 0x%08X — DI registers are 32-bit; "
                        "handling as 32-bit\n",
                (int)size * 8, write ? "write" : "read", addr);
        warned = 1;
    }
}

u32 gcn_di_read(void* user, CPUState* cpu, u32 addr, u8 size) {
    (void)cpu;
    GcnDi* di = (GcnDi*)user;
    u32 off = addr - GCN_DI_BASE;
    if (size != 4)
        di_warn_size_once(addr, size, 0);

    switch (off) {
    case GCN_DI_SR:      return di->disr;
    case GCN_DI_CVR:     return di->dicvr;
    case GCN_DI_CMDBUF0: return di->cmdbuf[0];
    case GCN_DI_CMDBUF1: return di->cmdbuf[1];
    case GCN_DI_CMDBUF2: return di->cmdbuf[2];
    case GCN_DI_MAR:     return di->dimar;
    case GCN_DI_LENGTH:  return di->dilength;
    case GCN_DI_CR:      return di->dicr;
    case GCN_DI_IMMBUF:  return di->diimmbuf;
    case GCN_DI_CFG:     return GCN_DI_CFG_VALUE;   /* read-only */
    default:             return 0;
    }
}

void gcn_di_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size) {
    GcnDi* di = (GcnDi*)user;
    u32 off = addr - GCN_DI_BASE;
    if (size != 4)
        di_warn_size_once(addr, size, 1);

    switch (off) {
    case GCN_DI_SR:
        /* Copy the mask/break enables; W1C the three status bits; re-eval. */
        di->disr = (di->disr & ~(GCN_DI_SR_DEINTMASK | GCN_DI_SR_TCINTMASK |
                                 GCN_DI_SR_BRKINTMASK | GCN_DI_SR_BREAK)) |
                   (value & (GCN_DI_SR_DEINTMASK | GCN_DI_SR_TCINTMASK |
                             GCN_DI_SR_BRKINTMASK | GCN_DI_SR_BREAK));
        if (value & GCN_DI_SR_DEINT)  di->disr &= ~GCN_DI_SR_DEINT;
        if (value & GCN_DI_SR_TCINT)  di->disr &= ~GCN_DI_SR_TCINT;
        if (value & GCN_DI_SR_BRKINT) di->disr &= ~GCN_DI_SR_BRKINT;
        /* BREAK/abort is not modeled (IPL boot never sets it); Dolphin
         * DEBUG_ASSERTs. Any future BREAK write diverges loudly here. */
        di_update_interrupts(di);
        return;

    case GCN_DI_CVR:
        di->dicvr = (di->dicvr & ~GCN_DI_CVR_CVRINTMASK) |
                    (value & GCN_DI_CVR_CVRINTMASK);
        if (value & GCN_DI_CVR_CVRINT) di->dicvr &= ~GCN_DI_CVR_CVRINT;   /* W1C */
        di_update_interrupts(di);
        return;

    case GCN_DI_CMDBUF0: di->cmdbuf[0] = value; return;
    case GCN_DI_CMDBUF1: di->cmdbuf[1] = value; return;
    case GCN_DI_CMDBUF2: di->cmdbuf[2] = value; return;
    case GCN_DI_MAR:     di->dimar = value & GCN_DI_MAR_MASK; return;
    case GCN_DI_LENGTH:  di->dilength = value & GCN_DI_LENGTH_MASK; return;

    case GCN_DI_CR:
        di->dicr = value & GCN_DI_CR_MASK;
        if (di->dicr & GCN_DI_CR_TSTART) {
            /* ExecuteCommand runs immediately; completion is deferred to the
             * next gcn_di_tick (see di.h COMPLETION MODEL). */
            u8 opcode = (u8)(di->cmdbuf[0] >> 24);
            u8 subcmd = (u8)(di->cmdbuf[0] & 0xFFu);
            di->pending_read_active = 0;
            di->pending_cycles = di_us_to_cycles(GCN_DI_MIN_COMMAND_LATENCY_US);
            di->pending_intr = di_execute_command(di, cpu);
            di->cmd_pending = 1;
            di_log_command(di, cpu, opcode, subcmd, di->pending_intr);
            /* Sparse, low-volume (a handful of commands per boot/menu
             * transition) — a plain GcnEventKind addition, same shape as
             * GCN_EV_EXI_XFER, rather than a dedicated ring (contrast the
             * high-volume memcard ring). detail=opcode, aux=subcmd<<8|intr. */
            gcn_ring_event(GCN_EV_DI_CMD, opcode,
                           ((u32)subcmd << 8) | di->pending_intr, cpu->pc);
        }
        return;

    case GCN_DI_IMMBUF: di->diimmbuf = value; return;

    case GCN_DI_CFG:
        /* Read-only (DVDInterface.cpp:628-629 InvalidWrite). Ignore. */
        return;

    default:
        return;
    }
}
