// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

[[nodiscard]] inline u64 HashCombine(const u64 seed, const u64 hash) {
    return seed ^ (hash + 0x9e3779b9 + (seed << 12) + (seed >> 4));
}

[[nodiscard]] inline u32 HashCombine(const u32 seed, const u32 hash) {
    return seed ^ (hash + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

/// Hash of the integer keys of tables that mask the hash with a power of two, as tsl::robin_map
/// does. The std::hash of MSVC runs FNV-1a over every byte of the key, a chain of eight
/// multiplications.
struct IntegerKeyHash {
    [[nodiscard]] std::size_t operator()(u64 key) const noexcept {
        key ^= key >> 33;
        key *= 0xFF51AFD7ED558CCDULL;
        key ^= key >> 33;
        return static_cast<std::size_t>(key);
    }
};