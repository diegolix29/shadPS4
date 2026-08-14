// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>
#include "common/types.h"

namespace Core {

struct alignas(16) GuestRegisterSnapshot {
    u64 rax;
    u64 rbx;
    u64 rcx;
    u64 rdx;
    u64 rsi;
    u64 rdi;
    u64 rbp;
    u64 rsp;
    u64 r8;
    u64 r9;
    u64 r10;
    u64 r11;
    u64 r12;
    u64 r13;
    u64 r14;
    u64 r15;
    u64 rflags;
    u32 mxcsr;
    alignas(16) std::array<std::array<u8, 32>, 16> ymm;
};

using GuestCodeHook = void PS4_SYSV_ABI (*)(u64 tag, const GuestRegisterSnapshot* registers);

// Windows static guest red-zone protection
struct RedZonePatchResult {
    u64 function_count{};
    u64 instruction_count{};
    u64 red_zone_function_count{};
    u64 memory_instruction_count{};
    u64 short_memory_instruction_count{};
    u64 patched_memory_instruction_count{};
    u64 stack_dependent_memory_instruction_count{};
    u64 control_flow_memory_instruction_count{};
    u64 unrelocatable_memory_instruction_count{};
    u64 indirect_red_zone_function_count{};
    u64 cpu_patch_instruction_count{};
    u64 patched_cpu_patch_instruction_count{};
    u64 unsupported_cpu_patch_instruction_count{};
};

/// Registers a module for patching, providing an area to generate trampoline code.
void RegisterPatchModule(void* module_ptr, u64 module_size, void* trampoline_area_ptr,
                         u64 trampoline_area_size);

/// Installs a byte-verified observer while preserving GPR, flags, MXCSR, and YMM state.
/// The copied instruction span must contain whole instructions and must not use relative
/// addressing.
bool InstallGuestCodeHook(void* address, std::span<const u8> expected_instructions, u64 tag,
                          GuestCodeHook hook);

/// Applies CPU patches that need to be done before beginning executions.
void PrePatchInstructions(u64 segment_addr, u64 segment_size);

/// Keeps Windows exception dispatch outside live guest red zones at faultable memory accesses.
RedZonePatchResult PatchRedZoneMemoryInstructions(u64 segment_addr, u64 segment_size,
                                                  std::span<const uintptr_t> function_starts);

} // namespace Core