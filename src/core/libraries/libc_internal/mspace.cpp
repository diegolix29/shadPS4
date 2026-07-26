// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/libraries/libc_internal/mspace.h"

// common/assert.h transitively requires spdlog which is absent from the
// minimal test build (shadps4_libc_mspace_test links only GTest). Use
// std::abort() for the invariant check in InsertFreeBlock; unlike assert(),
// std::abort() fires unconditionally in both debug and release (NDEBUG) builds
// and is self-contained within <cstdlib>.
#include <cstdlib>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Libraries::LibcInternal {

namespace {

constexpr std::size_t DefaultAlignment = 16;
constexpr u32 KnownMspaceFlags = 0x0f;

struct Allocation {
    std::size_t size{};
};

class Arena {
public:
    Arena(std::string name_, std::uintptr_t base_, std::size_t capacity_)
        : name(std::move(name_)), base(base_), capacity(capacity_) {
        if (capacity > 0) {
            free_blocks[0] = capacity;
        }
    }

    void* Allocate(std::size_t size, std::size_t alignment);
    s32 Free(void* pointer);
    // Realloc: null-pointer, zero-size, shrink, grow, and move semantics.
    void* Realloc(void* pointer, std::size_t new_size);
    std::size_t UsableSize(void* pointer) const;

private:
    void InsertFreeBlock(std::size_t offset, std::size_t size);

    // Locked internal helpers — must be called while holding `mutex`.
    // Allocates `size` bytes with `alignment`; returns guest pointer or nullptr.
    void* AllocateLocked(std::size_t size, std::size_t alignment);
    // Frees the block at `addr`; caller must guarantee addr is a live allocation.
    void FreeLocked(std::uintptr_t addr);

    std::string name;
    std::uintptr_t base{};
    std::size_t capacity{};
    mutable std::mutex mutex;
    std::map<std::size_t, std::size_t> free_blocks;
    std::unordered_map<std::uintptr_t, Allocation> allocations;
};

struct HandleEntry {
    std::shared_ptr<Arena> arena;
    // Keeps a destroyed range reserved while a concurrent operation still has
    // a shared owner. The weak reference expires as soon as that operation
    // releases the arena, permitting safe range reuse.
    std::weak_ptr<Arena> retired_arena;
    std::uintptr_t base{};
    std::size_t capacity{};
};

std::mutex registry_mutex;
std::unordered_map<void*, std::unique_ptr<HandleEntry>> handles;

bool RangesOverlap(std::uintptr_t first_base, std::size_t first_capacity,
                   std::uintptr_t second_base, std::size_t second_capacity) {
    const std::uintptr_t first_end = first_base + first_capacity;
    const std::uintptr_t second_end = second_base + second_capacity;
    return first_base < second_end && second_base < first_end;
}

std::optional<std::uintptr_t> AlignUp(std::uintptr_t value, std::size_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return std::nullopt;
    }
    const std::uintptr_t misalign = value & (alignment - 1);
    if (misalign == 0) {
        return value;
    }
    const std::uintptr_t addition = alignment - misalign;
    if (value > std::numeric_limits<std::uintptr_t>::max() - addition) {
        return std::nullopt;
    }
    return value + addition;
}

std::shared_ptr<Arena> ResolveArena(void* handle) {
    if (!handle) {
        return nullptr;
    }
    std::lock_guard lock(registry_mutex);
    auto it = handles.find(handle);
    if (it != handles.end()) {
        return it->second->arena;
    }
    return nullptr;
}

// Precondition: Arena::mutex must be held by the caller.
void Arena::InsertFreeBlock(std::size_t offset, std::size_t size) {
    if (size == 0) {
        return;
    }
    auto [it, inserted] = free_blocks.emplace(offset, size);
    // An exact-offset collision means a block whose start address is already in
    // the free list is being inserted again — an impossible state that indicates
    // a double-free or internal bookkeeping corruption. Abort unconditionally
    // (std::abort is not gated on NDEBUG, unlike assert) so this invariant is
    // enforced in both debug and release builds without any stderr noise in the
    // normal (non-corrupt) code path.
    if (!inserted) [[unlikely]] {
        std::abort();
    }
    auto next = std::next(it);
    if (next != free_blocks.end() && it->first + it->second == next->first) {
        it->second += next->second;
        free_blocks.erase(next);
    }
    if (it != free_blocks.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second == it->first) {
            prev->second += it->second;
            free_blocks.erase(it);
        }
    }
}

