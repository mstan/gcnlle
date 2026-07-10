// Shim: plain-malloc page allocation; write-protect is a no-op (interpreter only).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdlib>
namespace Common {
inline void* AllocateMemoryPages(size_t size) { return std::calloc(1, size); }
inline void  FreeMemoryPages(void* ptr, size_t /*size*/) { std::free(ptr); }
inline void  WriteProtectMemory(void* /*ptr*/, size_t /*size*/, bool /*exec*/ = true) {}
inline void  UnWriteProtectMemory(void* /*ptr*/, size_t /*size*/, bool /*exec*/ = true) {}
}
