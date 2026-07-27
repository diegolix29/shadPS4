// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Vulkan {

/// Records a varied command stream through a scheduler with a recording thread and through one
/// without, then checks both GPU results against a CPU model and against each other, and checks
/// with the record audit that only the recording thread called into the command buffers.
/// Needs a Vulkan device but no window. Returns true when everything matches.
bool RunRecordSelfTest(u32 rounds);

} // namespace Vulkan
