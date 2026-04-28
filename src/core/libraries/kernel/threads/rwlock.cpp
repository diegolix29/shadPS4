// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <unordered_map>

#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/libs.h"
#include "common/sync_trace.h"

namespace Libraries::Kernel {

// FIX(GR2FORK): per-thread reader-recursion counts.
// MSVC's std::shared_timed_mutex (the backing store of PthreadRwlock.lock) is
// NOT recursive. If a thread already holds a read lock and a writer becomes
// queued, a recursive Rdlock() call blocks forever — because shared_timed_mutex
// is write-preferring, it won't admit new readers while a writer is waiting.
// POSIX pthread_rwlock allows recursive read-locking from the same thread, and
// GR2 (Havok scheduler) relies on this. We intercept recursive Rdlock here and
// bump a thread-local count instead of calling into shared_timed_mutex, then
// symmetrically release on Unlock. Fully transparent to the rwlock itself —
// the underlying std::shared_timed_mutex only sees one shared-lock hold per
// thread regardless of how deep the game recurses.
static std::unordered_map<PthreadRwlock*, int>& TlsReadCounts() {
    thread_local std::unordered_map<PthreadRwlock*, int> counts;
    return counts;
}

static std::mutex RwlockStaticLock;

#define THR_RWLOCK_INITIALIZER ((PthreadRwlock*)NULL)
#define THR_RWLOCK_DESTROYED ((PthreadRwlock*)1)

#define CHECK_AND_INIT_RWLOCK                                                                      \
    if (prwlock = (*rwlock); prwlock <= THR_RWLOCK_DESTROYED) [[unlikely]] {                       \
        if (prwlock == THR_RWLOCK_INITIALIZER) {                                                   \
            int ret;                                                                               \
            ret = InitStatic(g_curthread, rwlock);                                                 \
            if (ret)                                                                               \
                return (ret);                                                                      \
        } else if (prwlock == THR_RWLOCK_DESTROYED) {                                              \
            return POSIX_EINVAL;                                                                   \
        }                                                                                          \
        prwlock = *rwlock;                                                                         \
    }

static int RwlockInit(PthreadRwlockT* rwlock, const PthreadRwlockAttrT* attr) {
    auto* prwlock = new (std::nothrow) PthreadRwlock{};
    if (prwlock == nullptr) {
        return POSIX_ENOMEM;
    }
    *rwlock = prwlock;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_rwlock_destroy(PthreadRwlockT* rwlock) {
    PthreadRwlockT prwlock = *rwlock;
    if (prwlock == THR_RWLOCK_INITIALIZER) {
        return 0;
    }
    if (prwlock == THR_RWLOCK_DESTROYED) {
        return POSIX_EINVAL;
    }
    *rwlock = THR_RWLOCK_DESTROYED;
    delete prwlock;
    return 0;
}

static int InitStatic(Pthread* thread, PthreadRwlockT* rwlock) {
    std::scoped_lock lk{RwlockStaticLock};
    if (*rwlock == THR_RWLOCK_INITIALIZER) {
        return RwlockInit(rwlock, nullptr);
    }
    return 0;
}

int PS4_SYSV_ABI posix_pthread_rwlock_init(PthreadRwlockT* rwlock, const PthreadRwlockAttrT* attr) {
    *rwlock = nullptr;
    return RwlockInit(rwlock, attr);
}

int PthreadRwlock::Rdlock(const OrbisKernelTimespec* abstime) {
    Pthread* curthread = g_curthread;

    // FIX(GR2FORK): recursive read-lock shortcut. If this thread already
    // holds a read lock on `this`, just bump the TLS count — don't go through
    // std::shared_timed_mutex (which would deadlock when a writer is queued).
    auto& tls_counts = TlsReadCounts();
    auto it = tls_counts.find(this);
    if (it != tls_counts.end() && it->second > 0) {
        it->second++;
        curthread->rdlock_count++;
        return 0;
    }

    /*
     * POSIX said the validity of the abstimeout parameter need
     * not be checked if the lock can be immediately acquired.
     */
    if (lock.try_lock_shared()) {
        curthread->rdlock_count++;
        tls_counts[this] = 1;
        Common::SyncTrace::RwRdlockAcquired(this);
        return 0;
    }
    if (abstime && (abstime->tv_nsec >= 1000000000 || abstime->tv_nsec < 0)) [[unlikely]] {
        return POSIX_EINVAL;
    }

    Common::SyncTrace::RwWaitBegin(this, /*want_write=*/false);

    // Note: On interruption an attempt to relock the mutex is made.
    if (abstime != nullptr) {
        if (!lock.try_lock_shared_until(abstime->TimePoint())) {
            Common::SyncTrace::RwWaitEnd(this);
            return POSIX_ETIMEDOUT;
        }
    } else {
        lock.lock_shared();
    }

    Common::SyncTrace::RwWaitEnd(this);
    Common::SyncTrace::RwRdlockAcquired(this);
    tls_counts[this] = 1;
    curthread->rdlock_count++;
    return 0;
}

int PthreadRwlock::Wrlock(const OrbisKernelTimespec* abstime) {
    Pthread* curthread = g_curthread;

    /*
     * POSIX said the validity of the abstimeout parameter need
     * not be checked if the lock can be immediately acquired.
     */
    if (lock.try_lock()) {
        owner = curthread;
        // FIX(GR2FORK): track write-lock acquisition.
        Common::SyncTrace::RwWrlockAcquired(this);
        return 0;
    }

    if (abstime && (abstime->tv_nsec >= 1000000000 || abstime->tv_nsec < 0)) {
        return POSIX_EINVAL;
    }

    // FIX(GR2FORK): track that we're blocking on the writer slot.
    Common::SyncTrace::RwWaitBegin(this, /*want_write=*/true);

    // Note: On interruption an attempt to relock the mutex is made.
    if (abstime != nullptr) {
        if (!lock.try_lock_until(abstime->TimePoint())) {
            Common::SyncTrace::RwWaitEnd(this);
            return POSIX_ETIMEDOUT;
        }
    } else {
        lock.lock();
    }

    Common::SyncTrace::RwWaitEnd(this);
    Common::SyncTrace::RwWrlockAcquired(this);
    owner = curthread;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_rwlock_rdlock(PthreadRwlockT* rwlock) {
    PthreadRwlockT prwlock{};
    CHECK_AND_INIT_RWLOCK
    return prwlock->Rdlock(nullptr);
}

int PS4_SYSV_ABI posix_pthread_rwlock_timedrdlock(PthreadRwlockT* rwlock,
                                                  const OrbisKernelTimespec* abstime) {
    PthreadRwlockT prwlock{};
    CHECK_AND_INIT_RWLOCK
    return prwlock->Rdlock(abstime);
}

int PS4_SYSV_ABI posix_pthread_rwlock_tryrdlock(PthreadRwlockT* rwlock) {
    Pthread* curthread = g_curthread;
    PthreadRwlockT prwlock{};
    CHECK_AND_INIT_RWLOCK

    // FIX(GR2FORK): recursive read-lock shortcut (same reason as Rdlock).
    auto& tls_counts = TlsReadCounts();
    auto tls_it = tls_counts.find(prwlock);
    if (tls_it != tls_counts.end() && tls_it->second > 0) {
        tls_it->second++;
        curthread->rdlock_count++;
        return 0;
    }

    if (!prwlock->lock.try_lock_shared()) {
        return POSIX_EBUSY;
    }

    Common::SyncTrace::RwRdlockAcquired(prwlock);
    tls_counts[prwlock] = 1;
    curthread->rdlock_count++;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_rwlock_trywrlock(PthreadRwlockT* rwlock) {
    Pthread* curthread = g_curthread;
    PthreadRwlockT prwlock{};
    CHECK_AND_INIT_RWLOCK

    if (!prwlock->lock.try_lock()) {
        return POSIX_EBUSY;
    }
    prwlock->owner = curthread;
    // FIX(GR2FORK): track successful trywrlock acquisition.
    Common::SyncTrace::RwWrlockAcquired(prwlock);
    return 0;
}

int PS4_SYSV_ABI posix_pthread_rwlock_wrlock(PthreadRwlockT* rwlock) {
    PthreadRwlockT prwlock{};
    CHECK_AND_INIT_RWLOCK
    return prwlock->Wrlock(nullptr);
}

int PS4_SYSV_ABI posix_pthread_rwlock_timedwrlock(PthreadRwlockT* rwlock,
                                                  const OrbisKernelTimespec* abstime) {
    PthreadRwlockT prwlock{};
    CHECK_AND_INIT_RWLOCK
    return prwlock->Wrlock(abstime);
}

int PS4_SYSV_ABI posix_pthread_rwlock_unlock(PthreadRwlockT* rwlock) {
    Pthread* curthread = g_curthread;
    PthreadRwlockT prwlock = *rwlock;
    if (prwlock <= THR_RWLOCK_DESTROYED) [[unlikely]] {
        return POSIX_EINVAL;
    }

    if (prwlock->owner == curthread) {
        prwlock->owner = nullptr;
        Common::SyncTrace::RwWrlockReleased(prwlock);
        prwlock->lock.unlock();
    } else {
        if (prwlock->owner == nullptr) {
            curthread->rdlock_count--;
        }
        // FIX(GR2FORK): recursive read-lock counting. Only call unlock_shared
        // on the OUTERMOST release; inner releases just decrement the TLS
        // counter.
        auto& tls_counts = TlsReadCounts();
        auto tls_it = tls_counts.find(prwlock);
        if (tls_it != tls_counts.end()) {
            if (tls_it->second > 1) {
                tls_it->second--;
                return 0; // inner release; don't touch the mutex
            }
            tls_counts.erase(tls_it);
        }
        Common::SyncTrace::RwRdlockReleased(prwlock);
        prwlock->lock.unlock_shared();
    }

    return 0;
}

int PS4_SYSV_ABI posix_pthread_rwlockattr_destroy(PthreadRwlockAttrT* rwlockattr) {
    if (rwlockattr == nullptr) {
        return POSIX_EINVAL;
    }
    PthreadRwlockAttrT prwlockattr = *rwlockattr;
    if (prwlockattr == nullptr) {
        return POSIX_EINVAL;
    }

    delete prwlockattr;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_rwlockattr_getpshared(const PthreadRwlockAttrT* rwlockattr,
                                                     int* pshared) {
    *pshared = (*rwlockattr)->pshared;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_rwlockattr_init(PthreadRwlockAttrT* rwlockattr) {
    if (rwlockattr == nullptr) {
        return POSIX_EINVAL;
    }

    auto prwlockattr = new (std::nothrow) PthreadRwlockAttr{};
    if (prwlockattr == nullptr) {
        return POSIX_ENOMEM;
    }

    prwlockattr->pshared = 0;
    *rwlockattr = prwlockattr;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_rwlockattr_setpshared(PthreadRwlockAttrT* rwlockattr, int pshared) {
    /* Only PTHREAD_PROCESS_PRIVATE is supported. */
    if (pshared != 0) {
        return POSIX_EINVAL;
    }

    (*rwlockattr)->pshared = pshared;
    return 0;
}

void RegisterRwlock(Core::Loader::SymbolsResolver* sym) {
    // Posix-Kernel
    LIB_FUNCTION("1471ajPzxh0", "libkernel", 1, "libkernel", posix_pthread_rwlock_destroy);
    LIB_FUNCTION("ytQULN-nhL4", "libkernel", 1, "libkernel", posix_pthread_rwlock_init);
    LIB_FUNCTION("iGjsr1WAtI0", "libkernel", 1, "libkernel", posix_pthread_rwlock_rdlock);
    LIB_FUNCTION("lb8lnYo-o7k", "libkernel", 1, "libkernel", posix_pthread_rwlock_timedrdlock);
    LIB_FUNCTION("9zklzAl9CGM", "libkernel", 1, "libkernel", posix_pthread_rwlock_timedwrlock);
    LIB_FUNCTION("SFxTMOfuCkE", "libkernel", 1, "libkernel", posix_pthread_rwlock_tryrdlock);
    LIB_FUNCTION("XhWHn6P5R7U", "libkernel", 1, "libkernel", posix_pthread_rwlock_trywrlock);
    LIB_FUNCTION("EgmLo6EWgso", "libkernel", 1, "libkernel", posix_pthread_rwlock_unlock);
    LIB_FUNCTION("sIlRvQqsN2Y", "libkernel", 1, "libkernel", posix_pthread_rwlock_wrlock);
    LIB_FUNCTION("qsdmgXjqSgk", "libkernel", 1, "libkernel", posix_pthread_rwlockattr_destroy);
    LIB_FUNCTION("VqEMuCv-qHY", "libkernel", 1, "libkernel", posix_pthread_rwlockattr_getpshared);
    LIB_FUNCTION("xFebsA4YsFI", "libkernel", 1, "libkernel", posix_pthread_rwlockattr_init);
    LIB_FUNCTION("OuKg+kRDD7U", "libkernel", 1, "libkernel", posix_pthread_rwlockattr_setpshared);

    // Posix
    LIB_FUNCTION("1471ajPzxh0", "libScePosix", 1, "libkernel", posix_pthread_rwlock_destroy);
    LIB_FUNCTION("ytQULN-nhL4", "libScePosix", 1, "libkernel", posix_pthread_rwlock_init);
    LIB_FUNCTION("iGjsr1WAtI0", "libScePosix", 1, "libkernel", posix_pthread_rwlock_rdlock);
    LIB_FUNCTION("lb8lnYo-o7k", "libScePosix", 1, "libkernel", posix_pthread_rwlock_timedrdlock);
    LIB_FUNCTION("9zklzAl9CGM", "libScePosix", 1, "libkernel", posix_pthread_rwlock_timedwrlock);
    LIB_FUNCTION("SFxTMOfuCkE", "libScePosix", 1, "libkernel", posix_pthread_rwlock_tryrdlock);
    LIB_FUNCTION("XhWHn6P5R7U", "libScePosix", 1, "libkernel", posix_pthread_rwlock_trywrlock);
    LIB_FUNCTION("EgmLo6EWgso", "libScePosix", 1, "libkernel", posix_pthread_rwlock_unlock);
    LIB_FUNCTION("sIlRvQqsN2Y", "libScePosix", 1, "libkernel", posix_pthread_rwlock_wrlock);
    LIB_FUNCTION("qsdmgXjqSgk", "libScePosix", 1, "libkernel", posix_pthread_rwlockattr_destroy);
    LIB_FUNCTION("VqEMuCv-qHY", "libScePosix", 1, "libkernel", posix_pthread_rwlockattr_getpshared);
    LIB_FUNCTION("xFebsA4YsFI", "libScePosix", 1, "libkernel", posix_pthread_rwlockattr_init);
    LIB_FUNCTION("OuKg+kRDD7U", "libScePosix", 1, "libkernel", posix_pthread_rwlockattr_setpshared);

    // Orbis
    LIB_FUNCTION("i2ifZ3fS2fo", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_rwlockattr_destroy));
    LIB_FUNCTION("LcOZBHGqbFk", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_rwlockattr_getpshared));
    LIB_FUNCTION("yOfGg-I1ZII", "libkernel", 1, "libkernel", ORBIS(posix_pthread_rwlockattr_init));
    LIB_FUNCTION("-ZvQH18j10c", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_rwlockattr_setpshared));
    LIB_FUNCTION("BB+kb08Tl9A", "libkernel", 1, "libkernel", ORBIS(posix_pthread_rwlock_destroy));
    LIB_FUNCTION("6ULAa0fq4jA", "libkernel", 1, "libkernel", ORBIS(posix_pthread_rwlock_init));
    LIB_FUNCTION("Ox9i0c7L5w0", "libkernel", 1, "libkernel", ORBIS(posix_pthread_rwlock_rdlock));
    LIB_FUNCTION("iPtZRWICjrM", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_rwlock_timedrdlock));
    LIB_FUNCTION("adh--6nIqTk", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_rwlock_timedwrlock));
    LIB_FUNCTION("XD3mDeybCnk", "libkernel", 1, "libkernel", ORBIS(posix_pthread_rwlock_tryrdlock));
    LIB_FUNCTION("bIHoZCTomsI", "libkernel", 1, "libkernel", ORBIS(posix_pthread_rwlock_trywrlock));
    LIB_FUNCTION("+L98PIbGttk", "libkernel", 1, "libkernel", ORBIS(posix_pthread_rwlock_unlock));
    LIB_FUNCTION("mqdNorrB+gI", "libkernel", 1, "libkernel", ORBIS(posix_pthread_rwlock_wrlock));
}

} // namespace Libraries::Kernel
