// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include "common/types.h"

namespace Core {

/// Registers a module for patching, providing an area to generate trampoline code.
void RegisterPatchModule(void* module_ptr, u64 module_size, void* trampoline_area_ptr,
                         u64 trampoline_area_size);

/// Applies CPU patches that need to be done before beginning executions.
void PrePatchInstructions(u64 segment_addr, u64 segment_size);

/// Game-specific workaround for red zone corruption.
///
/// On platforms without a sigaltstack equivalent for hardware exceptions (Windows VEH in
/// particular), a synchronous access-violation used for our GPU buffer/texture dirty tracking
/// can be delivered while a guest leaf function is using its SysV red zone. The OS builds its
/// exception context on the current stack before our handler ever runs, clobbering whatever the
/// guest had stored there. This patches a known-affected function to reserve stack space ahead
/// of the red zone for the duration of the function, so a fault landing anywhere inside it can
/// no longer corrupt live data.
///
/// `entry_addr`/`entry_patch_size` mark the function's first instruction and how many bytes are
/// free to overwrite there (must fit a `lea rsp, [rsp-128]`, 4-8 bytes depending on encoding).
/// `exit_addrs`/`exit_patch_size` mark each return point (there may be more than one) and the
/// free bytes available right before each `ret`.
void PatchRedZoneGuard(void* entry_addr, size_t entry_patch_size, std::span<void* const> exit_addrs,
                       size_t exit_patch_size);

/// Widens an existing `sub rsp, imm8` / `add rsp, imm8` stack-frame adjustment in place by
/// overwriting its one-byte immediate. Useful when a function already opens its own frame (so
/// there's no free space to inject a new instruction the way PatchRedZoneGuard does) but still
/// needs more headroom below it, e.g. because it makes heavy use of the red zone on top of that
/// frame. `expected_old_value` is checked before writing, since a mismatch means the offset
/// doesn't correspond to what was reverse-engineered (different game version/build).
void PatchStackReserveImmediate(void* addr, u8 expected_old_value, u8 new_value);

} // namespace Core