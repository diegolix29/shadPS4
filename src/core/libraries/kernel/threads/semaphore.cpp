// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <condition_variable>
#include <list>
#include <mutex>
#include <semaphore>

#include "core/libraries/kernel/sync/semaphore.h"

#include "common/logging/log.h"
#include "common/slot_vector.h"
#include "common/sync_trace.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"

namespace Libraries::Kernel {

// FIX(GR2FORK): see event_flag.cpp for rationale.
static constexpr auto kTraceSemaWaitThresholdMs = std::chrono::milliseconds(500);

constexpr s32 ORBIS_KERNEL_SEM_VALUE_MAX = 0x7FFFFFFF;

struct PthreadSem {
    explicit PthreadSem(s32 value_) : semaphore{value_}, value{value_} {}

    CountingSemaphore semaphore;
    std::atomic<s32> value;
};

class OrbisSem {
public:
    OrbisSem(s32 init_count, s32 max_count, std::string_view name, bool is_fifo)
        : name{name}, token_count{init_count}, max_count{max_count}, init_count{init_count},
          is_fifo{is_fifo} {}
    ~OrbisSem() = default;

    s32 Wait(bool can_block, s32 need_count, u32* timeout) {
        const auto trace_start = std::chrono::steady_clock::now();
        Common::SyncTrace::NoteWaitBegin(
            Common::SyncTrace::Op::WAIT_SEMA_SLOW, this, name,
            static_cast<u64>(need_count));
        struct WaitEndGuard {
            ~WaitEndGuard() { Common::SyncTrace::NoteWaitEnd(); }
        } _end_guard;

        std::unique_lock lk{mutex};
        if (token_count >= need_count) {
            token_count -= need_count;
            return ORBIS_OK;
        }
        if (!can_block) {
            return ORBIS_KERNEL_ERROR_EBUSY;
        }

        if (timeout && *timeout == 0) {
            return ORBIS_KERNEL_ERROR_ETIMEDOUT;
        }

        // Create waiting thread object and add it into the list of waiters.
        WaitingThread waiter{need_count, is_fifo};
        const auto it = AddWaiter(&waiter);

        // Perform the wait.
        const s32 result = waiter.Wait(lk, timeout);
        if (result == ORBIS_KERNEL_ERROR_ETIMEDOUT) {
            wait_list.erase(it);
        }

        const auto d = std::chrono::steady_clock::now() - trace_start;
        if (d >= kTraceSemaWaitThresholdMs || result != ORBIS_OK) {
            Common::SyncTrace::Record(
                Common::SyncTrace::Op::WAIT_SEMA_SLOW, this, name,
                static_cast<u64>(need_count),
                std::chrono::duration_cast<std::chrono::milliseconds>(d).count(),
                static_cast<u32>(wait_list.size()), result);
        }
        return result;
    }

    bool Signal(s32 signal_count) {
        std::scoped_lock lk{mutex};
        const s32 before = token_count.load();
        const u32 waiters_before = static_cast<u32>(wait_list.size());

        if (token_count + signal_count > max_count) {
            Common::SyncTrace::Record(
                Common::SyncTrace::Op::SIGNAL_SEMA, this, name,
                static_cast<u64>(signal_count),
                static_cast<u64>(before),
                waiters_before,
                /*result=*/-1 /* over_max */);
            return false;
        }
        token_count += signal_count;

        int woken = 0;
        // Wake up threads in order of priority.
        for (auto it = wait_list.begin(); it != wait_list.end();) {
            auto* waiter = *it;
            if (waiter->need_count > token_count) {
                ++it;
                continue;
            }
            it = wait_list.erase(it);
            token_count -= waiter->need_count;
            waiter->was_signaled = true;
            waiter->sem.release();
            ++woken;
        }

        Common::SyncTrace::Record(
            Common::SyncTrace::Op::SIGNAL_SEMA, this, name,
            static_cast<u64>(signal_count),
            static_cast<u64>(token_count.load()),
            waiters_before, woken);
        return true;
    }

    s32 Cancel(s32 set_count, s32* num_waiters) {
        std::scoped_lock lk{mutex};
        const u32 waiters = static_cast<u32>(wait_list.size());
        Common::SyncTrace::Record(
            Common::SyncTrace::Op::CANCEL_SEMA, this, name,
            static_cast<u64>(set_count), 0, waiters, 0);
        if (num_waiters) {
            *num_waiters = static_cast<s32>(waiters);
        }
        for (auto* waiter : wait_list) {
            waiter->was_canceled = true;
            waiter->sem.release();
        }
        wait_list.clear();
        token_count = set_count < 0 ? init_count : set_count;
        return ORBIS_OK;
    }

    void Delete() {
        std::scoped_lock lk{mutex};
        Common::SyncTrace::Record(
            Common::SyncTrace::Op::DELETE_SEMA, this, name,
            0, 0, static_cast<u32>(wait_list.size()), 0);
        for (auto* waiter : wait_list) {
            waiter->was_deleted = true;
            waiter->sem.release();
        }
        wait_list.clear();
    }

public:
    struct WaitingThread {
        BinarySemaphore sem;
        u32 priority;
        s32 need_count;
        std::string thr_name;
        bool was_signaled{};
        bool was_deleted{};
        bool was_canceled{};

