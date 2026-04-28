// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <queue>

#include "core/libraries/avplayer/avplayer.h"

#define AVPLAYER_IS_ERROR(x) ((x) < 0)

namespace Libraries::AvPlayer {

enum class AvState {
    Unknown,
    Initial,
    AddingSource,
    Ready,
    Play,
    Stop,
    EndOfFile,
    Pause,
    C0x08,
    Jump,
    TrickMode,
    C0x0B,
    Buffering,
    Starting,
    Error,
};

enum class AvEventType {
    ChangeFlowState = 21,
    WarningId = 22,
    RevertState = 30,
    AddSource = 40,
    Error = 255,
};

union AvPlayerEventData {
    u32 num_frames; // 20
    AvState state;  // AvEventType::ChangeFlowState
    s32 error;      // AvEventType::WarningId
    u32 attempt;    // AvEventType::AddSource
};

struct AvPlayerEvent {
    AvEventType event;
    AvPlayerEventData payload;
};

template <class T>
class AvPlayerQueue {
public:
    // FIX(GR2FORK v4): lock the size check. Previously Size() was read
    // without the mutex, which produced the classic size-then-pop TOCTOU:
    //   thread A: Size()==1, enters pop branch
    //   thread B: Size()==1, enters pop branch
    //   thread A: locks, front()+pop(), unlocks  → queue now empty
    //   thread B: locks, front() on EMPTY queue (UB), pop() corrupts
    // Observable symptom: silent process death during avplayer loop
    // reset (demuxer drains m_video_frames while the game thread polls
    // via GetVideoData on the other side of the same queue).
    size_t Size() {
        std::lock_guard guard(m_mutex);
        return m_queue.size();
    }

    void Push(T&& value) {
        std::lock_guard guard(m_mutex);
        m_queue.emplace(std::forward<T>(value));
    }

    // WARNING(GR2FORK v4): Front() returns a raw reference to internal
    // storage. The reference is only valid while the caller guarantees
    // that no other thread will Pop/Push/Clear this queue. In multi-
    // producer/multi-consumer scenarios use TryPeek() instead — it
    // copies out whatever the caller needs while the mutex is held.
    T& Front() {
        return m_queue.front();
    }

    // FIX(GR2FORK v4): atomic peek. Invokes `acc(front_ref)` while
    // holding the queue mutex, letting the caller copy out whatever
    // fields it needs without exposing a dangling reference. Returns
    // false if the queue was empty (acc not invoked).
    template <class Acc>
    bool TryPeek(Acc acc) {
        std::lock_guard guard(m_mutex);
        if (m_queue.empty()) {
            return false;
        }
        acc(m_queue.front());
        return true;
    }

    // FIX(GR2FORK v4): atomic pop. Size check + front + pop all under
    // the mutex so concurrent consumers can't both enter the pop path
    // and have the loser dereference/pop an empty queue. Return
    // nullopt for "queue was empty" — callers must check has_value().
    std::optional<T> Pop() {
        std::lock_guard guard(m_mutex);
        if (m_queue.empty()) {
            return std::nullopt;
        }
        auto result = std::move(m_queue.front());
        m_queue.pop();
        return result;
    }

    void Clear() {
        std::lock_guard guard(m_mutex);
        m_queue = {};
    }

private:
    std::mutex m_mutex{};
    std::queue<T> m_queue{};
};

AvPlayerSourceType GetSourceType(std::string_view path);

} // namespace Libraries::AvPlayer
