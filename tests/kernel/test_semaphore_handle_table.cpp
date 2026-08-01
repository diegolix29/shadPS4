// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The semaphore handle table is a process-wide container that guest titles drive concurrently from
// many threads. It used to be an unsynchronized Common::SlotVector, so an insert that reallocated
// the backing store, or a delete racing with a concurrent lookup, freed an OrbisSem out from under
// another thread. These tests reproduce that churn through the public API.
//
// They are written to fault, not to assert: with the container unguarded the race shows up as a
// use-after-free (reliably under AddressSanitizer, intermittently as a crash or a bogus return code
// without it). With the container guarded, every operation must return one of its documented codes
// and every thread must finish.

#include <array>
#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "common/slot_vector.h"
#include "common/types.h"

namespace Libraries::Kernel {

using OrbisKernelSema = Common::SlotId;

// Declared here because the semaphore API has no header: it is reached from the guest through
// LIB_FUNCTION registrations only.
s32 PS4_SYSV_ABI sceKernelCreateSema(OrbisKernelSema* sem, const char* pName, u32 attr,
                                     s32 initCount, s32 maxCount, const void* pOptParam);
s32 PS4_SYSV_ABI sceKernelSignalSema(OrbisKernelSema sem, s32 signalCount);
s32 PS4_SYSV_ABI sceKernelPollSema(OrbisKernelSema sem, s32 needCount);
s32 PS4_SYSV_ABI sceKernelCancelSema(OrbisKernelSema sem, s32 setCount, s32* pNumWaitThreads);
s32 PS4_SYSV_ABI sceKernelDeleteSema(OrbisKernelSema sem);

} // namespace Libraries::Kernel

namespace {

using namespace Libraries::Kernel;

constexpr s32 kOk = 0;
constexpr s32 kEsrch = 0x80020003;  // ORBIS_KERNEL_ERROR_ESRCH
constexpr s32 kEbusy = 0x80020010;  // ORBIS_KERNEL_ERROR_EBUSY
constexpr s32 kEinval = 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL

// A handle is invalid until some thread publishes one, and goes back to invalid when deleted.
constexpr u32 kEmpty = Common::SlotId::INVALID_INDEX;

/// A small window of shared handles, so that create/delete keep colliding on the same slots
/// instead of drifting apart into private ranges.
class HandleWindow {
public:
    explicit HandleWindow(size_t size) : slots(size) {
        for (auto& slot : slots) {
            slot.store(kEmpty, std::memory_order_relaxed);
        }
    }

    /// Publishes a handle into a slot, or returns it to the caller to destroy if the slot is taken.
    bool Publish(size_t index, u32 handle) {
        u32 expected = kEmpty;
        return slots[index % slots.size()].compare_exchange_strong(expected, handle,
                                                                   std::memory_order_acq_rel);
    }

    /// Claims a handle out of a slot so exactly one thread deletes it.
    u32 Claim(size_t index) {
        return slots[index % slots.size()].exchange(kEmpty, std::memory_order_acq_rel);
    }

    /// Reads a slot without claiming it: the handle may be deleted by another thread before, or
    /// while, the caller uses it. That is the point.
    u32 Peek(size_t index) const {
        return slots[index % slots.size()].load(std::memory_order_acquire);
    }

    size_t Size() const {
        return slots.size();
    }

private:
    std::vector<std::atomic<u32>> slots;
};

bool IsExpectedCode(s32 result) {
    return result == kOk || result == kEsrch || result == kEbusy || result == kEinval;
}

} // namespace

