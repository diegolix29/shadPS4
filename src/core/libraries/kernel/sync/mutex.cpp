// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "mutex.h"

#include "common/assert.h"

namespace Libraries::Kernel {

TimedMutex::TimedMutex() {
#ifdef _WIN64
    mtx = CreateMutex(nullptr, false, nullptr);
    ASSERT(mtx);
#else
    // Use adaptive mutex: the kernel does a brief spin before falling back to futex.
    // This is significantly faster than std::timed_mutex for short critical sections
    // and avoids the prctl syscall overhead seen in profiling.
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP);
    int ret = pthread_mutex_init(&mtx, &attr);
    ASSERT(ret == 0);
    pthread_mutexattr_destroy(&attr);
#endif
}

TimedMutex::~TimedMutex() {
#ifdef _WIN64
    CloseHandle(mtx);
#else
    pthread_mutex_destroy(&mtx);
#endif
}

void TimedMutex::lock() {
#ifdef _WIN64
    for (;;) {
        u64 res = WaitForSingleObjectEx(mtx, INFINITE, true);
        if (res == WAIT_OBJECT_0) {
            return;
        }
    }
#else
    pthread_mutex_lock(&mtx);
#endif
}

bool TimedMutex::try_lock() {
#ifdef _WIN64
    return WaitForSingleObjectEx(mtx, 0, true) == WAIT_OBJECT_0;
#else
    return pthread_mutex_trylock(&mtx) == 0;
#endif
}

void TimedMutex::unlock() {
#ifdef _WIN64
    ReleaseMutex(mtx);
#else
    pthread_mutex_unlock(&mtx);
#endif
}

} // namespace Libraries::Kernel
