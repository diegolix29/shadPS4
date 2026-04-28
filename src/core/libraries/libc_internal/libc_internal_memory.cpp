// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cerrno>
#include <cstdlib>
#include <cstring>

#ifdef _WIN64
#include <malloc.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "libc_internal_memory.h"

namespace Libraries::LibcInternal {

void* PS4_SYSV_ABI internal_memset(void* s, int c, size_t n) {
    return std::memset(s, c, n);
}

void* PS4_SYSV_ABI internal_memcpy(void* dest, const void* src, size_t n) {
    return std::memcpy(dest, src, n);
}

s32 PS4_SYSV_ABI internal_memcpy_s(void* dest, size_t destsz, const void* src, size_t count) {
#ifdef _WIN64
    return memcpy_s(dest, destsz, src, count);
#else
    std::memcpy(dest, src, count);
    return 0; // ALL OK
#endif
}

s32 PS4_SYSV_ABI internal_memcmp(const void* s1, const void* s2, size_t n) {
    return std::memcmp(s1, s2, n);
}

// --- sceLibcMspace --- Host-delegated allocator for PS4 mspace heaps ---
//
// PS4 games and system libraries use mspace (dlmalloc's memory space API) to
// create private heaps. The game calls sceLibcMspaceCreate with a pre-mapped
// memory region, then allocates/frees through the mspace handle.
//
// We delegate all allocations to the host C library (malloc/free/posix_memalign)
// instead of managing the pre-mapped region ourselves. This is necessary because
// games like Gravity Rush 2 use their main mspace for >10,000 allocations with
// active free/realloc, which a simple bump allocator cannot support.
//
// The mspace handle (returned by Create) is stored in a header at the start of
// the game's pre-mapped region for validation, but actual allocations come from
// the host heap. Host and guest share the same address space on shadPS4, so the
// returned pointers are usable by guest code.

struct MspaceHeader {
    u64 magic;       // 0x4D53504143450000 ("MSPACE\0\0")
    void* base;      // start of managed region (not used for allocations)
    size_t capacity; // total size of region (informational only)
    size_t used;     // approximate bytes allocated (for stats/debugging)
};

static constexpr u64 MSPACE_MAGIC = 0x4D53504143450000ULL;

// --- Platform-abstracted aligned allocation ---
// On Linux/macOS, posix_memalign returns pointers freeable with free().
// On Windows, _aligned_malloc requires _aligned_free.

static void* AlignedAlloc(size_t alignment, size_t size) {
    if (alignment < sizeof(void*)) {
        alignment = sizeof(void*);
    }
    // Ensure alignment is a power of two (required by posix_memalign)
    if ((alignment & (alignment - 1)) != 0) {
        size_t a = 1;
        while (a < alignment) {
            a <<= 1;
        }
        alignment = a;
    }
    if (size == 0) {
        size = 1; // POSIX requires non-zero size for posix_memalign
    }
#ifdef _WIN64
    return _aligned_malloc(size, alignment);
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
    return ptr;
#endif
}

static void AlignedFree(void* ptr) {
    if (!ptr) {
        return;
    }
#ifdef _WIN64
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

static void* AlignedRealloc(void* ptr, size_t new_size) {
    if (new_size == 0) {
        AlignedFree(ptr);
        return nullptr;
    }
#ifdef _WIN64
    return _aligned_realloc(ptr, new_size, 16);
#else
    // On POSIX, realloc works on posix_memalign'd pointers
    return realloc(ptr, new_size);
#endif
}

static size_t HostUsableSize(void* ptr) {
    if (!ptr) {
        return 0;
    }
#ifdef _WIN64
    return _aligned_msize(ptr, 16, 0);
#elif defined(__APPLE__)
    return malloc_size(ptr);
#else
    return malloc_usable_size(ptr);
#endif
}

// --- sceLibcMspace API implementation ---

void* PS4_SYSV_ABI sceLibcMspaceCreate(const char* name, void* base, size_t capacity,
                                         u32 flag) {
    if (!base || capacity < sizeof(MspaceHeader)) {
        LOG_ERROR(Lib_LibcInternal, "sceLibcMspaceCreate: invalid args base={} cap={:#x}",
                  base, capacity);
        return nullptr;
    }

    auto* hdr = static_cast<MspaceHeader*>(base);
    hdr->magic = MSPACE_MAGIC;
    hdr->base = base;
    hdr->capacity = capacity;
    hdr->used = 0;

    LOG_INFO(Lib_LibcInternal, "sceLibcMspaceCreate: name='{}' base={} cap={:#x} → msp={}",
             name ? name : "(null)", base, capacity, base);
    return base;
}

void* PS4_SYSV_ABI sceLibcMspaceMalloc(void* msp, size_t size) {
    if (!msp) {
        LOG_ERROR(Lib_LibcInternal, "sceLibcMspaceMalloc: null mspace");
        return nullptr;
    }

    void* ptr = AlignedAlloc(16, size);
    if (ptr) {
        std::memset(ptr, 0, size);
    }

    LOG_DEBUG(Lib_LibcInternal, "sceLibcMspaceMalloc: msp={} size={:#x} → {}",
              msp, size, ptr);
    return ptr;
}

void PS4_SYSV_ABI sceLibcMspaceFree(void* msp, void* ptr) {
    LOG_DEBUG(Lib_LibcInternal, "sceLibcMspaceFree: msp={} ptr={}", msp, ptr);
    AlignedFree(ptr);
}

s32 PS4_SYSV_ABI sceLibcMspaceDestroy(void* msp) {
    if (!msp) {
        return -1;
    }

    auto* hdr = static_cast<MspaceHeader*>(msp);
    if (hdr->magic == MSPACE_MAGIC) {
        LOG_INFO(Lib_LibcInternal, "sceLibcMspaceDestroy: msp={}", msp);
        hdr->magic = 0; // invalidate
    }
    return 0;
}

// --- New functions: PosixMemalign, MallocUsableSize, Memalign, Realloc, Calloc ---

int PS4_SYSV_ABI sceLibcMspacePosixMemalign(void* msp, void** memptr,
                                              size_t alignment, size_t size) {
    // int sceLibcMspacePosixMemalign(void* msp, void** memptr, size_t alignment, size_t size)
    //
    // The game's primary aligned allocator — 10,295 call sites in GR2's eboot.
    // Previously auto-stubbed: returned 0 (success) without writing *memptr,
    // so every caller got NULL and crashed dereferencing at +0x100.
    //
    // Returns 0 on success (writes allocated pointer to *memptr), ENOMEM on failure.

    if (!memptr) {
        LOG_ERROR(Lib_LibcInternal, "sceLibcMspacePosixMemalign: null memptr");
        return ENOMEM;
    }

    void* ptr = AlignedAlloc(alignment, size);
    if (!ptr) {
        LOG_ERROR(Lib_LibcInternal,
                  "sceLibcMspacePosixMemalign: OOM msp={} align={:#x} size={:#x}",
                  msp, alignment, size);
        *memptr = nullptr;
        return ENOMEM;
    }

    std::memset(ptr, 0, size);
    *memptr = ptr;

    LOG_DEBUG(Lib_LibcInternal,
              "sceLibcMspacePosixMemalign: msp={} align={:#x} size={:#x} → {}",
              msp, alignment, size, ptr);
    return 0;
}

size_t PS4_SYSV_ABI sceLibcMspaceMallocUsableSize(void* ptr) {
    // size_t sceLibcMspaceMallocUsableSize(void* ptr)
    //
    // Single-argument — confirmed by RE of 21,338 call sites in GR2's eboot:
    // only rdi (ptr) is set before the call; rsi is never explicitly loaded.
    // Despite the "Mspace" in the name, the function operates directly on the
    // allocation's metadata (like dlmalloc_usable_size) without needing the
    // mspace handle.

    size_t usable = HostUsableSize(ptr);
    LOG_DEBUG(Lib_LibcInternal,
              "sceLibcMspaceMallocUsableSize: ptr={} → {:#x}", ptr, usable);
    return usable;
}

void* PS4_SYSV_ABI sceLibcMspaceMemalign(void* msp, size_t alignment, size_t size) {
    // void* sceLibcMspaceMemalign(void* msp, size_t alignment, size_t size)
    // Returns aligned pointer or nullptr on failure. 45 call sites in GR2.

    void* ptr = AlignedAlloc(alignment, size);
    if (ptr) {
        std::memset(ptr, 0, size);
    }

    LOG_DEBUG(Lib_LibcInternal,
              "sceLibcMspaceMemalign: msp={} align={:#x} size={:#x} → {}",
              msp, alignment, size, ptr);
    return ptr;
}

void* PS4_SYSV_ABI sceLibcMspaceRealloc(void* msp, void* ptr, size_t size) {
    // void* sceLibcMspaceRealloc(void* msp, void* ptr, size_t size)
    // Reallocates ptr to new size. 11 call sites in GR2.

    void* new_ptr = AlignedRealloc(ptr, size);

    LOG_DEBUG(Lib_LibcInternal,
              "sceLibcMspaceRealloc: msp={} ptr={} size={:#x} → {}",
              msp, ptr, size, new_ptr);
    return new_ptr;
}

void* PS4_SYSV_ABI sceLibcMspaceCalloc(void* msp, size_t nmemb, size_t size) {
    // void* sceLibcMspaceCalloc(void* msp, size_t nmemb, size_t size)
    // Allocates zeroed memory for nmemb elements of given size. 1 call site in GR2.

    const size_t total = nmemb * size;
    void* ptr = AlignedAlloc(16, total);
    if (ptr) {
        std::memset(ptr, 0, total);
    }

    LOG_DEBUG(Lib_LibcInternal,
              "sceLibcMspaceCalloc: msp={} nmemb={} size={:#x} → {}",
              msp, nmemb, size, ptr);
    return ptr;
}

// --- Standard libc functions needed by game code ---
// These are imported via libSceLibcInternal NIDs but were previously only
// auto-stubbed (returning 0), which silently broke data movement and allocation.

void* PS4_SYSV_ABI internal_memmove(void* dest, const void* src, size_t n) {
    return std::memmove(dest, src, n);
}

int PS4_SYSV_ABI internal_posix_memalign(void** memptr, size_t alignment, size_t size) {
    if (!memptr) {
        return EINVAL;
    }
    void* ptr = AlignedAlloc(alignment, size);
    if (!ptr) {
        *memptr = nullptr;
        return ENOMEM;
    }
    std::memset(ptr, 0, size);
    *memptr = ptr;
    return 0;
}

int PS4_SYSV_ABI sceLibcMspaceMallocStatsFast(void* msp, void* stats_out) {
    // int sceLibcMspaceMallocStatsFast(void* msp, void* stats_out)
    // Fills stats struct with allocation statistics. 4 call sites in GR2.
    // We zero the output struct and return 0 (success).

    if (stats_out) {
        // dlmalloc mallinfo struct is typically 0x30 bytes (10 ints + padding).
        std::memset(stats_out, 0, 0x30);
    }

    LOG_DEBUG(Lib_LibcInternal,
              "sceLibcMspaceMallocStatsFast: msp={} → 0", msp);
    return 0;
}

void RegisterlibSceLibcInternalMemory(Core::Loader::SymbolsResolver* sym) {

    LIB_FUNCTION("NFLs+dRJGNg", "libSceLibcInternal", 1, "libSceLibcInternal", internal_memcpy_s);
    LIB_FUNCTION("Q3VBxCXhUHs", "libSceLibcInternal", 1, "libSceLibcInternal", internal_memcpy);
    LIB_FUNCTION("8zTFvBIAIN8", "libSceLibcInternal", 1, "libSceLibcInternal", internal_memset);
    LIB_FUNCTION("DfivPArhucg", "libSceLibcInternal", 1, "libSceLibcInternal", internal_memcmp);

    // sceLibcMspace — PS4 mspace (dlmalloc memory space) allocator.
    // Used by both games (main heap) and system libraries (private pools).
    // All allocations delegated to host C library for proper free/realloc support.
    LIB_FUNCTION("-hn1tcVHq5Q", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceCreate);
    LIB_FUNCTION("OJjm-QOIHlI", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceMalloc);
    LIB_FUNCTION("Vla-Z+eXlxo", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceFree);
    LIB_FUNCTION("W6SiVSiCDtI", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceDestroy);

    // Aligned allocation — the game's primary allocator (10,295 call sites in GR2).
    // Previously auto-stubbed in aerolib.inl (returned 0 without writing output
    // pointer, causing all callers to dereference NULL → crash at +0x100).
    LIB_FUNCTION("qWESlyXMI3E", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspacePosixMemalign);

    // Usable size query — 21,353 call sites in GR2. Single-arg (ptr only).
    LIB_FUNCTION("fEoW6BJsPt4", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceMallocUsableSize);

    // Additional mspace allocator functions used by the game.
    LIB_FUNCTION("iF1iQHzxBJU", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceMemalign);
    LIB_FUNCTION("gigoVHZvVPE", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceRealloc);
    LIB_FUNCTION("LYo3GhIlB38", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceCalloc);
    LIB_FUNCTION("k04jLXu3+Ic", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceMallocStatsFast);

    // Standard libc functions — previously auto-stubbed (returned 0 silently).
    // memmove: used by std::vector reallocation in game code (10,000+ call sites).
    // posix_memalign: used by fiber initialization in the content listing system.
    LIB_FUNCTION("+P6FRGH4LfA", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_memmove);
    LIB_FUNCTION("cVSk9y8URbc", "libSceLibcInternal", 1, "libSceLibcInternal",
                 internal_posix_memalign);
}

} // namespace Libraries::LibcInternal
