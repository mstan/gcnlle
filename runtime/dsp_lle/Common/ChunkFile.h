// Shim: no savestates in the runtime — PointerWrap is inert.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
class PointerWrap {
public:
  template <typename T> void Do(T&) {}
  template <typename T> void DoArray(T*, size_t) {}
  template <typename T> void DoArray(T&) {}
  template <typename T> void DoPOD(T&) {}
  bool IsReadMode() const { return false; }
};
