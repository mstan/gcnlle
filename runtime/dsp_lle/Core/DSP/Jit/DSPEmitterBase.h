// Shim: interpreter-only build — the JIT is never instantiated. Minimal type so
// DSPCore's guarded m_dsp_jit branches compile. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include "Common/CommonTypes.h"
class PointerWrap;
namespace DSP {
class DSPCore;
namespace JIT {
class DSPEmitter {
public:
  virtual ~DSPEmitter() = default;
  u16 RunCycles(u16 cycles) { return cycles; }
  void ClearIRAM() {}
  void DoState(PointerWrap&) {}
};
inline std::unique_ptr<DSPEmitter> CreateDSPEmitter(DSPCore&) { return nullptr; }
}
}
