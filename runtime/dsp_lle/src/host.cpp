// SPDX-License-Identifier: GPL-3.0-or-later
//
// DSP::Host implementation (net-new) — binds the vendored DSP-LLE core to the
// runtime's MEM1 and interrupt line. Mirrors oracle/dolphin/.../HW/DSPLLE/
// DSPHost.cpp, minus the debug symbol/dump/CRC paths (interpreter execution
// only needs the DMA + re-analyze on ucode load).
#include "Core/DSP/DSPHost.h"

#include "Core/DSP/DSPAnalyzer.h"
#include "Core/DSP/DSPCore.h"
#include "Common/Hash.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Defined in dsp_lle_c.cpp.
extern uint8_t* g_dsp_mem1;
extern uint32_t g_dsp_mem1_size;
extern uint8_t* g_dsp_aram;
extern volatile int g_dsp_int_pending;

#define GCN_DSP_HOST_ARAM_MASK 0x00FFFFFFu   /* 16 MB ARAM */

namespace DSP::Host
{
// DMA is against guest main memory (cached 0x80.. / uncached 0xC..); mask to the
// physical MEM1 offset. Idempotent for already-physical addresses.
static inline u32 phys(u32 addr) { return addr & 0x1FFFFFFFu; }

// Read/WriteHostMemory are the DSP ACCELERATOR's view — the ARAM, not main
// memory (Dolphin DSPHost.cpp: ReadARAM/WriteARAM).
u8 ReadHostMemory(u32 addr)
{
  return g_dsp_aram ? g_dsp_aram[addr & GCN_DSP_HOST_ARAM_MASK] : 0;
}

void WriteHostMemory(u8 value, u32 addr)
{
  if (g_dsp_aram)
    g_dsp_aram[addr & GCN_DSP_HOST_ARAM_MASK] = value;
}

// MEM1 is big-endian; the DSP wants host-endian u16. size is in bytes.
void DMAToDSP(u16* dst, u32 addr, u32 size)
{
  u32 base = phys(addr);
  for (u32 i = 0; i + 1 < size; i += 2)
  {
    dst[i / 2] = (base + i + 1 < g_dsp_mem1_size)
                     ? static_cast<u16>((u16(g_dsp_mem1[base + i]) << 8) | g_dsp_mem1[base + i + 1])
                     : 0;
  }
}

void DMAFromDSP(const u16* src, u32 addr, u32 size)
{
  u32 base = phys(addr);
  for (u32 i = 0; i + 1 < size; i += 2)
  {
    if (base + i + 1 < g_dsp_mem1_size)
    {
      g_dsp_mem1[base + i] = static_cast<u8>(src[i / 2] >> 8);
      g_dsp_mem1[base + i + 1] = static_cast<u8>(src[i / 2] & 0xFF);
    }
  }
}

void OSD_AddMessage(std::string /*str*/, u32 /*ms*/) {}
bool OnThread() { return false; }
bool IsWiiHost() { return false; }

// Release-store: pairs with the acquire exchange in dsp_lle_take_interrupt —
// the GCN_DSP_THREAD worker raises this from its own thread.
void InterruptRequest() { __atomic_store_n((int*)&g_dsp_int_pending, 1, __ATOMIC_RELEASE); }

void CodeLoaded(DSPCore& dsp, u32 addr, size_t size)
{
  u32 p = phys(addr);
  const u8* ptr = (p < g_dsp_mem1_size) ? (g_dsp_mem1 + p) : nullptr;
  CodeLoaded(dsp, ptr, size);
}

void CodeLoaded(DSPCore& dsp, const u8* ptr, size_t size)
{
  auto& state = dsp.DSPState();
  if (getenv("GCN_DSP_TRACE") && ptr) {
    std::fprintf(stderr, "[dsp] ucode loaded (%zu B) first16:", size);
    for (int i = 0; i < 16; i++) std::fprintf(stderr, " %02x", ptr[i]);
    std::fprintf(stderr, "\n"); std::fflush(stderr);
  }
  if (ptr)
    state.SetIRAMCRC(Common::HashAdler32(ptr, size));
  dsp.ClearIRAM();               // no-op under the interpreter
  state.GetAnalyzer().Analyze(state);  // re-scan the freshly loaded ucode
}
}  // namespace DSP::Host
