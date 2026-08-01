// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Minimal replacements for the guest-thread machinery the kernel objects under test link against.
// The handle-table tests drive the non-blocking half of the semaphore API, which never reaches a
// guest pthread, so these only have to satisfy the linker.

#include <chrono>

#include <boost/asio/io_context.hpp>

#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/time.h"

namespace Libraries::Kernel {

// The event queues schedule timer callbacks on this context. The handle-table tests never arm a
// timer event, so nothing has to run it.
boost::asio::io_context io_context;

void KernelSignalRequest() {}

thread_local Pthread* g_curthread = nullptr;

s32* PS4_SYSV_ABI __Error() {
    static thread_local s32 error_value = 0;
    return &error_value;
}

// Cancellation points are no-ops: the tests run on host threads, which are never cancelled.
void PthreadTestCancel() {}
void PthreadCancelInterrupt() noexcept {}

s32 ErrnoToSceKernelError(s32 e) {
    return e;
}

s32 PS4_SYSV_ABI posix_clock_gettime(u32 clock_id, OrbisKernelTimespec* ts) {
    if (ts == nullptr) {
        return -1;
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    ts->tv_sec = ns / 1'000'000'000;
    ts->tv_nsec = ns % 1'000'000'000;
    return 0;
}

} // namespace Libraries::Kernel
