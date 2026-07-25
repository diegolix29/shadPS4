// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Common {

/// Call once at startup (from SignalDispatch ctor). Checks BACHATA_CRASH_REGISTERS.
void InitCrashReporter();

/// Async-signal-safe: dumps registers + fault info to stderr.
/// Must only be called from a signal handler where raw_context is valid.
void ReportCrash(void* raw_context, int signum, void* siginfo);

} // namespace Common
