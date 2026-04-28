// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <bit>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/sync_trace.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"

namespace Libraries::Kernel {

// FIX(GR2FORK): wait threshold for SLOW log-to-ring. Waits faster than this
// are ignored entirely; slower ones go in the ring. 500ms is well below the
// watchdog's 15s critical threshold so we always catch near-hangs.
static constexpr auto kTraceWaitThresholdMs = std::chrono::milliseconds(500);

// FIX(GR2FORK): EventFlag race-guard window. Protects against the Havok
// scheduler bug where one thread does:
//     Worker:       Set(bit)
//     Coordinator:  Clear(0)  ; wipes the bit
//     Coordinator:  Wait(bit) ; never wakes — signal lost
// We track per-bit set timestamps. On Wait entry, if the wait wants a bit
// that was Set within this window but is no longer present in m_bits, we
// re-inject it. A wait that successfully consumes a bit (via ClearMode)
// clears the timestamp so legitimate "wait for NEXT signal" sequences are
// unaffected.
#ifndef GR2FORK_EF_RACE_WINDOW_MS
#define GR2FORK_EF_RACE_WINDOW_MS 20
#endif
static constexpr auto kEfRaceGuardWindow =
    std::chrono::milliseconds(GR2FORK_EF_RACE_WINDOW_MS);


constexpr int ORBIS_KERNEL_EVF_ATTR_TH_FIFO = 0x01;
constexpr int ORBIS_KERNEL_EVF_ATTR_TH_PRIO = 0x02;
constexpr int ORBIS_KERNEL_EVF_ATTR_SINGLE = 0x10;
constexpr int ORBIS_KERNEL_EVF_ATTR_MULTI = 0x20;

constexpr int ORBIS_KERNEL_EVF_WAITMODE_AND = 0x01;
constexpr int ORBIS_KERNEL_EVF_WAITMODE_OR = 0x02;
constexpr int ORBIS_KERNEL_EVF_WAITMODE_CLEAR_ALL = 0x10;
constexpr int ORBIS_KERNEL_EVF_WAITMODE_CLEAR_PAT = 0x20;

class EventFlagInternal {
public:
    enum class ClearMode { None, All, Bits };
    enum class WaitMode { And, Or };
    enum class ThreadMode { Single, Multi };
    enum class QueueMode { Fifo, ThreadPrio };

    EventFlagInternal(const std::string& name, ThreadMode thread_mode, QueueMode queue_mode,
                      uint64_t bits)
        : m_name(name), m_thread_mode(thread_mode), m_queue_mode(queue_mode), m_bits(bits) {};

