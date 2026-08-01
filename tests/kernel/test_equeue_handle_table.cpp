// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The event-queue handle table is a process-wide std::unordered_map that guest titles drive
// concurrently: syscall threads create, wait and delete, while timer and GPU/VideoOut paths trigger
// events on the same queues. It used to be unguarded, so an insert that rehashed the bucket array,
// or a delete racing with a concurrent lookup, freed an EqueueInternal out from under another
// thread. The sharpest window is sceKernelWaitEqueue, which looks the queue up and then blocks in
// the unlocked WaitForEvents while another thread can delete it.
//
// These tests reproduce that churn through the public API. They are written to fault, not to
// assert: against the unguarded map the race shows up as a use-after-free or map corruption.

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "common/types.h"
#include "core/libraries/kernel/equeue.h"

namespace Libraries::Kernel {

// Declared here because the event-queue API is reached from the guest through LIB_FUNCTION
// registrations only.
int PS4_SYSV_ABI sceKernelCreateEqueue(OrbisKernelEqueue* eq, const char* name);
int PS4_SYSV_ABI sceKernelDeleteEqueue(OrbisKernelEqueue eq);
int PS4_SYSV_ABI sceKernelWaitEqueue(OrbisKernelEqueue eq, OrbisKernelEvent* ev, int num, int* out,
                                     OrbisKernelUseconds* timo);
int PS4_SYSV_ABI sceKernelAddUserEvent(OrbisKernelEqueue eq, int id);
int PS4_SYSV_ABI sceKernelTriggerUserEvent(OrbisKernelEqueue eq, int id, void* udata);
int PS4_SYSV_ABI sceKernelDeleteUserEvent(OrbisKernelEqueue eq, int id);

} // namespace Libraries::Kernel

namespace {

using namespace Libraries::Kernel;

constexpr int kOk = 0;
constexpr int kEbadf = 0x80020009;     // ORBIS_KERNEL_ERROR_EBADF
constexpr int kEnoent = 0x80020002;    // ORBIS_KERNEL_ERROR_ENOENT
constexpr int kEnomem = 0x8002000c;    // ORBIS_KERNEL_ERROR_ENOMEM
constexpr int kEtimedout = 0x8002003c; // ORBIS_KERNEL_ERROR_ETIMEDOUT

constexpr OrbisKernelEqueue kEmpty = 0;

bool IsExpectedCode(int result) {
    return result == kOk || result == kEbadf || result == kEnoent || result == kEnomem ||
           result == kEtimedout;
}

/// A small window of shared handles, so create and delete keep colliding on the same queues.
class QueueWindow {
public:
    explicit QueueWindow(size_t size) : slots(size) {
        for (auto& slot : slots) {
            slot.store(kEmpty, std::memory_order_relaxed);
        }
    }

    bool Publish(size_t index, OrbisKernelEqueue eq) {
        OrbisKernelEqueue expected = kEmpty;
        return slots[index % slots.size()].compare_exchange_strong(expected, eq,
                                                                   std::memory_order_acq_rel);
    }

    OrbisKernelEqueue Claim(size_t index) {
        return slots[index % slots.size()].exchange(kEmpty, std::memory_order_acq_rel);
    }

    /// Reads a slot without claiming it: another thread may delete the queue before, or while, the
    /// caller uses it. That is the point.
    OrbisKernelEqueue Peek(size_t index) const {
        return slots[index % slots.size()].load(std::memory_order_acquire);
    }

    size_t Size() const {
        return slots.size();
    }

private:
    std::vector<std::atomic<OrbisKernelEqueue>> slots;
};

} // namespace

