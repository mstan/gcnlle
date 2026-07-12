// SPDX-License-Identifier: GPL-3.0-or-later
//
// C API implementation + the shared host-binding globals. Mirrors Dolphin's
// DSPLLE HW driver (oracle/dolphin/.../HW/DSPLLE/DSPLLE.cpp) against the vendored
// interpreter core, driving the real DSP firmware.
#include "dsp_lle_c.h"

#include "Core/DSP/DSPCore.h"
#include "Core/DSP/DSPTables.h"
#include "Core/DSP/Interpreter/DSPInterpreter.h"

#include <cstdlib>
#include <cstdio>

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
    g_core->RunCycles(dsp_cycles);
  }
}

int dsp_lle_take_interrupt(void) {
  int p = g_dsp_int_pending;
  g_dsp_int_pending = 0;
  return p;
}

}  // extern "C"
