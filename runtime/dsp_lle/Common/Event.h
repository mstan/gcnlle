// Shim: synchronous DSP core never blocks (State::Running), so Event is inert.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace Common {
class Event {
public:
  void Set() {}
  void Wait() {}
  void Reset() {}
};
}