        explicit WaitingThread(s32 need_count, bool is_fifo)
            : sem{0}, priority{0}, need_count{need_count} {
            // Retrieve calling thread priority for sorting into waiting threads list.
            if (!is_fifo) {
                priority = g_curthread->attr.prio;
            }

            thr_name = g_curthread->name;
        }

        [[nodiscard]] s32 GetResult() const {
            if (was_signaled) {
                return ORBIS_OK;
            }
            if (was_deleted) {
                return ORBIS_KERNEL_ERROR_EACCES;
            }
            if (was_canceled) {
                return ORBIS_KERNEL_ERROR_ECANCELED;
            }
            return ORBIS_KERNEL_ERROR_ETIMEDOUT;
        }

        s32 Wait(std::unique_lock<std::mutex>& lk, u32* timeout) {
            lk.unlock();
            if (!timeout) {
                // Wait indefinitely until we are woken up.
                sem.acquire();
                lk.lock();
            } else {
                // Wait until timeout runs out, recording how much remaining time there was.
                const auto start = std::chrono::high_resolution_clock::now();
                sem.try_acquire_for(std::chrono::microseconds(*timeout));
                const auto end = std::chrono::high_resolution_clock::now();
                const auto time =
                    std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
                lk.lock();
                if (was_signaled) {
                    *timeout -= time;
                } else {
                    *timeout = 0;
                }
            }
            return GetResult();
        }
    };

    using WaitList = std::list<WaitingThread*>;

    WaitList::iterator AddWaiter(WaitingThread* waiter) {
        // Insert at the end of the list for FIFO order.
        if (is_fifo) {
            wait_list.push_back(waiter);
            return --wait_list.end();
        }
        // Find the first with lower priority (greater number) than us and insert right before it.
        auto it = wait_list.begin();
        while (it != wait_list.end() && (*it)->priority <= waiter->priority) {
            ++it;
        }
        return wait_list.insert(it, waiter);
    }