void* Arena::Allocate(std::size_t size, std::size_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return nullptr;
    }
    std::lock_guard lock(mutex);
    return AllocateLocked(size, alignment);
}

// Precondition: Arena::mutex must be held by the caller.
void* Arena::AllocateLocked(std::size_t size, std::size_t alignment) {
    const std::size_t alloc_size = (size == 0) ? 1 : size;

    for (auto it = free_blocks.begin(); it != free_blocks.end(); ++it) {
        const std::size_t block_offset = it->first;
        const std::size_t block_size = it->second;
        const std::uintptr_t abs_block_start = base + block_offset;

        auto aligned_opt = AlignUp(abs_block_start, alignment);
        if (!aligned_opt.has_value()) {
            continue;
        }
        const std::uintptr_t aligned_abs = *aligned_opt;
        const std::uintptr_t abs_block_end = abs_block_start + block_size;
        if (aligned_abs >= abs_block_end) {
            continue;
        }

        const std::size_t padding = aligned_abs - abs_block_start;
        if (alloc_size > std::numeric_limits<std::size_t>::max() - padding) {
            continue;
        }
        const std::size_t total_needed = padding + alloc_size;
        if (block_size < total_needed) {
            continue;
        }

        const std::size_t leading_size = padding;
        const std::size_t trailing_size = block_size - total_needed;
        const std::size_t aligned_offset = block_offset + padding;
        const std::size_t trailing_offset = aligned_offset + alloc_size;

        free_blocks.erase(it);

        if (leading_size > 0) {
            InsertFreeBlock(block_offset, leading_size);
        }
        if (trailing_size > 0) {
            InsertFreeBlock(trailing_offset, trailing_size);
        }

        // Store the carved (normalized) allocation size so Free can return the
        // exact interval without re-deriving it from the user-requested size.
        allocations[aligned_abs] = Allocation{ .size = alloc_size };
        return reinterpret_cast<void*>(aligned_abs);
    }

    return nullptr;
}

s32 Arena::Free(void* pointer) {
    if (!pointer) {
        return 0; // null is a no-op success
    }
    const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(pointer);
    std::lock_guard lock(mutex);
    auto it = allocations.find(addr);
    if (it == allocations.end()) {
        return 1; // not a live allocation (unknown or double-free)
    }
    FreeLocked(addr);
    return 0;
}

// Precondition: Arena::mutex must be held and addr must name a live allocation.
void Arena::FreeLocked(std::uintptr_t addr) {
    auto it = allocations.find(addr);
    const std::size_t alloc_size = it->second.size;
    allocations.erase(it);
    const std::size_t offset = addr - base;
    InsertFreeBlock(offset, alloc_size);
}

void* Arena::Realloc(void* pointer, std::size_t new_size) {
    if (!pointer) {
        return Allocate(new_size, DefaultAlignment);
    }
    if (new_size == 0) {
        Free(pointer);
        return nullptr;
    }

    const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(pointer);
    std::lock_guard lock(mutex);
    auto allocation = allocations.find(addr);
    if (allocation == allocations.end()) {
        return nullptr;
    }

    const std::size_t old_size = allocation->second.size;
    if (new_size == old_size) {
        return pointer;
    }

    const std::size_t offset = addr - base;
    if (new_size < old_size) {
        allocation->second.size = new_size;
        InsertFreeBlock(offset + new_size, old_size - new_size);
        return pointer;
    }

    const std::size_t extra_size = new_size - old_size;
    const std::size_t tail_offset = offset + old_size;
    auto tail = free_blocks.find(tail_offset);
    if (tail != free_blocks.end() && tail->second >= extra_size) {
        const std::size_t tail_size = tail->second;
        free_blocks.erase(tail);
        if (tail_size > extra_size) {
            InsertFreeBlock(tail_offset + extra_size, tail_size - extra_size);
        }
        allocation->second.size = new_size;
        return pointer;
    }

    void* replacement = AllocateLocked(new_size, DefaultAlignment);
    if (!replacement) {
        return nullptr;
    }
    std::memcpy(replacement, pointer, old_size);
    FreeLocked(addr);
    return replacement;
}

std::size_t Arena::UsableSize(void* pointer) const {
    if (!pointer) {
        return 0;
    }

    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(pointer);
    std::lock_guard lock(mutex);
    auto allocation = allocations.find(address);
    return allocation != allocations.end() ? allocation->second.size : 0;
}

} // namespace