    int Wait(u64 bits, WaitMode wait_mode, ClearMode clear_mode, u64* result, u32* ptr_micros) {
        // FIX(GR2FORK): register in active-waiter table; erase on all exits.
        const auto trace_start = std::chrono::steady_clock::now();
        Common::SyncTrace::NoteWaitBegin(
            Common::SyncTrace::Op::WAIT_EF_SLOW, this, m_name, bits);
        struct WaitEndGuard {
            ~WaitEndGuard() { Common::SyncTrace::NoteWaitEnd(); }
        } _end_guard;

        std::unique_lock lock{m_mutex};

        // FIX(GR2FORK): race-guard. If bits this wait wants were Set within
        // kEfRaceGuardWindow but are no longer present in m_bits (wiped by
        // a racy Clear between the Set and this Wait), re-inject them.
        // This targets the Havok scheduler's
        //   Set(X) → Clear(0) → Wait(X)
        // pattern where the coordinator's reset wipes a fresh worker signal.
        (void)RaceGuardReinject(bits);

        uint32_t micros = 0;
        bool infinitely = true;
        if (ptr_micros != nullptr) {
            micros = *ptr_micros;
            infinitely = false;
        }

        if (m_thread_mode == ThreadMode::Single && m_waiting_threads > 0) {
            Common::SyncTrace::Record(
                Common::SyncTrace::Op::WAIT_EF_SLOW, this, m_name,
                bits, 0, m_waiting_threads, ORBIS_KERNEL_ERROR_EPERM);
            return ORBIS_KERNEL_ERROR_EPERM;
        }

        auto const start = std::chrono::system_clock::now();
        m_waiting_threads++;
        auto waitFunc = [this, wait_mode, bits] {
            return (m_status == Status::Canceled || m_status == Status::Deleted ||
                    (wait_mode == WaitMode::And && (m_bits & bits) == bits) ||
                    (wait_mode == WaitMode::Or && (m_bits & bits) != 0));
        };

        if (infinitely) {
            m_cond_var.wait(lock, waitFunc);
        } else {
            if (!m_cond_var.wait_for(lock, std::chrono::microseconds(micros), waitFunc)) {
                if (result != nullptr) {
                    *result = m_bits;
                }
                *ptr_micros = 0;
                --m_waiting_threads;
                const auto d = std::chrono::steady_clock::now() - trace_start;
                if (d >= kTraceWaitThresholdMs) {
                    Common::SyncTrace::Record(
                        Common::SyncTrace::Op::WAIT_EF_SLOW, this, m_name,
                        bits,
                        std::chrono::duration_cast<std::chrono::milliseconds>(d).count(),
                        m_waiting_threads, ORBIS_KERNEL_ERROR_ETIMEDOUT);
                }
                return ORBIS_KERNEL_ERROR_ETIMEDOUT;
            }
        }
        --m_waiting_threads;
        if (result != nullptr) {
            *result = m_bits;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::system_clock::now() - start)
                           .count();
        if (result != nullptr) {
            *result = m_bits;
        }

        if (ptr_micros != nullptr) {
            *ptr_micros = (elapsed >= micros ? 0 : micros - elapsed);
        }

        if (m_status == Status::Canceled) {
            Common::SyncTrace::Record(
                Common::SyncTrace::Op::WAIT_EF_SLOW, this, m_name,
                bits, 0, m_waiting_threads, ORBIS_KERNEL_ERROR_ECANCELED);
            return ORBIS_KERNEL_ERROR_ECANCELED;
        } else if (m_status == Status::Deleted) {
            Common::SyncTrace::Record(
                Common::SyncTrace::Op::WAIT_EF_SLOW, this, m_name,
                bits, 0, m_waiting_threads, ORBIS_KERNEL_ERROR_EACCES);
            return ORBIS_KERNEL_ERROR_EACCES;
        }

        if (clear_mode == ClearMode::All) {
            // FIX(GR2FORK): consumed — clear all stamps so a later race-guard
            // check doesn't incorrectly re-inject a stale signal.
            m_bits = 0;
            m_bit_set_at.fill({});
        } else if (clear_mode == ClearMode::Bits) {
            m_bits &= ~bits;
            ClearBitsStamps(bits);
        }

        const auto d = std::chrono::steady_clock::now() - trace_start;
        if (d >= kTraceWaitThresholdMs) {
            Common::SyncTrace::Record(
                Common::SyncTrace::Op::WAIT_EF_SLOW, this, m_name,
                bits,
                std::chrono::duration_cast<std::chrono::milliseconds>(d).count(),
                m_waiting_threads, ORBIS_OK);
        }
        return ORBIS_OK;
    }

    int Poll(u64 bits, WaitMode wait_mode, ClearMode clear_mode, u64* result) {
        u32 micros = 0;
        auto ret = Wait(bits, wait_mode, clear_mode, result, &micros);
        if (ret == ORBIS_KERNEL_ERROR_ETIMEDOUT) {
            // Poll returns EBUSY instead.
            ret = ORBIS_KERNEL_ERROR_EBUSY;
        }
        return ret;
    }

