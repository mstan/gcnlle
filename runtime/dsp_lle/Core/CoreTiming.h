// Shim: DSP init-code timer poll is a no-op (we clear on a nominal poll window).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace CoreTiming {
class CoreTimingManager { public: void ForceExceptionCheck(long long) {} };
}
