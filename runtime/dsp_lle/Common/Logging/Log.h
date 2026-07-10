// Shim: no-op logging for the vendored DSP LLE core. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace Common::Log { enum LogType { DSPLLE, DSPHLE, AUDIO, AUDIO_INTERFACE, COMMON }; }
#define GENERIC_LOG_FMT(...) ((void)0)
#define ERROR_LOG_FMT(...)  ((void)0)
#define WARN_LOG_FMT(...)   ((void)0)
#define NOTICE_LOG_FMT(...) ((void)0)
#define INFO_LOG_FMT(...)   ((void)0)
#define DEBUG_LOG_FMT(...)  ((void)0)