    void Set(u64 bits) {
        std::unique_lock lock{m_mutex};

        while (m_status != Status::Set) {
            m_mutex.unlock();
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            m_mutex.lock();
        }

        const u64 before = m_bits;
        m_bits |= bits;
        // FIX(GR2FORK): stamp bits we just Set for race-guard purposes.
        StampBitsSet(bits);
        const u32 waiters = static_cast<u32>(m_waiting_threads);
        m_cond_var.notify_all();

        // FIX(GR2FORK): ring-only record; no log I/O.
        Common::SyncTrace::Record(
            Common::SyncTrace::Op::SET_EF, this, m_name,
            /*arg1=*/bits, /*arg2=*/m_bits, waiters, /*result=*/0);
        (void)before;
    }

    void Clear(u64 bits) {
        std::unique_lock lock{m_mutex};
        while (m_status != Status::Set) {
            m_mutex.unlock();
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            m_mutex.lock();
        }

        m_bits &= bits;
        Common::SyncTrace::Record(
            Common::SyncTrace::Op::CLEAR_EF, this, m_name,
            /*arg1=*/bits, /*arg2=*/m_bits, 0, 0);
    }

    void Cancel(u64 setPattern, int* numWaitThreads) {
        std::unique_lock lock{m_mutex};

        while (m_status != Status::Set) {
            m_mutex.unlock();
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            m_mutex.lock();
        }

        if (numWaitThreads) {
            *numWaitThreads = m_waiting_threads;
        }

        m_status = Status::Canceled;
        m_bits = setPattern;

        Common::SyncTrace::Record(
            Common::SyncTrace::Op::CANCEL_EF, this, m_name,
            /*arg1=*/setPattern, /*arg2=*/0,
            static_cast<u32>(m_waiting_threads), 0);

        m_cond_var.notify_all();

        while (m_waiting_threads > 0) {
            m_mutex.unlock();
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            m_mutex.lock();
        }

        m_status = Status::Set;
    }

private:
    enum class Status { Set, Canceled, Deleted };

    std::mutex m_mutex;
    std::condition_variable m_cond_var;
    Status m_status = Status::Set;
    int m_waiting_threads = 0;
    std::string m_name;
    ThreadMode m_thread_mode = ThreadMode::Single;
    QueueMode m_queue_mode = QueueMode::Fifo;
    u64 m_bits = 0;

    // FIX(GR2FORK): per-bit "last Set time" for race-guard re-injection.
    // Default-constructed time_point has zero duration since epoch, which
    // we use as the "never set" sentinel.
    std::array<std::chrono::steady_clock::time_point, 64> m_bit_set_at{};

    // Called under m_mutex. For each bit in `bits`, stamp now.
    void StampBitsSet(u64 bits) {
        const auto now = std::chrono::steady_clock::now();
        while (bits != 0) {
            const int i = std::countr_zero(bits);
            m_bit_set_at[i] = now;
            bits &= bits - 1;
        }
    }

    // Called under m_mutex. For each bit in `bits`, mark "never set" so
    // subsequent Wait entries will not try to re-inject this bit.
    void ClearBitsStamps(u64 bits) {
        while (bits != 0) {
            const int i = std::countr_zero(bits);
            m_bit_set_at[i] = {};
            bits &= bits - 1;
        }
    }

