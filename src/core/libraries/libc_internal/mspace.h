// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Libraries::LibcInternal {

// Returns null for an invalid range or one overlapping a live (or in-flight
// destroyed) arena range.
void* MspaceCreate(const char* name, void* base, std::size_t capacity, u32 flags);
s32 MspaceDestroy(void* handle);
void* MspaceMalloc(void* handle, std::size_t size);
s32 MspaceFree(void* handle, void* pointer);
void* MspaceCalloc(void* handle, std::size_t count, std::size_t size);
void* MspaceRealloc(void* handle, void* pointer, std::size_t size);
void* MspaceMemalign(void* handle, std::size_t alignment, std::size_t size);
// Returns the carved extent of the current live allocation at `pointer` (zero
// byte requests occupy one byte). Pointer values have no historical provenance:
// after legitimate numeric-address reuse, the current live allocation wins.
// The pointer is never dereferenced.
std::size_t MspaceMallocUsableSize(void* pointer);

} // namespace Libraries::LibcInternal