// Threads create, arm, trigger, wait on and delete queues through a shared window, operating on
// handles they did not create. The waits use a short timeout so a delete can land while another
// thread is inside the unlocked WaitForEvents.
TEST(EqueueHandleTable, ConcurrentCreateWaitDeleteIsSafe) {
    constexpr size_t kThreads = 8;
    constexpr size_t kIterations = 4000;
    constexpr size_t kWindow = 16;

    QueueWindow window{kWindow};
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
                    OrbisKernelEqueue eq{};
                    if (sceKernelCreateEqueue(&eq, "test") != kOk) {
                        unexpected_code.store(true, std::memory_order_relaxed);
                        break;
                    }
                    sceKernelAddUserEvent(eq, 1);
                    if (!window.Publish(slot, eq)) {
                        sceKernelDeleteEqueue(eq);
                    }
                    break;
                }
                case 1: {
                    const auto eq = window.Peek(slot);
                    if (eq != kEmpty &&
                        !IsExpectedCode(sceKernelTriggerUserEvent(eq, 1, nullptr))) {
                        unexpected_code.store(true, std::memory_order_relaxed);
                    }
                    break;
                }
                case 2: {
                    const auto eq = window.Peek(slot);
                    if (eq != kEmpty) {
                        OrbisKernelEvent ev{};
                        int out = 0;
                        OrbisKernelUseconds timo = 200;
                        if (!IsExpectedCode(sceKernelWaitEqueue(eq, &ev, 1, &out, &timo))) {
                            unexpected_code.store(true, std::memory_order_relaxed);
                        }
                    }
                    break;
                }
                case 3: {
                    const auto eq = window.Peek(slot);
                    if (eq != kEmpty && !IsExpectedCode(sceKernelDeleteUserEvent(eq, 1))) {
                        unexpected_code.store(true, std::memory_order_relaxed);
                    }
                    break;
                }
                default: {
                    const auto eq = window.Claim(slot);
                    if (eq != kEmpty && !IsExpectedCode(sceKernelDeleteEqueue(eq))) {
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

    for (size_t slot = 0; slot < window.Size(); ++slot) {
        const auto eq = window.Claim(slot);
        if (eq != kEmpty) {
            sceKernelDeleteEqueue(eq);
        }
    }

    EXPECT_FALSE(unexpected_code.load()) << "an operation returned a code outside its contract";
    EXPECT_EQ(completed_ops.load(), kThreads * kIterations);
}

// A queue deleted while a waiter is blocked on it must not be freed under that waiter: the waiter
// holds it alive for the duration of the call, and returns normally.
TEST(EqueueHandleTable, DeleteDuringBlockingWaitIsSafe) {
    constexpr size_t kRounds = 500;

    for (size_t round = 0; round < kRounds; ++round) {
        OrbisKernelEqueue eq{};
        ASSERT_EQ(sceKernelCreateEqueue(&eq, "waited"), kOk);
        ASSERT_EQ(sceKernelAddUserEvent(eq, 1), kOk);

        std::atomic<bool> waiting{false};
        std::thread waiter{[&] {
            OrbisKernelEvent ev{};
            int out = 0;
            OrbisKernelUseconds timo = 2000;
            waiting.store(true, std::memory_order_release);
            const int res = sceKernelWaitEqueue(eq, &ev, 1, &out, &timo);
            EXPECT_TRUE(IsExpectedCode(res)) << "wait returned " << res;
        }};
        while (!waiting.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // Delete while the other thread is inside the unlocked wait.
        EXPECT_TRUE(IsExpectedCode(sceKernelDeleteEqueue(eq)));
        waiter.join();
    }
}

// The same delete, against a waiter with an infinite timeout (timo == nullptr). That waiter has no
// timeout to fall back on: only the deleted-flag wakeup in sceKernelDeleteEqueue can bring it back.
// Without it the thread holds its own shared_ptr on a queue nobody can trigger any more and never
// returns, blocking process shutdown. The bounded join below turns that regression into a test
// failure instead of a suite that hangs forever.
TEST(EqueueHandleTable, DeleteWakesInfiniteTimeoutWaiter) {
    constexpr size_t kRounds = 16;
    constexpr auto kWaiterDeadline = std::chrono::seconds(5);

    // Heap-shared with the waiter: if a regression leaves it blocked it gets detached, and a late
    // wakeup must not touch this test's dead stack frame.
    struct WaiterState {
        std::atomic<bool> waiting{false};
        std::atomic<bool> returned{false};
        int result = 0;
        int out_count = -1;
    };

    size_t woken_rounds = 0;
    for (size_t round = 0; round < kRounds; ++round) {
        OrbisKernelEqueue eq{};
        ASSERT_EQ(sceKernelCreateEqueue(&eq, "waited-forever"), kOk);
        ASSERT_EQ(sceKernelAddUserEvent(eq, 1), kOk);

        auto state = std::make_shared<WaiterState>();
        std::thread waiter{[state, eq] {
            OrbisKernelEvent ev{};
            int out = -1;
            state->waiting.store(true, std::memory_order_release);
            const int res = sceKernelWaitEqueue(eq, &ev, 1, &out, nullptr);
            state->result = res;
            state->out_count = out;
            state->returned.store(true, std::memory_order_release);
        }};
        while (!state->waiting.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // Let the waiter park inside the indefinite wait. Nothing ever triggers the event, so
        // only the delete can wake it.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        EXPECT_EQ(sceKernelDeleteEqueue(eq), kOk);

        const auto deadline = std::chrono::steady_clock::now() + kWaiterDeadline;
        while (!state->returned.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!state->returned.load(std::memory_order_acquire)) {
            waiter.detach();
            FAIL() << "infinite-timeout waiter still blocked " << kWaiterDeadline.count()
                   << " s after the delete (round " << round << ")";
        }
        waiter.join();

        // The wakeup reports zero events, which sceKernelWaitEqueue maps to ETIMEDOUT. EBADF is
        // the one benign alternative: the delete won the race to the handle lookup before the
        // wait even started.
        if (state->result == kEtimedout) {
            EXPECT_EQ(state->out_count, 0);
            ++woken_rounds;
        } else {
            EXPECT_EQ(state->result, kEbadf) << "wait returned " << state->result;
        }
    }

    // The parked-then-woken interleaving is the one this test exists for; with the parking delay
    // it is the overwhelmingly common outcome. All-EBADF would mean the wake path was never
    // exercised at all.
    EXPECT_GT(woken_rounds, 0u);
}