    // Called under m_mutex at Wait entry. If the wait wants a bit that was
    // set recently but is no longer in m_bits (wiped by a racy Clear),
    // re-inject it. Returns the mask of bits we re-injected.
    u64 RaceGuardReinject(u64 wants) {
        // FIX(GR2FORK): temporary disable for diagnostic purposes. The race
        // guard fires ~12,500 times per long session and might be masking
        // (or causing) downstream issues that surface as VK_ERROR_DEVICE_LOST.
        // Disabling lets us see whether the underlying Havok timing bug
        // actually manifests, or whether the guard itself is the problem.
        // To re-enable: define GR2FORK_EF_RACE_GUARD_ENABLE at build time,
        // or just delete the next two lines.
#ifndef GR2FORK_EF_RACE_GUARD_ENABLE
        return 0;
#endif
        if (wants == 0) return 0;
        const auto now = std::chrono::steady_clock::now();
        u64 reinject = 0;
        u64 missing = wants & ~m_bits;
        while (missing != 0) {
            const int i = std::countr_zero(missing);
            const auto& when = m_bit_set_at[i];
            if (when.time_since_epoch().count() != 0 &&
                (now - when) < kEfRaceGuardWindow) {
                reinject |= (1ULL << i);
            }
            missing &= missing - 1;
        }
        if (reinject != 0) {
            LOG_WARNING(Kernel_Event,
                        "[EF race-guard] ef={} name=\"{}\" "
                        "re-injecting bits={:#x} (recently Set, then wiped)",
                        static_cast<const void*>(this), m_name, reinject);
            m_bits |= reinject;
            ClearBitsStamps(reinject);
            m_cond_var.notify_all();
        }
        return reinject;
    }
};

using OrbisKernelUseconds = u32;
using OrbisKernelEventFlag = EventFlagInternal*;

struct OrbisKernelEventFlagOptParam {
    size_t size;
};

int PS4_SYSV_ABI sceKernelCreateEventFlag(OrbisKernelEventFlag* ef, const char* pName, u32 attr,
                                          u64 initPattern,
                                          const OrbisKernelEventFlagOptParam* pOptParam) {
    LOG_TRACE(Kernel_Event, "called name = {} attr = {:#x} initPattern = {:#x}", pName, attr,
              initPattern);
    if (ef == nullptr || pName == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (pOptParam || attr > (ORBIS_KERNEL_EVF_ATTR_MULTI | ORBIS_KERNEL_EVF_ATTR_TH_PRIO)) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    if (strlen(pName) >= 32) {
        return ORBIS_KERNEL_ERROR_ENAMETOOLONG;
    }

    auto thread_mode = EventFlagInternal::ThreadMode::Single;
    auto queue_mode = EventFlagInternal::QueueMode::Fifo;
    switch (attr & 0xfu) {
    case 0x01:
        queue_mode = EventFlagInternal::QueueMode::Fifo;
        break;
    case 0x02:
        queue_mode = EventFlagInternal::QueueMode::ThreadPrio;
        break;
    case 0x00:
        break;
    default:
        UNREACHABLE();
    }

    switch (attr & 0xf0) {
    case 0x10:
        thread_mode = EventFlagInternal::ThreadMode::Single;
        break;
    case 0x20:
        thread_mode = EventFlagInternal::ThreadMode::Multi;
        break;
    case 0x00:
        break;
    default:
        UNREACHABLE();
    }

    if (queue_mode == EventFlagInternal::QueueMode::ThreadPrio) {
        LOG_ERROR(Kernel_Event, "ThreadPriority attr is not supported!");
    }

    *ef = new EventFlagInternal(std::string(pName), thread_mode, queue_mode, initPattern);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteEventFlag(OrbisKernelEventFlag ef) {
    if (ef == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }

    delete ef;
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelOpenEventFlag() {
    LOG_ERROR(Kernel_Event, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelCloseEventFlag() {
    LOG_ERROR(Kernel_Event, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelClearEventFlag(OrbisKernelEventFlag ef, u64 bitPattern) {
    LOG_DEBUG(Kernel_Event, "called");
    if (ef == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    ef->Clear(bitPattern);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelCancelEventFlag(OrbisKernelEventFlag ef, u64 setPattern,
                                          int* pNumWaitThreads) {
    LOG_DEBUG(Kernel_Event, "called");
    if (ef == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    ef->Cancel(setPattern, pNumWaitThreads);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelSetEventFlag(OrbisKernelEventFlag ef, u64 bitPattern) {
    LOG_TRACE(Kernel_Event, "called");
    if (ef == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    ef->Set(bitPattern);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelPollEventFlag(OrbisKernelEventFlag ef, u64 bitPattern, u32 waitMode,
                                        u64* pResultPat) {
    LOG_DEBUG(Kernel_Event, "called bitPattern = {:#x} waitMode = {:#x}", bitPattern, waitMode);

    if (ef == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }

    if (bitPattern == 0) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    auto wait = EventFlagInternal::WaitMode::And;
    auto clear = EventFlagInternal::ClearMode::None;
    switch (waitMode & 0xf) {
    case 0x01:
        wait = EventFlagInternal::WaitMode::And;
        break;
    case 0x02:
        wait = EventFlagInternal::WaitMode::Or;
        break;
    default:
        UNREACHABLE();
    }

    switch (waitMode & 0xf0) {
    case 0x00:
        clear = EventFlagInternal::ClearMode::None;
        break;
    case 0x10:
        clear = EventFlagInternal::ClearMode::All;
        break;
    case 0x20:
        clear = EventFlagInternal::ClearMode::Bits;
        break;
    default:
        UNREACHABLE();
    }

    auto result = ef->Poll(bitPattern, wait, clear, pResultPat);

    if (result != ORBIS_OK && result != ORBIS_KERNEL_ERROR_EBUSY) {
        LOG_DEBUG(Kernel_Event, "returned {:#x}", result);
    }

    return result;
}
int PS4_SYSV_ABI sceKernelWaitEventFlag(OrbisKernelEventFlag ef, u64 bitPattern, u32 waitMode,
                                        u64* pResultPat, OrbisKernelUseconds* pTimeout) {
    LOG_DEBUG(Kernel_Event, "called bitPattern = {:#x} waitMode = {:#x}", bitPattern, waitMode);
    if (ef == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }

    if (bitPattern == 0) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    auto wait = EventFlagInternal::WaitMode::And;
    auto clear = EventFlagInternal::ClearMode::None;
    switch (waitMode & 0xf) {
    case 0x01:
        wait = EventFlagInternal::WaitMode::And;
        break;
    case 0x02:
        wait = EventFlagInternal::WaitMode::Or;
        break;
    default:
        UNREACHABLE();
    }

    switch (waitMode & 0xf0) {
    case 0x00:
        clear = EventFlagInternal::ClearMode::None;
        break;
    case 0x10:
        clear = EventFlagInternal::ClearMode::All;
        break;
    case 0x20:
        clear = EventFlagInternal::ClearMode::Bits;
        break;
    default:
        UNREACHABLE();
    }

    const int result = ef->Wait(bitPattern, wait, clear, pResultPat, pTimeout);
    if (result != ORBIS_OK && result != ORBIS_KERNEL_ERROR_ETIMEDOUT) {
        LOG_DEBUG(Kernel_Event, "returned {:#x}", result);
    }

    return result;
}

void RegisterKernelEventFlag(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("PZku4ZrXJqg", "libkernel", 1, "libkernel", sceKernelCancelEventFlag);
    LIB_FUNCTION("7uhBFWRAS60", "libkernel", 1, "libkernel", sceKernelClearEventFlag);
    LIB_FUNCTION("s9-RaxukuzQ", "libkernel", 1, "libkernel", sceKernelCloseEventFlag);
    LIB_FUNCTION("BpFoboUJoZU", "libkernel", 1, "libkernel", sceKernelCreateEventFlag);
    LIB_FUNCTION("8mql9OcQnd4", "libkernel", 1, "libkernel", sceKernelDeleteEventFlag);
    LIB_FUNCTION("1vDaenmJtyA", "libkernel", 1, "libkernel", sceKernelOpenEventFlag);
    LIB_FUNCTION("9lvj5DjHZiA", "libkernel", 1, "libkernel", sceKernelPollEventFlag);
    LIB_FUNCTION("IOnSvHzqu6A", "libkernel", 1, "libkernel", sceKernelSetEventFlag);
    LIB_FUNCTION("JTvBflhYazQ", "libkernel", 1, "libkernel", sceKernelWaitEventFlag);
}

} // namespace Libraries::Kernel