// Every thread creates, uses and deletes semaphores through a shared window of handles. Threads
// deliberately operate on handles they did not create, which is what makes a delete race with a
// concurrent lookup. The container also has to grow past its initial capacity so that an insert
// reallocates while other threads are indexing it.
TEST(SemaphoreHandleTable, ConcurrentCreateUseDeleteIsSafe) {
    constexpr size_t kThreads = 8;
    constexpr size_t kIterations = 20000;
    constexpr size_t kWindow = 16;

    HandleWindow window{kWindow};
    std::atomic<bool> unexpected_code{false};
    std::atomic<u64> completed_ops{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (size_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (size_t i = 0; i < kIterations; ++i) {
                const size_t slot = (i * 7 + t) % window.Size();
                switch ((i + t) % 5) {
                case 0: {
                    OrbisKernelSema sem{};
                    const s32 res = sceKernelCreateSema(&sem, "test", 0, 1, 8, nullptr);
                    if (res != kOk) {
                        unexpected_code.store(true, std::memory_order_relaxed);
                        break;
                    }
                    if (!window.Publish(slot, sem.index)) {
                        // The window slot was taken: drop the semaphore we just made rather than
                        // leaking it.
                        sceKernelDeleteSema(sem);
                    }
                    break;
                }
                case 1: {
                    const u32 handle = window.Peek(slot);
                    if (handle != kEmpty && !IsExpectedCode(sceKernelSignalSema(handle, 1))) {
                        unexpected_code.store(true, std::memory_order_relaxed);
                    }
                    break;
                }
                case 2: {
                    const u32 handle = window.Peek(slot);
                    if (handle != kEmpty && !IsExpectedCode(sceKernelPollSema(handle, 1))) {
                        unexpected_code.store(true, std::memory_order_relaxed);
                    }
                    break;
                }
                case 3: {
                    const u32 handle = window.Peek(slot);
                    s32 waiters = 0;
                    if (handle != kEmpty &&
                        !IsExpectedCode(sceKernelCancelSema(handle, 1, &waiters))) {
                        unexpected_code.store(true, std::memory_order_relaxed);
                    }
                    break;
                }
                default: {
                    const u32 handle = window.Claim(slot);
                    if (handle != kEmpty && !IsExpectedCode(sceKernelDeleteSema(handle))) {
                        unexpected_code.store(true, std::memory_order_relaxed);
                    }
                    break;
                }
                }
                completed_ops.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    // Drain whatever is still published so the table is left empty for the next test.
    for (size_t slot = 0; slot < window.Size(); ++slot) {
        const u32 handle = window.Claim(slot);
        if (handle != kEmpty) {
            sceKernelDeleteSema(handle);
        }
    }

    EXPECT_FALSE(unexpected_code.load()) << "an operation returned a code outside its contract";
    EXPECT_EQ(completed_ops.load(), kThreads * kIterations);
}

// Two threads deleting the same handle must not both succeed: the allocation check and the slot
// recycle have to happen together, or the semaphore is freed twice and its slot handed out twice.
TEST(SemaphoreHandleTable, ConcurrentDeleteOfSameHandleSucceedsOnce) {
    constexpr size_t kRounds = 5000;
    constexpr size_t kDeleters = 4;

    std::atomic<u64> total_ok{0};
    for (size_t round = 0; round < kRounds; ++round) {
        OrbisKernelSema sem{};
        ASSERT_EQ(sceKernelCreateSema(&sem, "dup", 0, 1, 4, nullptr), kOk);

        std::atomic<u32> start{0};
        std::atomic<u32> ok_count{0};
        std::vector<std::thread> threads;
        threads.reserve(kDeleters);
        for (size_t d = 0; d < kDeleters; ++d) {
            threads.emplace_back([&] {
                start.fetch_add(1, std::memory_order_acq_rel);
                while (start.load(std::memory_order_acquire) < kDeleters) {
                    std::this_thread::yield();
                }
                if (sceKernelDeleteSema(sem) == kOk) {
                    ok_count.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        // The slot may legitimately have been recycled by another round, but within a round only
        // one of the concurrent deletes can be the one that freed this semaphore.
        ASSERT_LE(ok_count.load(), 1u) << "the same semaphore was deleted more than once";
        total_ok.fetch_add(ok_count.load(), std::memory_order_relaxed);
    }
    EXPECT_EQ(total_ok.load(), kRounds) << "every round should have exactly one successful delete";
}
