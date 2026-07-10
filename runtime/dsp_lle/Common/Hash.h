// Shim: Adler-32 (matches Common::HashAdler32 for DSP ROM verification).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Common/CommonTypes.h"
#include <cstddef>
namespace Common {
inline u32 HashAdler32(const u8* data, size_t len) {
  u32 a = 1, b = 0;
  for (size_t i = 0; i < len; i++) { a = (a + data[i]) % 65521; b = (b + a) % 65521; }
  return (b << 16) | a;
}
}
