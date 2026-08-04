// SPDX-License-Identifier: GPL-3.0-or-later
//
// C API implementation + the shared host-binding globals. Mirrors Dolphin's
// DSPLLE HW driver (oracle/dolphin/.../HW/DSPLLE/DSPLLE.cpp) against the vendored
// interpreter core, driving the real DSP firmware.
#include "dsp_lle_c.h"

#include "Core/DSP/DSPCore.h"
#include "Core/DSP/DSPAccelerator.h"  /* full Accelerator definition — dsp_lle_save_state */
#include "Core/DSP/DSPHost.h"         /* DSP::Host::CodeLoaded — dsp_lle_load_state's post-load re-analyze */
#include "Core/DSP/DSPTables.h"
#include "Core/DSP/Interpreter/DSPInterpreter.h"
#include "Core/DSP/Interpreter/DSPIntTables.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>  // memset (dsp_lle_save_state)
#include <chrono>   // GCN_DSP_STATS wall-clock accounting only

// Wired by dsp_lle_init; read by src/host.cpp (the DSP::Host callbacks).
uint8_t* g_dsp_mem1 = nullptr;
uint32_t g_dsp_mem1_size = 0;
volatile int g_dsp_int_pending = 0;

// The 16 MB auxiliary RAM (ARAM) — owned here, shared with the AR DMA engine in
// dsp.c. The DSP accelerator (host.cpp Read/WriteHostMemory) reads/writes it.
#define GCN_DSP_LLE_ARAM_SIZE 0x01000000u
uint8_t* g_dsp_aram = nullptr;

using namespace DSP;

static DSPCore* g_core = nullptr;

