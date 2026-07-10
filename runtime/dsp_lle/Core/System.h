// Shim: minimal System providing the two accessors the DSP interpreter uses
// (fake time base + core-timing exception check). SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Core/HW/SystemTimers.h"
#include "Core/CoreTiming.h"
namespace Core {
class System {
public:
  static System& GetInstance() { static System s; return s; }
  SystemTimersManager& GetSystemTimers() { return m_timers; }
  CoreTiming::CoreTimingManager& GetCoreTiming() { return m_coretiming; }
private:
  SystemTimersManager m_timers;
  CoreTiming::CoreTimingManager m_coretiming;
};
}
