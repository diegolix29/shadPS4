// SPDX-License-Identifier: MIT
#pragma once

#include "common/types.h"
#include "guest_cpu.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <unordered_map>
#include <vector>

namespace Core::GuestCpu {

// This is the x86-64 SysV state at the FEX syscall boundary. Guest pointers
// remain guest virtual addresses; their range is checked before native HLE sees
// them. The caller owns the validator and any mapping lifetime it depends on.
struct HleCallFrame final {
    u64 operation{};
    std::array<u64, 16> gpr{};
    std::array<std::array<u64, 2>, 16> xmm{};
    std::uintptr_t rsp{};
    bool (*validate_range)(void* context, std::uintptr_t address, std::size_t size,
                           bool writable){};
    void* validate_context{};
    bool (*publish_host_range)(void* context, std::uintptr_t address, std::size_t size,
                               bool writable){};
    bool (*revoke_host_range)(void* context, std::uintptr_t address){};
    void* host_range_context{};
};

[[nodiscard]] bool PublishHostRange(const void* pointer, std::size_t size, bool writable);
[[nodiscard]] bool RevokeHostRange(const void* pointer);

struct HleCallFailure final {
    int error{};
    std::string_view name;
};

using HleCallResult = std::variant<bool, HleCallFailure>;

class HleCallAdapter {
public:
    virtual ~HleCallAdapter() = default;

    [[nodiscard]] u64 Operation() const noexcept { return operation; }
    [[nodiscard]] std::string_view Name() const noexcept { return name; }
    virtual HleCallResult Invoke(HleCallFrame& frame) const = 0;

private:
    friend class HleCallRegistry;

    void AssignOperation(u64 operation_, std::string_view name_) {
        operation = operation_;
        name = name_;
    }

    u64 operation{};
    std::string name;
};

class HleCallRegistry final {
public:
    std::shared_ptr<HleCallAdapter> Register(std::shared_ptr<HleCallAdapter> adapter,
                                             std::string_view name);
    [[nodiscard]] std::shared_ptr<HleCallAdapter> Find(u64 operation) const;

private:
    mutable std::shared_mutex registry_mutex;
    u64 next_operation{1};
    std::vector<std::shared_ptr<HleCallAdapter>> adapters;
};

struct HleVeneerFailure final {
    int error{};
};

using HleVeneerResult = std::variant<u64, HleVeneerFailure>;

class HleVeneerAllocator final {
public:
    HleVeneerAllocator() = default;
    HleVeneerAllocator(const HleVeneerAllocator&) = delete;
    HleVeneerAllocator& operator=(const HleVeneerAllocator&) = delete;
    ~HleVeneerAllocator();

    HleVeneerResult Allocate(const HleCallAdapter& adapter);

    [[nodiscard]] std::vector<GuestExecutionRange> GetExecutableRanges() const;
    // Dynamic modules allocate veneers after FEX thread start. FEX JIT must
    // discover those pages through QueryGuestExecutableRange, not only the
    // MappedRanges snapshot taken at Run().
    [[nodiscard]] std::optional<GuestExecutionRange> QueryExecutableRange(
        std::uintptr_t address) const;

private:
    struct Allocation final {
        void* page{};
        std::size_t size{};
    };

    mutable std::mutex allocator_mutex;
    std::vector<Allocation> allocations;
    std::vector<GuestExecutionRange> executable_ranges;
    std::unordered_map<u64, u64> veneers;
};

namespace detail {

constexpr std::array<std::size_t, 6> SysVIntegerArgs{7, 6, 2, 1, 8, 9};

struct CallCursor final {
    HleCallFrame& frame;
    std::size_t integer_index{};
    std::size_t vector_index{};
    std::size_t stack_offset{sizeof(u64)};

    std::optional<u64> NextInteger() {
        if (integer_index < SysVIntegerArgs.size()) {
            return frame.gpr[SysVIntegerArgs[integer_index++]];
        }
        return NextStackSlot();
    }

