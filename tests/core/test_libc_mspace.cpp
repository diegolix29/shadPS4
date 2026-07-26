// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "core/libraries/libc_internal/mspace.h"

namespace {

using Libraries::LibcInternal::MspaceCalloc;
using Libraries::LibcInternal::MspaceCreate;
using Libraries::LibcInternal::MspaceDestroy;
using Libraries::LibcInternal::MspaceFree;
using Libraries::LibcInternal::MspaceMalloc;
using Libraries::LibcInternal::MspaceMallocUsableSize;
using Libraries::LibcInternal::MspaceMemalign;
using Libraries::LibcInternal::MspaceRealloc;

TEST(LibcMspace, CreateAndMallocReturnGuestBackedPointer) {
    alignas(64) std::array<std::byte, 4096> storage{};

    void* handle = MspaceCreate("CrashArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);

    void* allocation = MspaceMalloc(handle, 128);
    ASSERT_NE(allocation, nullptr);

    const auto address = reinterpret_cast<std::uintptr_t>(allocation);
    const auto begin = reinterpret_cast<std::uintptr_t>(storage.data());
    EXPECT_GE(address, begin);
    EXPECT_LE(address + 128, begin + storage.size());
    EXPECT_EQ(address % 16, 0u);
    EXPECT_EQ(MspaceDestroy(handle), 0);
}

// ---------------------------------------------------------------------------
// Task 3 – Step 1: free/coalescing test
// ---------------------------------------------------------------------------

TEST(LibcMspace, FreeCoalescesAndReusesGuestStorage) {
    alignas(64) std::array<std::byte, 1024> storage{};

    void* handle = MspaceCreate("CoalesceArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);

    void* a = MspaceMalloc(handle, 128);
    void* b = MspaceMalloc(handle, 128);
    void* c = MspaceMalloc(handle, 128);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    // Verify that a and b are exactly adjacent so coalescing produces a
    // contiguous 256-byte interval (not merely two separated 128-byte gaps).
    const auto addr_a = reinterpret_cast<std::uintptr_t>(a);
    const auto addr_b = reinterpret_cast<std::uintptr_t>(b);
    ASSERT_EQ(addr_b, addr_a + 128u) << "a and b must be adjacent for coalescing to work";

    EXPECT_EQ(MspaceFree(handle, a), 0);
    EXPECT_EQ(MspaceFree(handle, b), 0);

    // 256 bytes fits only if the two freed blocks coalesced.
    void* big = MspaceMalloc(handle, 256);
    ASSERT_NE(big, nullptr);

    const auto addr = reinterpret_cast<std::uintptr_t>(big);
    const auto begin = reinterpret_cast<std::uintptr_t>(storage.data());
    EXPECT_GE(addr, begin);
    EXPECT_LE(addr + 256, begin + storage.size());
    EXPECT_EQ(MspaceDestroy(handle), 0);
}

// ---------------------------------------------------------------------------
// Task 3 – Step 5: invalid-handle, double-free, and destroy tests
// ---------------------------------------------------------------------------

TEST(LibcMspace, FreeNullPointerReturnsSuccess) {
    alignas(64) std::array<std::byte, 512> storage{};
    void* handle = MspaceCreate("NullFreeArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);
    EXPECT_EQ(MspaceFree(handle, nullptr), 0);
    EXPECT_EQ(MspaceDestroy(handle), 0);
}

TEST(LibcMspace, FreeUnknownPointerReturnsFailure) {
    alignas(64) std::array<std::byte, 512> storage{};
    void* handle = MspaceCreate("BadPtrArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);
    int dummy = 42;
    EXPECT_EQ(MspaceFree(handle, &dummy), 1);
    EXPECT_EQ(MspaceDestroy(handle), 0);
}

TEST(LibcMspace, DoubleFreeReturnsFailure) {
    alignas(64) std::array<std::byte, 512> storage{};
    void* handle = MspaceCreate("DfArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);

    void* p = MspaceMalloc(handle, 64);
    ASSERT_NE(p, nullptr);

    EXPECT_EQ(MspaceFree(handle, p), 0);
    EXPECT_EQ(MspaceFree(handle, p), 1);
    EXPECT_EQ(MspaceDestroy(handle), 0);
}

TEST(LibcMspace, FreePointerBelongingToDifferentArenaReturnsFailure) {
    alignas(64) std::array<std::byte, 512> storage_a{};
    alignas(64) std::array<std::byte, 512> storage_b{};

    void* handle_a = MspaceCreate("ArenaA", storage_a.data(), storage_a.size(), 0);
    void* handle_b = MspaceCreate("ArenaB", storage_b.data(), storage_b.size(), 0);
    ASSERT_NE(handle_a, nullptr);
    ASSERT_NE(handle_b, nullptr);

    void* p = MspaceMalloc(handle_a, 64);
    ASSERT_NE(p, nullptr);

    EXPECT_EQ(MspaceFree(handle_b, p), 1);
    EXPECT_EQ(MspaceDestroy(handle_a), 0);
    EXPECT_EQ(MspaceDestroy(handle_b), 0);
}

TEST(LibcMspace, DestroyValidHandleReturnsSuccess) {
    alignas(64) std::array<std::byte, 512> storage{};
    void* handle = MspaceCreate("DestroyArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);
    EXPECT_EQ(MspaceDestroy(handle), 0);
}

TEST(LibcMspace, MallocThroughDestroyedHandleReturnsNull) {
    alignas(64) std::array<std::byte, 512> storage{};
    void* handle = MspaceCreate("DeadArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);
    EXPECT_EQ(MspaceDestroy(handle), 0);
    EXPECT_EQ(MspaceMalloc(handle, 64), nullptr);
}

TEST(LibcMspace, FreeThroughDestroyedHandleReturnsFailure) {
    alignas(64) std::array<std::byte, 512> storage{};
    void* handle = MspaceCreate("DeadFreeArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);

    void* p = MspaceMalloc(handle, 64);
    ASSERT_NE(p, nullptr);

    ASSERT_EQ(MspaceDestroy(handle), 0);
    EXPECT_EQ(MspaceFree(handle, p), 1);
}

TEST(LibcMspace, RepeatedDestroyReturnsFailure) {
    alignas(64) std::array<std::byte, 512> storage{};
    void* handle = MspaceCreate("RepeatDestroyArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);
    EXPECT_EQ(MspaceDestroy(handle), 0);
    EXPECT_EQ(MspaceDestroy(handle), 1);
}

TEST(LibcMspace, DestroyNullHandleReturnsFailure) {
    EXPECT_EQ(MspaceDestroy(nullptr), 1);
}

// ---------------------------------------------------------------------------
// Task 4 – calloc zeroing
// ---------------------------------------------------------------------------

TEST(LibcMspace, CallocZeroesExactlyTheAllocation) {
    alignas(64) std::array<std::byte, 4096> storage{};
    // Poison the backing buffer with 0xFF so we can detect un-zeroed bytes.
    std::fill(storage.begin(), storage.end(), std::byte{0xFF});

    void* handle = MspaceCreate("CallocArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);

    const std::size_t count = 10;
    const std::size_t elem  = 32;
    void* p = MspaceCalloc(handle, count, elem);
    ASSERT_NE(p, nullptr);

    // The returned pointer must lie inside the guest buffer.
    const auto addr  = reinterpret_cast<std::uintptr_t>(p);
    const auto begin = reinterpret_cast<std::uintptr_t>(storage.data());
    EXPECT_GE(addr, begin);
    EXPECT_LE(addr + count * elem, begin + storage.size());

    // Every byte of the logical allocation must be zero.
    const auto* bytes = static_cast<const std::byte*>(p);
    for (std::size_t i = 0; i < count * elem; ++i) {
        EXPECT_EQ(bytes[i], std::byte{0}) << "non-zero at index " << i;
    }
    EXPECT_EQ(MspaceDestroy(handle), 0);
}

// ---------------------------------------------------------------------------
// Task 4 – arena exhaustion
// ---------------------------------------------------------------------------

TEST(LibcMspace, MallocExhaustionReturnsNull) {
    // Small arena: only 256 bytes.
    alignas(64) std::array<std::byte, 256> storage{};
    void* handle = MspaceCreate("ExhaustArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);

    // Drain the arena with repeated small allocations.
    void* last_good = nullptr;
    for (int i = 0; i < 32; ++i) {
        void* p = MspaceMalloc(handle, 16);
        if (!p) break;
        last_good = p;
    }
    // At least one allocation must have succeeded.
    ASSERT_NE(last_good, nullptr);

    // Further allocations must fail, not crash.
    void* overflow_alloc = MspaceMalloc(handle, 16);
    EXPECT_EQ(overflow_alloc, nullptr);
    EXPECT_EQ(MspaceDestroy(handle), 0);
}

// ---------------------------------------------------------------------------
// Task 4 – oversized single request
// ---------------------------------------------------------------------------

TEST(LibcMspace, MallocOversizedRequestReturnsNull) {
    alignas(64) std::array<std::byte, 512> storage{};
    void* handle = MspaceCreate("OversizeArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);

    // Request more than the entire arena capacity.
    void* p = MspaceMalloc(handle, storage.size() + 1);
    EXPECT_EQ(p, nullptr);

    // A live allocation made before the failure must still be intact.
    void* q = MspaceMalloc(handle, 16);
    ASSERT_NE(q, nullptr);
    const auto addr  = reinterpret_cast<std::uintptr_t>(q);
    const auto begin = reinterpret_cast<std::uintptr_t>(storage.data());
    EXPECT_GE(addr, begin);
    EXPECT_LE(addr + 16, begin + storage.size());
    EXPECT_EQ(MspaceDestroy(handle), 0);
}

// ---------------------------------------------------------------------------
// Task 4 – MspaceCreate edge cases
// ---------------------------------------------------------------------------

TEST(LibcMspace, CreateWithNullBaseReturnsNull) {
    EXPECT_EQ(MspaceCreate("NullBase", nullptr, 4096, 0), nullptr);
}

TEST(LibcMspace, CreateWithZeroCapacityReturnsNull) {
    alignas(64) std::array<std::byte, 4096> storage{};
    EXPECT_EQ(MspaceCreate("ZeroCap", storage.data(), 0, 0), nullptr);
}

TEST(LibcMspace, CreateWithRangeOverflowReturnsNull) {
    // base + capacity would wrap around the address space.
    void* base = reinterpret_cast<void*>(std::numeric_limits<std::uintptr_t>::max() - 16);
    EXPECT_EQ(MspaceCreate("Overflow", base, 4096, 0), nullptr);
}

TEST(LibcMspace, CreateWithUnknownFlagsReturnsNull) {
    alignas(64) std::array<std::byte, 4096> storage{};
    EXPECT_EQ(MspaceCreate("BadFlags", storage.data(), storage.size(), 0xFF00), nullptr);
}

// ---------------------------------------------------------------------------
// Task 4 – calloc count*size multiplication overflow
// ---------------------------------------------------------------------------

TEST(LibcMspace, CallocMultiplicationOverflowReturnsNull) {
    alignas(64) std::array<std::byte, 4096> storage{};
    void* handle = MspaceCreate("OverflowCallocArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);

    // count * size overflows std::size_t; calloc must detect this and return null.
    const std::size_t huge_count = std::numeric_limits<std::size_t>::max() / 2 + 1;
    const std::size_t elem_size  = 2;
    void* p = MspaceCalloc(handle, huge_count, elem_size);
    EXPECT_EQ(p, nullptr);

    // A live allocation made before the failed calloc must still work.
    void* q = MspaceMalloc(handle, 16);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(MspaceDestroy(handle), 0);
}

// ---------------------------------------------------------------------------
// Task 5 – Step 1: realloc edge-case tests
// ---------------------------------------------------------------------------

TEST(LibcMspace, ReallocNullPointerActsLikeMalloc) {
    // realloc(handle, nullptr, size) must behave exactly like malloc.
    alignas(64) std::array<std::byte, 4096> storage{};
    void* handle = MspaceCreate("ReallocNullPtrArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);

    void* p = MspaceRealloc(handle, nullptr, 128);
    ASSERT_NE(p, nullptr);

    const auto addr  = reinterpret_cast<std::uintptr_t>(p);
    const auto begin = reinterpret_cast<std::uintptr_t>(storage.data());
    EXPECT_GE(addr, begin);
    EXPECT_LE(addr + 128, begin + storage.size());
    EXPECT_EQ(addr % 16, 0u);

    // The malloc-equivalent rule also applies to a zero-byte request.
    void* zero = MspaceRealloc(handle, nullptr, 0);
    EXPECT_NE(zero, nullptr);
    EXPECT_EQ(MspaceDestroy(handle), 0);
}

TEST(LibcMspace, ReallocZeroSizeFreesAndReturnsNull) {
    // realloc(handle, ptr, 0) must free the block and return null.
    alignas(64) std::array<std::byte, 4096> storage{};
    void* handle = MspaceCreate("ReallocZeroArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);

    void* p = MspaceMalloc(handle, 256);
    ASSERT_NE(p, nullptr);

    void* result = MspaceRealloc(handle, p, 0);
    EXPECT_EQ(result, nullptr);

    // First-fit must reuse the exact interval released by realloc.
    void* q = MspaceMalloc(handle, 256);
    EXPECT_EQ(q, p);
    EXPECT_EQ(MspaceDestroy(handle), 0);
}

TEST(LibcMspace, ReallocForeignPointerReturnsNullLeavesArenasUntouched) {
    // A pointer from another arena, or one that is stale, must be rejected
    // without changing either arena's first-fit allocation state.
    alignas(64) std::array<std::byte, 512> storage_a{};
    alignas(64) std::array<std::byte, 512> storage_b{};
    void* handle_a = MspaceCreate("ForeignRA", storage_a.data(), storage_a.size(), 0);
    void* handle_b = MspaceCreate("ForeignRB", storage_b.data(), storage_b.size(), 0);
    ASSERT_NE(handle_a, nullptr);
    ASSERT_NE(handle_b, nullptr);

    void* p = MspaceMalloc(handle_a, 64);
    void* q = MspaceMalloc(handle_b, 64);
    ASSERT_NE(p, nullptr);
    ASSERT_NE(q, nullptr);

    // Attempt realloc through the wrong handle.
    void* result = MspaceRealloc(handle_b, p, 128);
    EXPECT_EQ(result, nullptr);

    // Each arena's next allocation must still start exactly after its live
    // first allocation; a failed foreign realloc must not consume or free it.
    EXPECT_EQ(MspaceMalloc(handle_a, 64), static_cast<std::byte*>(p) + 64);
    EXPECT_EQ(MspaceMalloc(handle_b, 64), static_cast<std::byte*>(q) + 64);

    // p is still live in arena_a. Once it is freed, a stale realloc must also
    // leave the free list alone so first-fit returns the original interval.
    EXPECT_EQ(MspaceFree(handle_a, p), 0);
    EXPECT_EQ(MspaceRealloc(handle_a, p, 128), nullptr);
    EXPECT_EQ(MspaceMalloc(handle_a, 64), p);
    EXPECT_EQ(MspaceDestroy(handle_a), 0);
    EXPECT_EQ(MspaceDestroy(handle_b), 0);
}

// ---------------------------------------------------------------------------
// Task 5 – Step 4: shrink and adjacent in-place growth tests
// ---------------------------------------------------------------------------

TEST(LibcMspace, ReallocShrinkPreservesPrefixAndCoalescesTail) {
    // Shrink a 128-byte block to 64 bytes.  The trailing 64-byte fragment
    // must be returned to the free list, so a subsequent 64-byte allocation
    // succeeds from the same region.
    alignas(64) std::array<std::byte, 4096> storage{};
    void* handle = MspaceCreate("ShrinkArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);

    void* p = MspaceMalloc(handle, 128);
    ASSERT_NE(p, nullptr);

    // Write a recognisable pattern into the first 64 bytes.
    auto* bytes = static_cast<std::byte*>(p);
    for (std::size_t i = 0; i < 64; ++i) {
        bytes[i] = std::byte{static_cast<unsigned char>(i & 0xFF)};
    }

    void* q = MspaceRealloc(handle, p, 64);
    // Shrink is in-place and returns precisely the released tail to first-fit.
    ASSERT_EQ(q, p);

    // Prefix preserved byte-by-byte.
    const auto* qb = static_cast<const std::byte*>(q);
    for (std::size_t i = 0; i < 64; ++i) {
        EXPECT_EQ(qb[i], std::byte{static_cast<unsigned char>(i & 0xFF)})
            << "byte mismatch at index " << i;
    }

    // The freed tail must be available again.
    void* tail = MspaceMalloc(handle, 64);
    EXPECT_EQ(tail, bytes + 64);
    EXPECT_EQ(MspaceDestroy(handle), 0);
}

TEST(LibcMspace, ReallocAdjacentFreeBlockEnablesInPlaceGrowth) {
    // Lay out: [A:128][B:128][C:rest]
    // Free B so the free-list has an immediately adjacent block after A.
    // Grow A to 256 bytes in-place; it must consume B without copying.
    alignas(64) std::array<std::byte, 4096> storage{};
    void* handle = MspaceCreate("InPlaceGrowArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);

    void* a = MspaceMalloc(handle, 128);
    void* b = MspaceMalloc(handle, 128);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    // Verify adjacency: b must start immediately after a.
    const auto addr_a = reinterpret_cast<std::uintptr_t>(a);
    const auto addr_b = reinterpret_cast<std::uintptr_t>(b);
    ASSERT_EQ(addr_b, addr_a + 128u) << "a and b must be adjacent for in-place growth";

    // Write pattern to a.
    auto* ab = static_cast<std::byte*>(a);
    for (std::size_t i = 0; i < 128; ++i) {
        ab[i] = std::byte{static_cast<unsigned char>(i & 0xFF)};
    }

    EXPECT_EQ(MspaceFree(handle, b), 0); // release B → adjacent free tail

    void* grown = MspaceRealloc(handle, a, 256);
    // In-place growth must succeed and return the same address.
    ASSERT_NE(grown, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(grown), addr_a)
        << "in-place growth must keep the same base address";

    // Original bytes preserved.
    const auto* gb = static_cast<const std::byte*>(grown);
    for (std::size_t i = 0; i < 128; ++i) {
        EXPECT_EQ(gb[i], std::byte{static_cast<unsigned char>(i & 0xFF)})
            << "byte mismatch at index " << i;
    }

    // B was consumed by the enlarged allocation and cannot be handed out
    // independently. The next first-fit block begins after the grown interval.
    void* following = MspaceMalloc(handle, 128);
    EXPECT_EQ(following, static_cast<std::byte*>(a) + 256);
    EXPECT_EQ(MspaceDestroy(handle), 0);
}

// ---------------------------------------------------------------------------
// Task 5 – Step 7: moving-realloc (allocate-copy-free fallback)
// ---------------------------------------------------------------------------

TEST(LibcMspace, ReallocBlockedInPlaceGrowthMovesAndPreservesMinBytes) {
    // Lay out: [A:128][B:128 live neighbour][…rest]
    // With B still live, A cannot grow in-place.  Realloc to 192 bytes must
    // allocate elsewhere, copy min(128,192)=128 bytes, and free the old slot.
    alignas(64) std::array<std::byte, 4096> storage{};
    void* handle = MspaceCreate("MoveReallocArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);

    void* a = MspaceMalloc(handle, 128);
    void* b = MspaceMalloc(handle, 128);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    // b must immediately follow a so there really is no adjacent free space.
    const auto addr_a = reinterpret_cast<std::uintptr_t>(a);
    const auto addr_b = reinterpret_cast<std::uintptr_t>(b);
    ASSERT_EQ(addr_b, addr_a + 128u) << "layout assumption: b follows a immediately";

    // Fill a with a known pattern.
    auto* ab = static_cast<std::byte*>(a);
    for (std::size_t i = 0; i < 128; ++i) {
        ab[i] = std::byte{static_cast<unsigned char>((i * 3 + 7) & 0xFF)};
    }

    // Grow a to 192 bytes; b is still live so in-place is impossible.
    void* grown = MspaceRealloc(handle, a, 192);
    ASSERT_NE(grown, nullptr);

    // The returned pointer must be different (moved to another location).
    EXPECT_NE(reinterpret_cast<std::uintptr_t>(grown), addr_a)
        << "growth should have moved the block since b was still live";

    // min(128, 192) = 128 bytes must be preserved verbatim.
    const auto* gb = static_cast<const std::byte*>(grown);
    for (std::size_t i = 0; i < 128; ++i) {
        EXPECT_EQ(gb[i], std::byte{static_cast<unsigned char>((i * 3 + 7) & 0xFF)})
            << "byte mismatch at index " << i;
    }

    // The old slot for a must have been freed: freeing b and then requesting
    // 256 bytes should succeed (old-a-slot + b-slot = 256 bytes coalesced).
    EXPECT_EQ(MspaceFree(handle, b), 0);
    void* combined = MspaceMalloc(handle, 256);
    EXPECT_EQ(combined, a);
    EXPECT_EQ(MspaceDestroy(handle), 0);
}

TEST(LibcMspace, ReallocFailedGrowthLeavesOriginalLiveAndUnchanged) {
    // Attempt a realloc that is bigger than the entire arena.  The allocation
    // must fail, returning null, while the original block stays valid.
    alignas(64) std::array<std::byte, 512> storage{};
    void* handle = MspaceCreate("FailGrowArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);

    void* p = MspaceMalloc(handle, 64);
    ASSERT_NE(p, nullptr);

    // Poison pattern.
    auto* pb = static_cast<std::byte*>(p);
    for (std::size_t i = 0; i < 64; ++i) {
        pb[i] = std::byte{0xAB};
    }

    // Request far more memory than the arena holds.
    void* result = MspaceRealloc(handle, p, storage.size() + 1);
    EXPECT_EQ(result, nullptr);

    // Original pointer must still be live and byte-identical.
    const auto* pb2 = static_cast<const std::byte*>(p);
    for (std::size_t i = 0; i < 64; ++i) {
        EXPECT_EQ(pb2[i], std::byte{0xAB}) << "original data corrupted at index " << i;
    }
    // Freeing the original must succeed.
    EXPECT_EQ(MspaceFree(handle, p), 0);
    EXPECT_EQ(MspaceDestroy(handle), 0);
}

// ---------------------------------------------------------------------------
// Task 6 – aligned allocation and pointer-only usable-size lookup
// ---------------------------------------------------------------------------

TEST(LibcMspace, MemalignHonorsRequestedAlignmentWithinUnalignedGuestSubspan) {
    alignas(256) std::array<std::byte, 4097> storage{};
    auto* const guest_base = storage.data() + 1; // Intentionally not 16-byte aligned.
    const std::size_t guest_size = storage.size() - 1;
    void* handle = MspaceCreate("MemalignArena", guest_base, guest_size, 0);
    ASSERT_NE(handle, nullptr);

    const auto begin = reinterpret_cast<std::uintptr_t>(guest_base);
    const auto end = begin + guest_size;
    for (const std::size_t alignment : {16u, 64u, 256u}) {
        void* allocation = MspaceMemalign(handle, alignment, 31);
        ASSERT_NE(allocation, nullptr);
        const auto address = reinterpret_cast<std::uintptr_t>(allocation);
        EXPECT_GE(address, begin);
        EXPECT_LE(address + 31, end);
        EXPECT_EQ(address % alignment, 0u);
    }

    void* normalized = MspaceMemalign(handle, 8, 17);
    ASSERT_NE(normalized, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(normalized) % 16, 0u);
    EXPECT_EQ(MspaceDestroy(handle), 0);
}

TEST(LibcMspace, MemalignRejectsInvalidAndOversizedAlignments) {
    alignas(64) std::array<std::byte, 1024> storage{};
    void* handle = MspaceCreate("InvalidMemalignArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);

    EXPECT_EQ(MspaceMemalign(handle, 0, 16), nullptr);
    EXPECT_EQ(MspaceMemalign(handle, 24, 16), nullptr);
    const std::size_t oversized_power_of_two = std::numeric_limits<std::size_t>::max() / 2 + 1;
    EXPECT_EQ(MspaceMemalign(handle, oversized_power_of_two, 16), nullptr);
    EXPECT_NE(MspaceMemalign(handle, 16, storage.size()), nullptr);
    EXPECT_EQ(MspaceDestroy(handle), 0);

    const auto high_base = std::numeric_limits<std::uintptr_t>::max() - 127;
    void* overflow_handle = MspaceCreate("OverflowMemalignArena",
                                         reinterpret_cast<void*>(high_base), 1, 0);
    ASSERT_NE(overflow_handle, nullptr);
    EXPECT_EQ(MspaceMemalign(overflow_handle, 256, 1), nullptr);
    EXPECT_NE(MspaceMemalign(overflow_handle, 16, 1), nullptr);
    EXPECT_EQ(MspaceDestroy(overflow_handle), 0);
}

TEST(LibcMspace, CreateRejectsOverlappingLiveRangesAndAllowsReuseAfterDestroy) {
    alignas(64) static std::array<std::byte, 2048> storage{};
    void* first = MspaceCreate("OverlapFirst", storage.data(), 1024, 0);
    ASSERT_NE(first, nullptr);

    EXPECT_EQ(MspaceCreate("OverlapRejected", storage.data() + 512, 512, 0), nullptr);
    void* adjacent = MspaceCreate("Adjacent", storage.data() + 1024, 1024, 0);
    ASSERT_NE(adjacent, nullptr);

    EXPECT_EQ(MspaceDestroy(first), 0);
    void* reused = MspaceCreate("ReuseAfterDestroy", storage.data() + 512, 512, 0);
    ASSERT_NE(reused, nullptr);

    EXPECT_EQ(MspaceDestroy(adjacent), 0);
    EXPECT_EQ(MspaceDestroy(reused), 0);
}

TEST(LibcMspace, UsableSizeDescribesCurrentLiveAllocationAfterNumericReuse) {
    alignas(64) static std::array<std::byte, 256> storage{};
    void* first = MspaceCreate("ReuseFirst", storage.data(), storage.size(), 0);
    ASSERT_NE(first, nullptr);
    void* stale = MspaceMalloc(first, 41);
    ASSERT_NE(stale, nullptr);
    EXPECT_EQ(MspaceMallocUsableSize(stale), 41u);
    ASSERT_EQ(MspaceDestroy(first), 0);
    EXPECT_EQ(MspaceMallocUsableSize(stale), 0u);

    void* second = MspaceCreate("ReuseSecond", storage.data(), storage.size(), 0);
    ASSERT_NE(second, nullptr);
    void* current = MspaceMalloc(second, 73);
    ASSERT_EQ(current, stale);
    EXPECT_EQ(MspaceMallocUsableSize(stale), 73u);
    EXPECT_EQ(MspaceDestroy(second), 0);
}

TEST(LibcMspace, UsableSizeReportsOneByteCarvedExtentForZeroSizeRequests) {
    alignas(64) static std::array<std::byte, 512> storage{};
    void* handle = MspaceCreate("ZeroUsableSize", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);

    void* malloc_block = MspaceMalloc(handle, 0);
    void* calloc_block = MspaceCalloc(handle, 0, 7);
    void* memalign_block = MspaceMemalign(handle, 64, 0);
    ASSERT_NE(malloc_block, nullptr);
    ASSERT_NE(calloc_block, nullptr);
    ASSERT_NE(memalign_block, nullptr);
    EXPECT_EQ(MspaceMallocUsableSize(malloc_block), 1u);
    EXPECT_EQ(MspaceMallocUsableSize(calloc_block), 1u);
    EXPECT_EQ(MspaceMallocUsableSize(memalign_block), 1u);

    ASSERT_EQ(MspaceFree(handle, malloc_block), 0);
    EXPECT_EQ(MspaceMallocUsableSize(malloc_block), 0u);
    ASSERT_EQ(MspaceDestroy(handle), 0);
    EXPECT_EQ(MspaceMallocUsableSize(calloc_block), 0u);
    EXPECT_EQ(MspaceMallocUsableSize(memalign_block), 0u);
}

TEST(LibcMspace, UsableSizeReportsExactLiveRequestsAndRejectsInvalidPointers) {
    // Static guest storage avoids overlapping stack-backed arenas left live by
    // earlier tests; production mspace guest ranges are likewise distinct.
    alignas(64) static std::array<std::byte, 4096> storage{};
    void* handle = MspaceCreate("UsableSizeArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);

    void* malloc_block = MspaceMalloc(handle, 23);
    void* calloc_block = MspaceCalloc(handle, 3, 11);
    void* realloc_block = MspaceMalloc(handle, 19);
    void* memalign_block = MspaceMemalign(handle, 64, 59);
    ASSERT_NE(malloc_block, nullptr);
    ASSERT_NE(calloc_block, nullptr);
    ASSERT_NE(realloc_block, nullptr);
    ASSERT_NE(memalign_block, nullptr);
    realloc_block = MspaceRealloc(handle, realloc_block, 47);
    ASSERT_NE(realloc_block, nullptr);

    EXPECT_EQ(MspaceMallocUsableSize(malloc_block), 23u);
    EXPECT_EQ(MspaceMallocUsableSize(calloc_block), 33u);
    EXPECT_EQ(MspaceMallocUsableSize(realloc_block), 47u);
    EXPECT_EQ(MspaceMallocUsableSize(memalign_block), 59u);
    EXPECT_EQ(MspaceMallocUsableSize(nullptr), 0u);
    std::byte foreign{};
    EXPECT_EQ(MspaceMallocUsableSize(&foreign), 0u);

    ASSERT_EQ(MspaceFree(handle, malloc_block), 0);
    EXPECT_EQ(MspaceMallocUsableSize(malloc_block), 0u);

    ASSERT_EQ(MspaceDestroy(handle), 0);
    EXPECT_EQ(MspaceMallocUsableSize(calloc_block), 0u);
    EXPECT_EQ(MspaceMallocUsableSize(realloc_block), 0u);
    EXPECT_EQ(MspaceMallocUsableSize(memalign_block), 0u);
}

TEST(LibcMspace, UsableSizeFindsLiveBlocksAcrossArenasWithoutCrossArenaOwnership) {
    alignas(64) static std::array<std::byte, 1024> storage_a{};
    alignas(64) static std::array<std::byte, 1024> storage_b{};
    void* handle_a = MspaceCreate("UsableSizeArenaA", storage_a.data(), storage_a.size(), 0);
    void* handle_b = MspaceCreate("UsableSizeArenaB", storage_b.data(), storage_b.size(), 0);
    ASSERT_NE(handle_a, nullptr);
    ASSERT_NE(handle_b, nullptr);

    void* block_a = MspaceMalloc(handle_a, 41);
    void* block_b = MspaceMalloc(handle_b, 73);
    ASSERT_NE(block_a, nullptr);
    ASSERT_NE(block_b, nullptr);

    EXPECT_EQ(MspaceMallocUsableSize(block_a), 41u);
    EXPECT_EQ(MspaceMallocUsableSize(block_b), 73u);
    EXPECT_EQ(MspaceFree(handle_a, block_b), 1);
    EXPECT_EQ(MspaceMallocUsableSize(block_b), 73u);
    EXPECT_EQ(MspaceDestroy(handle_a), 0);
    EXPECT_EQ(MspaceDestroy(handle_b), 0);
    EXPECT_EQ(MspaceMallocUsableSize(block_a), 0u);
    EXPECT_EQ(MspaceMallocUsableSize(block_b), 0u);
}

} // namespace