extern "C" {

void dsp_lle_init(const uint8_t* irom_be, const uint8_t* coef_be,
                  uint8_t* mem1, uint32_t mem1_size) {
  g_dsp_mem1 = mem1;
  g_dsp_mem1_size = mem1_size;
  g_dsp_int_pending = 0;
  if (!g_dsp_aram)
    g_dsp_aram = (uint8_t*)calloc(1, GCN_DSP_LLE_ARAM_SIZE);

  g_core = new DSPCore();
  DSPInitOptions opts;
  opts.core_type = DSPInitOptions::CoreType::Interpreter;
  // ROM files are big-endian 16-bit words; the core wants host-endian u16.
  for (uint32_t i = 0; i < DSP_IROM_SIZE; i++)
    opts.irom_contents[i] = (uint16_t(irom_be[2 * i]) << 8) | irom_be[2 * i + 1];
  for (uint32_t i = 0; i < DSP_COEF_SIZE; i++)
    opts.coef_contents[i] = (uint16_t(coef_be[2 * i]) << 8) | coef_be[2 * i + 1];

  g_core->Initialize(opts);
  g_core->Reset();
  // Build the DSPOPCTemplate opcode dispatch table (s_op_table in DSPTables.cpp).
  // Dolphin's HW/DSPLLE/DSPLLE.cpp does this right after Reset(); our C-API port
  // replaced that driver, so the call has to live here. Without it GetOpTemplate()
  // returns null and the first executed DSP instruction null-derefs in
  // ExecuteInstruction(). (The interpreter's own function-pointer tables are a
  // separate array initialized by the Interpreter ctor.)
  InitInstructionTable();
  // Fuse the template's extension bit with the interpreter's main/extension
  // handlers. ExecuteInstruction then needs one opcode-indexed lookup instead
  // of three independent tables on every real DSP instruction.
  Interpreter::FinalizeInstructionTables();
  g_core->SetState(State::Running);
}

void dsp_lle_shutdown(void) {
  if (g_core) {
    g_core->Shutdown();
    delete g_core;
    g_core = nullptr;
  }
  free(g_dsp_aram);
  g_dsp_aram = nullptr;
}

uint16_t dsp_lle_read_control(void) {
  return g_core->GetInterpreter().ReadControlRegister();
}

uint16_t dsp_lle_write_control(uint16_t value) {
  g_core->GetInterpreter().WriteControlRegister(value);
  if ((value & CR_EXTERNAL_INT) != 0) {
    g_core->CheckExternalInterrupt();
    g_core->CheckExceptions();
  }
  return g_core->GetInterpreter().ReadControlRegister();
}

uint16_t dsp_lle_read_mbox_hi(int cpu_mailbox) {
  return g_core->ReadMailboxHigh(cpu_mailbox ? Mailbox::CPU : Mailbox::DSP);
}
uint16_t dsp_lle_read_mbox_lo(int cpu_mailbox) {
  return g_core->ReadMailboxLow(cpu_mailbox ? Mailbox::CPU : Mailbox::DSP);
}
void dsp_lle_write_mbox_hi(uint16_t value) {
  g_core->WriteMailboxHigh(Mailbox::CPU, value);
}
void dsp_lle_write_mbox_lo(uint16_t value) {
  g_core->WriteMailboxLow(Mailbox::CPU, value);
}
uint32_t dsp_lle_peek_mbox_cpu(void) {
  return g_core->PeekMailbox(Mailbox::CPU);
}

uint16_t dsp_lle_pc(void) {
  return g_core ? g_core->DSPState().pc : 0;
}

uint32_t dsp_lle_peek_mbox_dsp(void) {
  return g_core ? g_core->PeekMailbox(Mailbox::DSP) : 0;
}

uint8_t* dsp_lle_aram(void) { return g_dsp_aram; }
uint32_t dsp_lle_aram_size(void) { return GCN_DSP_LLE_ARAM_SIZE; }

void dsp_lle_update(int ppc_cycles) {
  // Fast-path: a halted DSP core (CR_HALT set) runs nothing. Interpreter::RunCycles'
  // very first action is `if ((control_reg & CR_HALT) != 0) return 0;`
  // (Interpreter/DSPInterpreter.cpp:178) — every entry while halted is a guaranteed
  // no-op with zero side effects (no Step, no CheckExceptions, no state change). The
  // GameCube IPL leaves the DSP halted ~99.9% of boot, so descending into
  // DSPCore::RunCycles -> Interpreter::RunCycles each block just to hit that early
  // return is wasted work. Skip it here, reading the SAME raw control_reg the
  // interpreter checks (NOT dsp_lle_read_control, which carries CR_INIT_CODE
  // timebase side effects). CR_HALT is cleared ONLY by a CPU control-register write
  // (Interpreter::WriteControlRegister:265, the sole writer of control_reg), which
  // runs on the MMIO path via dsp_lle_write_control — never from this per-block
  // tick — so no wake is ever missed. Exact, not a heuristic.
  // GCN_DSP_HALT_SKIP=0 disables it (for A/B measurement); default on.
  static int s_halt_skip = -1;   // cached; getenv-per-block is a hot-path cost
  if (s_halt_skip < 0) {
    const char* e = getenv("GCN_DSP_HALT_SKIP");
    s_halt_skip = (e && e[0] == '0') ? 0 : 1;
  }
  if (s_halt_skip && (g_core->DSPState().control_reg & CR_HALT) != 0)
    return;

  const int dsp_cycles = ppc_cycles / 6;
  if (dsp_cycles > 0) {
    static int s_trace = -1;   // read once; getenv per block was a hot-path cost
    if (s_trace < 0) s_trace = getenv("GCN_DSP_TRACE") ? 1 : 0;
    if (s_trace && (g_core->DSPState().control_reg & CR_HALT) == 0) {
      fprintf(stderr, "[dsp] pc=%04x cr=%04x\n",
              g_core->DSPState().pc, g_core->DSPState().control_reg);
      fflush(stderr);
    }
    // GCN_DSP_STATS=1: per-call accounting of the RunCycles budget vs the pc it
    // stopped on and whether the analyzer would idle-skip there. Answers "is
    // the batched budget being burned stepping an unrecognized wait loop, or
    // is idle-skip actually terminating calls early?" without a debugger.
    // Env-gated observability only; off by default (zero hot-path cost beyond
    // one cached getenv).
    static int s_stats = -1;
    if (s_stats < 0) s_stats = getenv("GCN_DSP_STATS") ? 1 : 0;
    if (s_stats) {
      static uint64_t calls = 0, budget = 0, stop_idle = 0, stop_other = 0;
      static uint64_t wall_ns = 0;
      auto& st = g_core->DSPState();
      calls++;
      budget += (uint64_t)dsp_cycles;
      auto t0 = std::chrono::steady_clock::now();
      g_core->RunCycles(dsp_cycles);
      auto t1 = std::chrono::steady_clock::now();
      wall_ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
      if (st.GetAnalyzer().IsIdleSkip(st.pc))
        stop_idle++;
      else
        stop_other++;
      if ((calls & 0x3FFu) == 0u) {
        uint64_t steps = st.GetStepCounter();
        fprintf(stderr,
                "[dsp-stats] calls=%llu budget=%llu (avg %.1f cyc/call) "
                "steps=%llu wall_ms=%.1f ns/step=%.1f ns/call=%.0f "
                "stop@idle=%llu stop@other=%llu last_pc=%04x cr=%04x "
                "imem=%04x,%04x,%04x,%04x,%04x,%04x,%04x\n",
                (unsigned long long)calls, (unsigned long long)budget,
                (double)budget / (double)calls,
                (unsigned long long)steps,
                (double)wall_ns / 1e6,
                steps ? (double)wall_ns / (double)steps : 0.0,
                (double)wall_ns / (double)calls,
                (unsigned long long)stop_idle, (unsigned long long)stop_other,
                st.pc, st.control_reg,
                st.ReadIMEM(st.pc), st.ReadIMEM(static_cast<uint16_t>(st.pc + 1u)),
                st.ReadIMEM(static_cast<uint16_t>(st.pc + 2u)),
                st.ReadIMEM(static_cast<uint16_t>(st.pc + 3u)),
                st.ReadIMEM(static_cast<uint16_t>(st.pc + 4u)),
                st.ReadIMEM(static_cast<uint16_t>(st.pc + 5u)),
                st.ReadIMEM(static_cast<uint16_t>(st.pc + 6u)));
        fflush(stderr);
      }
      return;
    }
    g_core->RunCycles(dsp_cycles);
  }
}

int dsp_lle_halted(void) {
  return g_core && (g_core->DSPState().control_reg & CR_HALT) != 0;
}

// SNAPSHOT_RESUME pass A: static_assert the hand-rolled GcnDspLleSnapshot
// array sizes (dsp_lle_c.h) against the real compile-time constants, so a
// future upstream resync that changes DSP_IRAM_SIZE/DSP_DRAM_SIZE/
// DSP_STACK_DEPTH fails the build here instead of silently truncating a save.
static_assert(DSP_IRAM_SIZE == 4096, "GcnDspLleSnapshot::iram size mismatch");
static_assert(DSP_DRAM_SIZE == 4096, "GcnDspLleSnapshot::dram size mismatch");
static_assert(DSP_STACK_DEPTH == 32, "GcnDspLleSnapshot::reg_stacks size mismatch");

int dsp_lle_save_state(GcnDspLleSnapshot* out) {
  if (!g_core || !out) return 0;
  memset(out, 0, sizeof(*out));

  SDSP& dsp = g_core->DSPState();
  for (int i = 0; i < 4; i++) {
    out->r_ar[i] = dsp.r.ar[i];
    out->r_ix[i] = dsp.r.ix[i];
    out->r_wr[i] = dsp.r.wr[i];
    out->r_st[i] = dsp.r.st[i];
  }
  out->r_cr = dsp.r.cr;
  out->r_sr = dsp.r.sr;
  out->r_prod = dsp.r.prod.val;
  out->r_ax[0] = dsp.r.ax[0].val;
  out->r_ax[1] = dsp.r.ax[1].val;
  out->r_ac[0] = dsp.r.ac[0].val;
  out->r_ac[1] = dsp.r.ac[1].val;

  out->pc = dsp.pc;
  out->control_reg = dsp.control_reg;
  out->control_reg_init_code_clear_time = dsp.control_reg_init_code_clear_time;
  for (int i = 0; i < 4; i++) out->reg_stack_ptrs[i] = dsp.reg_stack_ptrs[i];
  out->exceptions = dsp.exceptions;
  out->external_interrupt_waiting = dsp.external_interrupt_waiting.load() ? 1 : 0;
  out->reset_dspjit_codespace = dsp.reset_dspjit_codespace ? 1 : 0;
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < DSP_STACK_DEPTH; j++)
      out->reg_stacks[i][j] = dsp.reg_stacks[i][j];

  {
    const auto& ifx = dsp.IFXRegs();
    for (size_t i = 0; i < ifx.size() && i < 256; i++) out->ifx_regs[i] = ifx[i];
  }
  out->mailbox[0] = g_core->PeekMailbox(Mailbox::CPU);
  out->mailbox[1] = g_core->PeekMailbox(Mailbox::DSP);

  if (dsp.iram)
    for (uint32_t i = 0; i < DSP_IRAM_SIZE; i++) out->iram[i] = dsp.iram[i];
  if (dsp.dram)
    for (uint32_t i = 0; i < DSP_DRAM_SIZE; i++) out->dram[i] = dsp.dram[i];

  out->core_state = (int32_t)g_core->GetState();

  if (Accelerator* acc = dsp.GetAccelerator()) {
    out->accel_start_address = acc->GetStartAddress();
    out->accel_end_address = acc->GetEndAddress();
    out->accel_current_address = acc->GetCurrentAddress();
    out->accel_sample_format = acc->GetSampleFormat();
    out->accel_gain = acc->GetGain();
    out->accel_yn1 = acc->GetYn1();
    out->accel_yn2 = acc->GetYn2();
    out->accel_pred_scale = acc->GetPredScale();
    out->accel_input = acc->GetInput();
    out->accel_reads_stopped = acc->GetReadsStopped() ? 1 : 0;
  }

  out->dsp_int_pending = g_dsp_int_pending;
  return 1;
}

