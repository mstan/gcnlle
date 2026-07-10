// Shim: DSP ROM hash matches the Official Nintendo ROM, so this is never invoked.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
template <typename... A> inline bool AskYesNoFmtT(const char*, A&&...) { return false; }