void* MspaceCreate(const char* name, void* base, std::size_t capacity, u32 flags) {
    if (!base || capacity == 0) {
        return nullptr;
    }
    const std::uintptr_t base_addr = reinterpret_cast<std::uintptr_t>(base);
    if (base_addr > std::numeric_limits<std::uintptr_t>::max() - capacity) {
        return nullptr;
    }
    if ((flags & ~KnownMspaceFlags) != 0) {
        return nullptr;
    }

    std::string name_str;
    if (name) {
        for (std::size_t i = 0; i < 63 && name[i] != '\0'; ++i) {
            name_str.push_back(name[i]);
        }
    }

    std::lock_guard lock(registry_mutex);
    for (const auto& [handle, entry] : handles) {
        if ((entry->arena || !entry->retired_arena.expired()) &&
            RangesOverlap(base_addr, capacity, entry->base, entry->capacity)) {
            return nullptr;
        }
    }

    auto entry = std::make_unique<HandleEntry>();
    entry->arena = std::make_shared<Arena>(std::move(name_str), base_addr, capacity);
    entry->base = base_addr;
    entry->capacity = capacity;
    void* handle = entry.get();
    handles[handle] = std::move(entry);
    return handle;
}

s32 MspaceDestroy(void* handle) {
    if (!handle) {
        return 1;
    }
    std::lock_guard lock(registry_mutex);
    auto it = handles.find(handle);
    if (it == handles.end()) {
        return 1; // unknown token
    }
    if (!it->second->arena) {
        return 1; // already tombstoned
    }
    // Tombstone without erasing the token. A weak reference reserves the range
    // until any operation that resolved this arena before destruction releases
    // its shared owner.
    auto arena = std::move(it->second->arena);
    it->second->retired_arena = arena;
    return 0;
}

void* MspaceMalloc(void* handle, std::size_t size) {
    auto arena = ResolveArena(handle);
    if (!arena) {
        return nullptr;
    }
    return arena->Allocate(size, DefaultAlignment);
}

s32 MspaceFree(void* handle, void* pointer) {
    auto arena = ResolveArena(handle);
    if (!arena) {
        return 1; // invalid or tombstoned handle
    }
    return arena->Free(pointer);
}

void* MspaceCalloc(void* handle, std::size_t count, std::size_t size) {
    auto arena = ResolveArena(handle);
    if (!arena) {
        return nullptr;
    }
    // Overflow-safe multiplication: if count != 0 and size exceeds max/count,
    // the product would wrap; reject it as required by the task brief.
    if (count != 0 && size > std::numeric_limits<std::size_t>::max() / count) {
        return nullptr;
    }
    const std::size_t total = count * size;
    // Allocate; the arena normalises a zero total to 1 byte internally.
    void* ptr = arena->Allocate(total, DefaultAlignment);
    if (!ptr) {
        return nullptr;
    }
    // Zero exactly the logical byte count requested by the caller.
    // When total == 0 the caller receives a 1-byte allocation (arena
    // minimum) but we zero zero bytes, which is safe and correct.
    std::memset(ptr, 0, total);
    return ptr;
}

void* MspaceRealloc(void* handle, void* pointer, std::size_t size) {
    auto arena = ResolveArena(handle);
    if (!arena) {
        return nullptr;
    }
    return arena->Realloc(pointer, size);
}

void* MspaceMemalign(void* handle, std::size_t alignment, std::size_t size) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return nullptr;
    }
    alignment = std::max(alignment, DefaultAlignment);

    auto arena = ResolveArena(handle);
    if (!arena) {
        return nullptr;
    }
    return arena->Allocate(size, alignment);
}

std::size_t MspaceMallocUsableSize(void* pointer) {
    if (!pointer) {
        return 0;
    }

    std::vector<std::shared_ptr<Arena>> arenas;
    {
        std::lock_guard lock(registry_mutex);
        arenas.reserve(handles.size());
        for (const auto& [handle, entry] : handles) {
            if (entry->arena) {
                arenas.push_back(entry->arena);
            }
        }
    }

    for (const auto& arena : arenas) {
        const std::size_t size = arena->UsableSize(pointer);
        if (size != 0) {
            return size;
        }
    }
    return 0;
}

} // namespace Libraries::LibcInternal
