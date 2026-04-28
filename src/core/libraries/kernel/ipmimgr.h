// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Kernel {

/// PS4 IPMI kernel entry point (NID: Hk7iHmGxB18).
/// All libSceIpmi operations funnel through this single syscall.
/// Currently registered but not actively called — libSceScreenShot's module_start
/// requires proper invocation before IPMI calls flow through here.
int PS4_SYSV_ABI ipmimgr_call(int opcode, int sub_id, void* output_ptr,
                               void* input_data, int input_size, u64 magic);

void RegisterIpmi(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::Kernel
