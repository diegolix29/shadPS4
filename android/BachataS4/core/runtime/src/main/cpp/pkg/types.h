// Minimal integer types for the Android PKG extractor (shadPS4-compatible names).
#pragma once

#include <cstdint>
#include <cstring>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// Big-endian wrappers matching shadPS4 layout (values stored big-endian on disk).
template <typename T>
struct be {
    T raw{};
    operator T() const {
        if constexpr (sizeof(T) == 2) {
            return static_cast<T>((raw >> 8) | (raw << 8));
        } else if constexpr (sizeof(T) == 4) {
            auto v = static_cast<std::uint32_t>(raw);
            v = ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
                ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
            return static_cast<T>(v);
        } else if constexpr (sizeof(T) == 8) {
            auto v = static_cast<std::uint64_t>(raw);
            v = ((v & 0x00000000000000FFull) << 56) | ((v & 0x000000000000FF00ull) << 40) |
                ((v & 0x0000000000FF0000ull) << 24) | ((v & 0x00000000FF000000ull) << 8) |
                ((v & 0x000000FF00000000ull) >> 8) | ((v & 0x0000FF0000000000ull) >> 24) |
                ((v & 0x00FF000000000000ull) >> 40) | ((v & 0xFF00000000000000ull) >> 56);
            return static_cast<T>(v);
        } else {
            return raw;
        }
    }
};

using u16_be = be<u16>;
using u32_be = be<u32>;
using u64_be = be<u64>;