    std::optional<u64> NextVector() {
        if (vector_index < 8) {
            return frame.xmm[vector_index++][0];
        }
        return NextStackSlot();
    }

private:
    std::optional<u64> NextStackSlot() {
        if (frame.rsp == 0 || frame.rsp > std::numeric_limits<std::uintptr_t>::max() - stack_offset ||
            frame.validate_range == nullptr ||
            !frame.validate_range(frame.validate_context, frame.rsp + stack_offset, sizeof(u64),
                                  false)) {
            return std::nullopt;
        }
        u64 value{};
        std::memcpy(&value, reinterpret_cast<const void*>(frame.rsp + stack_offset), sizeof(value));
        stack_offset += sizeof(u64);
        return value;
    }
};

template <typename T>
inline constexpr bool IsGuestPointer = std::is_pointer_v<T>;

template <typename T>
inline constexpr bool IsGuestFunctionPointer =
    IsGuestPointer<T> && std::is_function_v<std::remove_pointer_t<T>>;

template <typename T>
inline constexpr bool IsIntegerArgument = std::is_integral_v<T> || std::is_enum_v<T> ||
                                          IsGuestPointer<T>;

template <typename T>
inline constexpr bool IsVectorArgument = std::is_same_v<T, float> || std::is_same_v<T, double>;

template <typename T>
inline constexpr bool FitsIntegerRegister = [] {
    if constexpr (IsIntegerArgument<T>) {
        return sizeof(T) <= sizeof(u64);
    }
    return false;
}();

template <typename T>
inline constexpr bool IsSupportedArgument = FitsIntegerRegister<T> || IsVectorArgument<T>;

template <typename T>
inline constexpr bool IsSupportedReturn = std::is_void_v<T> ||
                                          FitsIntegerRegister<T> ||
                                          IsVectorArgument<T>;

template <typename T>
std::optional<T> DecodeArgument(CallCursor& cursor) {
    if constexpr (IsVectorArgument<T>) {
        const auto value = cursor.NextVector();
        if (!value) return std::nullopt;
        T result{};
        std::memcpy(&result, &*value, sizeof(result));
        return result;
    } else if constexpr (IsGuestFunctionPointer<T>) {
        const auto value = cursor.NextInteger();
        if (!value) return std::nullopt;
        return reinterpret_cast<T>(static_cast<std::uintptr_t>(*value));
    } else if constexpr (IsGuestPointer<T>) {
        const auto value = cursor.NextInteger();
        if (!value) return std::nullopt;
        if (*value != 0) {
            using PointedTo = std::remove_cv_t<std::remove_pointer_t<T>>;
            constexpr std::size_t minimum_size = [] {
                if constexpr (std::is_void_v<PointedTo> || !requires { sizeof(PointedTo); }) {
                    return std::size_t{1};
                } else {
                    return sizeof(PointedTo);
                }
            }();
            // Guest VMM only. HLE opaque handles (host new) live outside VMM;
            // publish via PublishHostRange when possible. If still unmapped, allow
            // non-null host-side addresses so FEX matches native x86 handle semantics.
            if (cursor.frame.validate_range == nullptr ||
                !cursor.frame.validate_range(cursor.frame.validate_context,
                                             static_cast<std::uintptr_t>(*value), minimum_size,
                                             !std::is_const_v<std::remove_pointer_t<T>>)) {
                if (*value < 4096) {
                    return std::nullopt;
                }
            }
        }
        return reinterpret_cast<T>(static_cast<std::uintptr_t>(*value));
    } else {
        const auto value = cursor.NextInteger();
        if (!value) return std::nullopt;
        return static_cast<T>(*value);
    }
}

template <typename Return>
void EncodeReturn(HleCallFrame& frame, Return&& value) {
    using T = std::remove_cvref_t<Return>;
    if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
        u64 bits{};
        std::memcpy(&bits, &value, sizeof(value));
        frame.xmm[0] = {bits, 0};
    } else if constexpr (IsGuestPointer<T>) {
        frame.gpr[0] = reinterpret_cast<std::uintptr_t>(value);
    } else {
        frame.gpr[0] = static_cast<u64>(value);
    }
}

template <typename Function>
class TypedHleCallAdapter;

template <typename Return, typename... Args>
class TypedHleCallAdapter<Return (*)(Args...)> final : public HleCallAdapter {
public:
    explicit TypedHleCallAdapter(Return (*function_)(Args...)) : function{function_} {}

    HleCallResult Invoke(HleCallFrame& frame) const override {
        if constexpr ((!IsSupportedArgument<Args> || ...) || !IsSupportedReturn<Return>) {
            return HleCallFailure{ENOTSUP, Name()};
        } else {
            CallCursor cursor{frame};
            std::tuple<std::optional<Args>...> decoded{DecodeArgument<Args>(cursor)...};
            if (!AllDecoded(decoded, std::index_sequence_for<Args...>{})) {
                return HleCallFailure{EFAULT, Name()};
            }
            if constexpr (std::is_void_v<Return>) {
                InvokeVoid(decoded, std::index_sequence_for<Args...>{});
                frame.gpr[0] = 0;
            } else {
                const auto result = InvokeResult(decoded, std::index_sequence_for<Args...>{});
                EncodeReturn(frame, result);
            }
            return true;
        }
    }

private:
    template <std::size_t... Index>
    static bool AllDecoded(const std::tuple<std::optional<Args>...>& decoded,
                           std::index_sequence<Index...>) {
        return (std::get<Index>(decoded).has_value() && ...);
    }

    template <std::size_t... Index>
    void InvokeVoid(const std::tuple<std::optional<Args>...>& decoded,
                    std::index_sequence<Index...>) const {
        function(*std::get<Index>(decoded)...);
    }

    template <std::size_t... Index>
    Return InvokeResult(const std::tuple<std::optional<Args>...>& decoded,
                        std::index_sequence<Index...>) const {
        return function(*std::get<Index>(decoded)...);
    }

    Return (*function)(Args...);
};

class UnsupportedHleCallAdapter final : public HleCallAdapter {
public:
    HleCallResult Invoke(HleCallFrame&) const override {
        return HleCallFailure{ENOSYS, Name()};
    }
};

} // namespace detail

template <typename Function>
std::shared_ptr<HleCallAdapter> MakeHleCallAdapter(Function function) {
    using Signature = decltype(function);
    return std::make_shared<detail::TypedHleCallAdapter<Signature>>(function);
}

inline std::shared_ptr<HleCallAdapter> MakeUnsupportedHleCallAdapter() {
    return std::make_shared<detail::UnsupportedHleCallAdapter>();
}

} // namespace Core::GuestCpu
