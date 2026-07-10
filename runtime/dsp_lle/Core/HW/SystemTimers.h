// Shim: fake time-base drives only the DSPInitCode clear window. Monotonic counter.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Common/CommonTypes.h"
namespace Core { class System; }
class SystemTimersManager {
public:
  // The DSP core uses the fake time base only to clear DSPInitCode ~130 ticks
  // after DSPInit is dropped. We don't model the real bus clock; advance it each
  // query so the window closes after a few CSR polls (the poll-aware oracle diff
  // makes the exact count immaterial).
  u64 GetFakeTimeBase() const { m_tb += 50; return m_tb; }
  mutable u64 m_tb = 0;
};
