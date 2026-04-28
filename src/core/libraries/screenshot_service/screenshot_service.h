// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Libraries::ScreenshotService {

// Apply the GR2 eboot binary patches that make the in-game photo gallery's
// VIEW (Cross) and MARK (Square) actions work on HLE-provided photos.
//
// Thirteen jcc-rel32 instructions in the Mark/View wrapper + dispatcher are
// NOP'd (or one je-rel8 is converted to jmp-rel8). These gates originally
// short-circuit to the sys_ng ("unavailable") sound whenever a cell's
// +0x08 / sub_obj2 / sub_obj1 dereference chain points at null — the exact
// state of the HLE-populated cells. With the gates removed, the Mark and
// View paths complete instead of bouncing to sys_ng.
//
// Triangle (Delete) is NOT patched — those gates are intentionally left in
// place; delete support is a separate problem.
//
// Safe to call multiple times; verifies expected opcode bytes before writing
// and no-ops if already patched or unrecognized.
void ApplyViewMarkPatches(uintptr_t eboot_base);

} // namespace Libraries::ScreenshotService