    WaitList wait_list;
    std::string name;
    std::atomic<s32> token_count;
    std::mutex mutex;
    s32 max_count;
    s32 init_count;
    bool is_fifo;
};

using OrbisKernelSema = Common::SlotId;

static Common::SlotVector<std::unique_ptr<OrbisSem>> orbis_sems;

s32 PS4_SYSV_ABI sceKernelCreateSema(OrbisKernelSema* sem, const char* pName, u32 attr,
                                     s32 initCount, s32 maxCount, const void* pOptParam) {
    if (!pName || attr > 2 || initCount < 0 || maxCount <= 0 || initCount > maxCount) {
        LOG_ERROR(Lib_Kernel, "Semaphore creation parameters are invalid!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    *sem = orbis_sems.insert(
        std::move(std::make_unique<OrbisSem>(initCount, maxCount, pName, attr == 1)));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelWaitSema(OrbisKernelSema sem, s32 needCount, u32* pTimeout) {
    if (!orbis_sems.is_allocated(sem)) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    return orbis_sems[sem]->Wait(true, needCount, pTimeout);
}

s32 PS4_SYSV_ABI sceKernelSignalSema(OrbisKernelSema sem, s32 signalCount) {
    if (!orbis_sems.is_allocated(sem)) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    if (!orbis_sems[sem]->Signal(signalCount)) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelPollSema(OrbisKernelSema sem, s32 needCount) {
    if (!orbis_sems.is_allocated(sem)) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    return orbis_sems[sem]->Wait(false, needCount, nullptr);
}

s32 PS4_SYSV_ABI sceKernelCancelSema(OrbisKernelSema sem, s32 setCount, s32* pNumWaitThreads) {
    if (!orbis_sems.is_allocated(sem)) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    return orbis_sems[sem]->Cancel(setCount, pNumWaitThreads);
}

s32 PS4_SYSV_ABI sceKernelDeleteSema(OrbisKernelSema sem) {
    if (!orbis_sems.is_allocated(sem)) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    orbis_sems[sem]->Delete();
    orbis_sems.erase(sem);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI posix_sem_init(PthreadSem** sem, s32 pshared, u32 value) {
    if (value > ORBIS_KERNEL_SEM_VALUE_MAX) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    if (sem != nullptr) {
        *sem = new PthreadSem(static_cast<s32>(value));
    }
    return 0;
}

s32 PS4_SYSV_ABI posix_sem_destroy(PthreadSem** sem) {
    if (sem == nullptr || *sem == nullptr) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    delete *sem;
    *sem = nullptr;
    return 0;
}

s32 PS4_SYSV_ABI posix_sem_wait(PthreadSem** sem) {
    if (sem == nullptr || *sem == nullptr) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    (*sem)->semaphore.acquire();
    --(*sem)->value;
    return 0;
}

s32 PS4_SYSV_ABI posix_sem_trywait(PthreadSem** sem) {
    if (sem == nullptr || *sem == nullptr) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    if (!(*sem)->semaphore.try_acquire()) {
        *__Error() = POSIX_EAGAIN;
        return -1;
    }
    --(*sem)->value;
    return 0;
}

s32 PS4_SYSV_ABI posix_sem_timedwait(PthreadSem** sem, const OrbisKernelTimespec* t) {
    if (sem == nullptr || *sem == nullptr) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    if (!(*sem)->semaphore.try_acquire_until(t->TimePoint())) {
        *__Error() = POSIX_ETIMEDOUT;
        return -1;
    }
    --(*sem)->value;
    return 0;
}

s32 PS4_SYSV_ABI posix_sem_post(PthreadSem** sem) {
    if (sem == nullptr || *sem == nullptr) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    if ((*sem)->value == ORBIS_KERNEL_SEM_VALUE_MAX) {
        *__Error() = POSIX_EOVERFLOW;
        return -1;
    }
    ++(*sem)->value;
    (*sem)->semaphore.release();
    return 0;
}

s32 PS4_SYSV_ABI posix_sem_getvalue(PthreadSem** sem, s32* sval) {
    if (sem == nullptr || *sem == nullptr) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    if (sval) {
        *sval = (*sem)->value;
    }
    return 0;
}

s32 PS4_SYSV_ABI scePthreadSemInit(PthreadSem** sem, s32 flag, u32 value, const char* name) {
    if (flag != 0) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    s32 ret = posix_sem_init(sem, 0, value);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePthreadSemDestroy(PthreadSem** sem) {
    s32 ret = posix_sem_destroy(sem);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePthreadSemWait(PthreadSem** sem) {
    s32 ret = posix_sem_wait(sem);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePthreadSemTrywait(PthreadSem** sem) {
    s32 ret = posix_sem_trywait(sem);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePthreadSemTimedwait(PthreadSem** sem, u32 usec) {
    OrbisKernelTimespec time{};
    time.tv_sec = usec / 1000000;
    time.tv_nsec = (usec % 1000000) * 1000;

    s32 ret = posix_sem_timedwait(sem, &time);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePthreadSemPost(PthreadSem** sem) {
    s32 ret = posix_sem_post(sem);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePthreadSemGetvalue(PthreadSem** sem, s32* sval) {
    s32 ret = posix_sem_getvalue(sem, sval);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

void RegisterSemaphore(Core::Loader::SymbolsResolver* sym) {
    // Orbis
    LIB_FUNCTION("188x57JYp0g", "libkernel", 1, "libkernel", sceKernelCreateSema);
    LIB_FUNCTION("Zxa0VhQVTsk", "libkernel", 1, "libkernel", sceKernelWaitSema);
    LIB_FUNCTION("4czppHBiriw", "libkernel", 1, "libkernel", sceKernelSignalSema);
    LIB_FUNCTION("12wOHk8ywb0", "libkernel", 1, "libkernel", sceKernelPollSema);
    LIB_FUNCTION("4DM06U2BNEY", "libkernel", 1, "libkernel", sceKernelCancelSema);
    LIB_FUNCTION("R1Jvn8bSCW8", "libkernel", 1, "libkernel", sceKernelDeleteSema);

    // Posix
    LIB_FUNCTION("pDuPEf3m4fI", "libScePosix", 1, "libkernel", posix_sem_init);
    LIB_FUNCTION("cDW233RAwWo", "libScePosix", 1, "libkernel", posix_sem_destroy);
    LIB_FUNCTION("YCV5dGGBcCo", "libScePosix", 1, "libkernel", posix_sem_wait);
    LIB_FUNCTION("WBWzsRifCEA", "libScePosix", 1, "libkernel", posix_sem_trywait);
    LIB_FUNCTION("w5IHyvahg-o", "libScePosix", 1, "libkernel", posix_sem_timedwait);
    LIB_FUNCTION("IKP8typ0QUk", "libScePosix", 1, "libkernel", posix_sem_post);
    LIB_FUNCTION("Bq+LRV-N6Hk", "libScePosix", 1, "libkernel", posix_sem_getvalue);

    LIB_FUNCTION("GEnUkDZoUwY", "libkernel", 1, "libkernel", scePthreadSemInit);
    LIB_FUNCTION("Vwc+L05e6oE", "libkernel", 1, "libkernel", scePthreadSemDestroy);
    LIB_FUNCTION("C36iRE0F5sE", "libkernel", 1, "libkernel", scePthreadSemWait);
    LIB_FUNCTION("H2a+IN9TP0E", "libkernel", 1, "libkernel", scePthreadSemTrywait);
    LIB_FUNCTION("fjN6NQHhK8k", "libkernel", 1, "libkernel", scePthreadSemTimedwait);
    LIB_FUNCTION("aishVAiFaYM", "libkernel", 1, "libkernel", scePthreadSemPost);
    LIB_FUNCTION("DjpBvGlaWbQ", "libkernel", 1, "libkernel", scePthreadSemGetvalue);
}

} // namespace Libraries::Kernel