int dsp_lle_load_state(const GcnDspLleSnapshot* in) {
  if (!g_core || !in) return 0;

  SDSP& dsp = g_core->DSPState();
  for (int i = 0; i < 4; i++) {
    dsp.r.ar[i] = in->r_ar[i];
    dsp.r.ix[i] = in->r_ix[i];
    dsp.r.wr[i] = in->r_wr[i];
    dsp.r.st[i] = in->r_st[i];
  }
  dsp.r.cr = in->r_cr;
  dsp.r.sr = in->r_sr;
  dsp.r.prod.val = in->r_prod;
  dsp.r.ax[0].val = in->r_ax[0];
  dsp.r.ax[1].val = in->r_ax[1];
  dsp.r.ac[0].val = in->r_ac[0];
  dsp.r.ac[1].val = in->r_ac[1];

  dsp.pc = in->pc;
  dsp.control_reg = in->control_reg;
  dsp.control_reg_init_code_clear_time = in->control_reg_init_code_clear_time;
  for (int i = 0; i < 4; i++) dsp.reg_stack_ptrs[i] = in->reg_stack_ptrs[i];
  dsp.exceptions = in->exceptions;
  dsp.external_interrupt_waiting.store(in->external_interrupt_waiting != 0);
  dsp.reset_dspjit_codespace = in->reset_dspjit_codespace != 0;
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < DSP_STACK_DEPTH; j++)
      dsp.reg_stacks[i][j] = in->reg_stacks[i][j];

  {
    auto& ifx = dsp.IFXRegsMutable();
    for (size_t i = 0; i < ifx.size() && i < 256; i++) ifx[i] = in->ifx_regs[i];
  }
  dsp.SetMailboxRaw(Mailbox::CPU, in->mailbox[0]);
  dsp.SetMailboxRaw(Mailbox::DSP, in->mailbox[1]);

  if (dsp.iram)
    for (uint32_t i = 0; i < DSP_IRAM_SIZE; i++) dsp.iram[i] = in->iram[i];
  if (dsp.dram)
    for (uint32_t i = 0; i < DSP_DRAM_SIZE; i++) dsp.dram[i] = in->dram[i];

  g_core->SetState(static_cast<State>(in->core_state));

  if (Accelerator* acc = dsp.GetAccelerator()) {
    acc->SetStartAddress(in->accel_start_address);
    acc->SetEndAddress(in->accel_end_address);
    acc->SetCurrentAddress(in->accel_current_address);
    acc->SetSampleFormat(in->accel_sample_format);
    acc->SetGain(in->accel_gain);
    acc->SetYn1(in->accel_yn1);
    acc->SetYn2(in->accel_yn2);
    acc->SetPredScale(in->accel_pred_scale);
    acc->SetInput(in->accel_input);
    acc->SetReadsStopped(in->accel_reads_stopped != 0);
  }

  g_dsp_int_pending = in->dsp_int_pending;

  // Mirror DSPCore.cpp's own post-load DoState behavior (DSPCore.cpp:415-416,
  // `if (p.IsReadMode()) Host::CodeLoaded(...)`), which never fires in this
  // runtime because PointerWrap::IsReadMode() always returns false (see
  // Common/ChunkFile.h). Re-analyzing the restored IRAM here is what makes
  // the Analyzer's idle-skip/loop-detection tables valid again — without it
  // they'd still reflect whatever ucode (if any) ran before this restore.
  if (dsp.iram)
    DSP::Host::CodeLoaded(*g_core, reinterpret_cast<const u8*>(dsp.iram),
                          DSP_IRAM_BYTE_SIZE);

  return 1;
}

int dsp_lle_take_interrupt(void) {
  // Atomic exchange: with the GCN_DSP_THREAD worker the flag is set by
  // Host::InterruptRequest on the worker thread (host.cpp) and consumed here
  // on the CPU thread; a plain read+clear could lose a set that lands
  // between the two. Equivalent single-threaded.
  return __atomic_exchange_n((int*)&g_dsp_int_pending, 0, __ATOMIC_ACQ_REL);
}

}  // extern "C"
