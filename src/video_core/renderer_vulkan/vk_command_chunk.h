// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/alignment.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

/// Arena of commands filled by the command processor and replayed in order into a Vulkan
/// command buffer by the recording thread. A chunk is owned by one thread at a time; the
/// scheduler hands it over under a mutex.
class CommandChunk {
public:
    static constexpr size_t Capacity = 64 * 1024;
    /// Payloads above this size go to the heap, so one long copy list neither overflows a chunk
    /// nor wastes most of one.
    static constexpr size_t LargePayloadSize = 8 * 1024;
    static constexpr size_t PayloadAlignment = 16;

    CommandChunk() = default;
    CommandChunk(const CommandChunk&) = delete;
    CommandChunk& operator=(const CommandChunk&) = delete;

    /// Appends func, called later as func(vk::CommandBuffer). Returns false when the chunk is
    /// full, in which case func is left untouched.
    template <typename Func>
    bool Record(Func&& func) {
        using Command = TypedCommand<std::decay_t<Func>>;
        static_assert(sizeof(Command) <= Capacity / 8, "Command is too large");
        void* const storage = Allocate(sizeof(Command), alignof(Command));
        if (storage == nullptr) {
            return false;
        }
        Link(new (storage) Command(std::forward<Func>(func)));
        return true;
    }

    /// Appends func, called later as func(vk::CommandBuffer, const std::byte* payload), together
    /// with size bytes of payload. Returns the payload for the caller to fill before recording
    /// anything else, or nullptr when the chunk is full, in which case func is left untouched.
    template <typename Func>
    std::byte* RecordWithPayload(size_t size, Func&& func) {
        using Command = PayloadCommand<std::decay_t<Func>>;
        static_assert(sizeof(Command) <= Capacity / 8, "Command is too large");
        const size_t mark = offset;
        std::byte* payload;
        if (size > LargePayloadSize) {
            payload = nullptr;
        } else {
            payload = static_cast<std::byte*>(Allocate(size, PayloadAlignment));
            if (payload == nullptr) {
                return nullptr;
            }
        }
        void* const storage = Allocate(sizeof(Command), alignof(Command));
        if (storage == nullptr) {
            offset = mark;
            return nullptr;
        }
        if (payload == nullptr) {
            payload = large_payloads.emplace_back(new std::byte[size]).get();
        }
        Link(new (storage) Command(std::forward<Func>(func), payload));
        return payload;
    }

    /// Replays every command in recording order.
    void ExecuteAll(vk::CommandBuffer cmdbuf) const {
        for (const CommandBase* command = first; command != nullptr; command = command->next) {
            command->execute(command, cmdbuf);
        }
    }

    /// Forgets every command so the chunk can be filled again.
    void Reset() noexcept {
        first = nullptr;
        last = nullptr;
        offset = 0;
        num_commands = 0;
        large_payloads.clear();
    }

    [[nodiscard]] bool Empty() const noexcept {
        return num_commands == 0;
    }

    [[nodiscard]] u32 NumCommands() const noexcept {
        return num_commands;
    }

    [[nodiscard]] size_t UsedBytes() const noexcept {
        return offset;
    }

private:
    struct CommandBase {
        using ExecuteFn = void (*)(const CommandBase*, vk::CommandBuffer);
        ExecuteFn execute;
        CommandBase* next;
    };

    // Commands are never destroyed: chunks are reset and refilled.
    template <typename Func>
    struct TypedCommand final : CommandBase {
        static_assert(std::is_trivially_destructible_v<Func>,
                      "Recorded commands must own their data by value");

        template <typename F>
        explicit TypedCommand(F&& func_)
            : CommandBase{&Execute, nullptr}, func{std::forward<F>(func_)} {}

        static void Execute(const CommandBase* base, vk::CommandBuffer cmdbuf) {
            static_cast<const TypedCommand*>(base)->func(cmdbuf);
        }

        Func func;
    };

    template <typename Func>
    struct PayloadCommand final : CommandBase {
        static_assert(std::is_trivially_destructible_v<Func>,
                      "Recorded commands must own their data by value");

        template <typename F>
        PayloadCommand(F&& func_, const std::byte* payload_)
            : CommandBase{&Execute, nullptr}, func{std::forward<F>(func_)}, payload{payload_} {}

        static void Execute(const CommandBase* base, vk::CommandBuffer cmdbuf) {
            const auto* self = static_cast<const PayloadCommand*>(base);
            self->func(cmdbuf, self->payload);
        }

        Func func;
        const std::byte* payload;
    };

    void* Allocate(size_t size, size_t alignment) noexcept {
        const size_t begin = Common::AlignUp(offset, alignment);
        if (begin + size > Capacity) {
            return nullptr;
        }
        offset = begin + size;
        return storage.data() + begin;
    }

    void Link(CommandBase* command) noexcept {
        if (last != nullptr) {
            last->next = command;
        } else {
            first = command;
        }
        last = command;
        ++num_commands;
    }

    alignas(64) std::array<std::byte, Capacity> storage;
    size_t offset{};
    CommandBase* first{};
    CommandBase* last{};
    u32 num_commands{};
    std::vector<std::unique_ptr<std::byte[]>> large_payloads;
};

} // namespace Vulkan
