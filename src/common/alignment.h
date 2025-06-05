// SPDX-FileCopyrightText: 2014 Jannik Vogel <email@jannikvogel.de>
// SPDX-License-Identifier: CC0-1.0

#pragma once

#include <cstddef>
#include <type_traits>

namespace Common {

template <typename T, typename U>
[[nodiscard]] constexpr auto AlignUp(T value, U alignment) {
    static_assert(std::is_integral_v<T> && std::is_integral_v<U>, "Both types must be integers.");
    using R = std::make_unsigned_t<std::common_type_t<T, U>>;
    return (static_cast<R>(value) + static_cast<R>(alignment) - 1) &
           ~(static_cast<R>(alignment) - 1);
}

template <typename T>
[[nodiscard]] constexpr T AlignDown(T value, std::size_t size) {
    static_assert(std::is_unsigned_v<T>, "T must be an unsigned value.");
    return static_cast<T>(value - value % size);
}

template <typename T>
    requires std::is_integral_v<T>
[[nodiscard]] constexpr bool IsAligned(T value, std::size_t alignment) {
    return (value & (alignment - 1)) == 0;
}

template <typename T>
    requires std::is_integral_v<T>
[[nodiscard]] constexpr bool Is16KBAligned(T value) {
    return (value & 0x3FFF) == 0;
}

template <typename T>
    requires std::is_integral_v<T>
[[nodiscard]] constexpr bool Is64KBAligned(T value) {
    return (value & 0xFFFF) == 0;
}

template <typename T>
    requires std::is_integral_v<T>
[[nodiscard]] constexpr bool Is2MBAligned(T value) {
    return (value & 0x1FFFFF) == 0;
}

} // namespace Common
