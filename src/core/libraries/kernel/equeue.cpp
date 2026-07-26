// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <magic_enum/magic_enum.hpp>

#include "common/assert.h"
#include "common/debug.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/file_sys/fs.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"

namespace Libraries::Kernel {

// Timestamp of the most recent shader or pipeline compile, in steady-clock nanoseconds. It lives
// here because the wait policy below is its only consumer.
static std::atomic<u64> last_shader_compile_ns{0};

static u64 SteadyNowNs() {
    // duration_cast, not raw ticks: steady_clock::period is not required to be nanoseconds.
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

static bool ShaderCompileRecent(std::chrono::nanoseconds within) {
    const u64 last = last_shader_compile_ns.load(std::memory_order_relaxed);
    if (last == 0) {
        return false;
    }
    const u64 now = SteadyNowNs();
    return now >= last && (now - last) < static_cast<u64>(within.count());
}

void SignalShaderCompile() {
    last_shader_compile_ns.store(SteadyNowNs(), std::memory_order_relaxed);
}

extern boost::asio::io_context io_context;
extern void KernelSignalRequest();

// std::unordered_map is NOT thread-safe, but guest titles create/wait/delete event queues and
// add/trigger events concurrently from many threads: syscall threads, the boost::asio timer thread
// that runs the callbacks below, and the GPU/VideoOut paths that reach in through GetEqueue().
// Guard the container with a dedicated mutex, and hold every queue through a shared_ptr so it stays
// alive across the unlocked blocking wait in sceKernelWaitEqueue even after another thread deletes
// it (and its handle is recycled). Without this, an insert that rehashes the map, or a delete
// racing with a concurrent access, frees an EqueueInternal out from under another thread ->
// use-after-free / map corruption. This mirrors the semaphore handle-table fix. Never hold
// kqueues_mutex across the blocking wait or we would deadlock against TriggerEvent.
static std::mutex kqueues_mutex;
static std::unordered_map<s32, std::shared_ptr<EqueueInternal>> kqueues;
static constexpr auto HrTimerSpinlockThresholdNs = 1200000u;

static std::shared_ptr<EqueueInternal> FindEqueue(OrbisKernelEqueue eq) {
    std::scoped_lock lock{kqueues_mutex};
    const auto it = kqueues.find(eq);
    if (it == kqueues.cend()) {
        return nullptr;
    }
    return it->second;
}

std::shared_ptr<EqueueInternal> GetEqueue(OrbisKernelEqueue eq) {
    // Hand out the owning reference, not a raw pointer into it: sceGnmAddEqEvent stashes the
    // result in an IRQ handler that outlives this call, so a raw pointer dangles the moment the
    // guest deletes the queue. Callers use `auto`, so they keep the queue alive for as long as
    // they hold it.
    return FindEqueue(eq);
}

static void HrTimerCallback(OrbisKernelEqueue eq, const OrbisKernelEvent& kevent) {
    if (const auto equeue = FindEqueue(eq)) {
        equeue->TriggerEvent(kevent.ident, OrbisKernelEvent::Filter::HrTimer, kevent.udata);
    }
}

static void TimerCallback(OrbisKernelEqueue eq, const OrbisKernelEvent& kevent) {
    const auto equeue = FindEqueue(eq);
    if (equeue && equeue->EventExists(kevent.ident, kevent.filter)) {
        equeue->TriggerEvent(kevent.ident, OrbisKernelEvent::Filter::Timer, kevent.udata);
        if (!(kevent.flags & OrbisKernelEvent::Flags::OneShot)) {
            // Reschedule the event for its next period.
            equeue->ScheduleEvent(kevent.ident, kevent.filter, TimerCallback);
        }
    }
}

// Events are uniquely identified by id and filter.
bool EqueueInternal::AddEvent(EqueueEvent& event) {
    // Save id and filter before event is moved into m_events.
    const u64 id = event.event.ident;
    const auto filter = event.event.filter;

    {
        std::scoped_lock lock{m_mutex};

        // Calculate timer interval
        event.time_added = std::chrono::steady_clock::now();
        if (event.event.filter == OrbisKernelEvent::Filter::Timer) {
            // Set timer interval, this is stored in milliseconds for timers.
            event.timer_interval = std::chrono::milliseconds(event.event.data);
        } else if (event.event.filter == OrbisKernelEvent::Filter::HrTimer) {
            // Retrieve inputted time, this is stored in the bintime format.
            OrbisKernelBintime* time = reinterpret_cast<OrbisKernelBintime*>(event.event.data);

            // Convert the bintime format to a timespec.
            OrbisKernelTimespec ts;
            ts.tv_sec = time->sec;
            ts.tv_nsec = (1000000000 * (time->frac >> 32)) >> 32;

            // Then use the timespec to set the timer interval.
            event.timer_interval = std::chrono::nanoseconds(ts.tv_nsec + ts.tv_sec * 1000000000);
        }

        // First, check if there's already an event with the same id and filter.
        const auto& find_it = std::ranges::find_if(m_events, [id, filter](auto& ev) {
            return ev.event.ident == id && ev.event.filter == filter;
        });
        // If there is a duplicate event, we need to update that instead.
        if (find_it != m_events.cend()) {
            // Specifically, update user data and timer_interval.
            // Trigger status and event data should remain intact.
            auto& old_event = *find_it;
            old_event.timer_interval = event.timer_interval;
            old_event.event.udata = event.event.udata;
            return true;
        }

        // Clear input data from event.
        event.event.data = 0;

        // Remove add flag from event
        event.event.flags &= ~OrbisKernelEvent::Flags::Add;

        // Clear flag is appended to most event types internally.
        if (event.event.filter != OrbisKernelEvent::Filter::User) {
            event.event.flags |= OrbisKernelEvent::Flags::Clear;
        }

        const auto& it = std::ranges::find(m_events, event);
        if (it != m_events.cend()) {
            *it = std::move(event);
        } else {
            m_events.emplace_back(std::move(event));
        }
    }

    // Schedule callbacks for timer events
    if (filter == OrbisKernelEvent::Filter::Timer) {
        return this->ScheduleEvent(id, OrbisKernelEvent::Filter::Timer, TimerCallback);
    } else if (filter == OrbisKernelEvent::Filter::HrTimer) {
        return this->ScheduleEvent(id, OrbisKernelEvent::Filter::HrTimer, HrTimerCallback);
    }

    return true;
}

bool EqueueInternal::ScheduleEvent(u64 id, s16 filter,
                                   void (*callback)(OrbisKernelEqueue, const OrbisKernelEvent&)) {
    std::scoped_lock lock{m_mutex};

    const auto& it = std::ranges::find_if(m_events, [id, filter](auto& ev) {
        return ev.event.ident == id && ev.event.filter == filter;
    });
    if (it == m_events.cend()) {
        return false;
    }

    const auto& event = *it;
    ASSERT(event.event.filter == OrbisKernelEvent::Filter::Timer ||
           event.event.filter == OrbisKernelEvent::Filter::HrTimer);

    if (!it->timer) {
        it->timer = std::make_unique<boost::asio::steady_timer>(io_context, event.timer_interval);
    } else {
        // If the timer already exists we are scheduling a reoccurrence after the next period.
        // Set the expiration time to the previous occurrence plus the period.
        it->timer->expires_at(it->timer->expiry() + event.timer_interval);
    }

    it->timer->async_wait(
        [this, event_data = event.event, callback](const boost::system::error_code& ec) {
            if (ec) {
                if (ec != boost::system::errc::operation_canceled) {
                    LOG_ERROR(Kernel_Event, "Timer callback error: {}", ec.message());
                } else {
                    // Timer was cancelled (removed) before it triggered
                    LOG_DEBUG(Kernel_Event, "Timer cancelled");
                }
                return;
            }
            callback(this->m_handle, event_data);
        });
    KernelSignalRequest();

    return true;
}

bool EqueueInternal::RemoveEvent(u64 id, s16 filter) {
    bool has_found = false;
    std::scoped_lock lock{m_mutex};

    const auto& it = std::ranges::find_if(m_events, [id, filter](auto& ev) {
        return ev.event.ident == id && ev.event.filter == filter;
    });
    if (it != m_events.cend()) {
        m_events.erase(it);
        has_found = true;
    }
    return has_found;
}

int EqueueInternal::WaitForEvents(OrbisKernelEvent* ev, int num, const OrbisKernelUseconds* timo) {
    if (timo != nullptr && *timo == 0) {
        // Effectively acts as a poll; only events that have already
        // arrived at the time of this function call can be received
        return GetTriggeredEvents(ev, num);
    }
    const auto micros = timo ? *timo : 0u;

    if (HasSmallTimer()) {
        // If a small timer is set, just wait for it to expire.
        return WaitForSmallTimer(ev, num, micros);
    }

    int count = 0;

    const auto predicate = [&] {
        count = GetTriggeredEvents(ev, num);
        return count > 0 || m_deleted;
    };

    if (micros == 0) {
        // Wait indefinitely for events
        std::unique_lock lock{m_mutex};
        m_cond.wait(lock, predicate);
    } else {
        // Wait up until the timeout value
        std::unique_lock lock{m_mutex};
        m_cond.wait_for(lock, std::chrono::microseconds(micros), predicate);
    }

    return count;
}

bool EqueueInternal::TriggerEvent(u64 ident, s16 filter, void* trigger_data) {
    bool has_found = false;
    {
        std::scoped_lock lock{m_mutex};
        for (auto& event : m_events) {
            if (event.event.ident == ident && event.event.filter == filter) {
                if (filter == OrbisKernelEvent::Filter::VideoOut) {
                    event.TriggerDisplay(trigger_data);
                } else if (filter == OrbisKernelEvent::Filter::User) {
                    event.TriggerUser(trigger_data);
                } else if (filter == OrbisKernelEvent::Filter::Timer ||
                           filter == OrbisKernelEvent::Filter::HrTimer) {
                    event.TriggerTimer();
                } else {
                    event.Trigger(trigger_data);
                }
                has_found = true;
            }
        }
    }
    m_cond.notify_one();
    return has_found;
}

int EqueueInternal::GetTriggeredEvents(OrbisKernelEvent* ev, int num) {
    int count = 0;
    for (auto it = m_events.begin(); it != m_events.end();) {
        if (it->IsTriggered()) {
            ev[count++] = it->event;
            if (it->event.flags & OrbisKernelEvent::Flags::Clear) {
                it->Clear();
            }
            if (it->event.flags & OrbisKernelEvent::Flags::OneShot) {
                it = m_events.erase(it);
            } else {
                ++it;
            }

            if (count == num) {
                break;
            }
        } else {
            ++it;
        }
    }

    return count;
}

bool EqueueInternal::AddSmallTimer(EqueueEvent& ev) {
    // Retrieve inputted time, this is stored in the bintime format
    OrbisKernelBintime* time = reinterpret_cast<OrbisKernelBintime*>(ev.event.data);
    OrbisKernelTimespec ts;
    ts.tv_sec = time->sec;
    ts.tv_nsec = ((1000000000 * (time->frac >> 32)) >> 32);

    // Create the small timer
    SmallTimer st;
    st.event = ev.event;
    st.added = std::chrono::steady_clock::now();
    st.interval = std::chrono::nanoseconds(ts.tv_nsec + ts.tv_sec * 1000000000);
    {
        std::scoped_lock lock{m_mutex};
        m_small_timers[st.event.ident] = std::move(st);
    }
    return true;
}

int EqueueInternal::WaitForSmallTimer(OrbisKernelEvent* ev, int num, u32 micros) {
    ASSERT(num >= 1);

    auto curr_clock = std::chrono::steady_clock::now();
    const auto wait_end_us = (micros == 0) ? std::chrono::steady_clock::time_point::max()
                                           : curr_clock + std::chrono::microseconds{micros};
    int count = 0;
    do {
        curr_clock = std::chrono::steady_clock::now();
        {
            std::scoped_lock lock{m_mutex};
            for (auto it = m_small_timers.begin(); it != m_small_timers.end() && count < num;) {
                const SmallTimer& st = it->second;

                if (curr_clock - st.added >= st.interval) {
                    ev[count++] = st.event;
                    it = m_small_timers.erase(it);
                } else {
                    ++it;
                }
            }

            if (count > 0)
                return count;
        }
        std::this_thread::yield();
    } while (curr_clock < wait_end_us);

    return 0;
}

bool EqueueInternal::EventExists(u64 id, s16 filter) {
    std::scoped_lock lock{m_mutex};

    const auto& it = std::ranges::find_if(m_events, [id, filter](auto& ev) {
        return ev.event.ident == id && ev.event.filter == filter;
    });

    return it != m_events.cend();
}

s32 PS4_SYSV_ABI posix_kqueue() {
    // Reserve a file handle for the kqueue
    auto* handles = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    s32 kqueue_handle = handles->CreateHandle();
    auto* kqueue_file = handles->GetFile(kqueue_handle);
    kqueue_file->type = Core::FileSys::FileType::Equeue;

    // Plenty of equeue logic uses names to identify queues.
    // Create a unique name for the queue we create.
    char name[32];
    memset(name, 0, sizeof(name));
    snprintf(name, sizeof(name), "kqueue%i", kqueue_handle);

    // Create the queue
    auto equeue = std::make_shared<EqueueInternal>(kqueue_handle, name);
    {
        std::scoped_lock lock{kqueues_mutex};
        kqueues[kqueue_handle] = std::move(equeue);
    }
    LOG_INFO(Kernel_Event, "kqueue created with name {}", name);

    // Return handle.
    return kqueue_handle;
}

// Helper method to detect supported filters.
// We don't want to allow adding events we don't handle properly.
bool SupportedEqueueFilter(OrbisKernelEvent::Filter filter) {
    return filter == OrbisKernelEvent::Filter::GraphicsCore ||
           filter == OrbisKernelEvent::Filter::HrTimer ||
           filter == OrbisKernelEvent::Filter::Timer || filter == OrbisKernelEvent::Filter::User ||
           filter == OrbisKernelEvent::Filter::VideoOut;
}

s32 PS4_SYSV_ABI posix_kevent(s32 handle, OrbisKernelEvent* changelist, u64 nchanges,
                              OrbisKernelEvent* eventlist, u64 nevents,
                              OrbisKernelTimespec* timeout) {
    LOG_INFO(Kernel_Event, "called, eq = {}, nchanges = {}, nevents = {}", handle, nchanges,
             nevents);

    // Get the equeue
    const auto equeue = FindEqueue(handle);
    if (!equeue) {
        *__Error() = POSIX_EBADF;
        return ORBIS_FAIL;
    }

    // First step is to apply all changes in changelist.
    for (u64 i = 0; i < nchanges; i++) {
        auto event = changelist[i];
        if (!SupportedEqueueFilter(event.filter)) {
            LOG_ERROR(Kernel_Event, "Unsupported event filter {}",
                      magic_enum::enum_name(event.filter));
            continue;
        }

        // Check the event flags to determine the appropriate action
        if (event.flags & OrbisKernelEvent::Flags::Add) {
            // The caller is requesting to add an event.
            EqueueEvent internal_event{};
            internal_event.event = event;
            if (!equeue->AddEvent(internal_event)) {
                // Failed to add event, return error.
                *__Error() = POSIX_ENOMEM;
                return ORBIS_FAIL;
            }
        }

        if (event.flags & OrbisKernelEvent::Flags::Delete) {
            // The caller is requesting to remove an event.
            if (!equeue->RemoveEvent(event.ident, event.filter)) {
                // Failed to remove event, return error.
                *__Error() = POSIX_ENOENT;
                return ORBIS_FAIL;
            }
        }

        if (event.filter == OrbisKernelEvent::Filter::User && event.fflags == 0x1000000) {
            // For user events, this fflags value indicates we need to trigger the event.
            if (!equeue->TriggerEvent(event.ident, OrbisKernelEvent::Filter::User, event.udata)) {
                *__Error() = POSIX_ENOENT;
                return ORBIS_FAIL;
            }
        } else if (event.fflags != 0) {
            // The title is using filter-specific flags. Right now, these are unhandled.
            LOG_ERROR(Kernel_Event, "Unhandled fflags {:#x} for event filter {}", event.fflags,
                      magic_enum::enum_name(event.filter));
            continue;
        }
    }

    // Now we need to wait on the event list.
    s32 count = 0;
    if (nevents > 0) {
        if (timeout != nullptr) {
            OrbisKernelUseconds micros = (timeout->tv_sec * 1000000) + (timeout->tv_nsec / 1000);
            count = equeue->WaitForEvents(eventlist, nevents, &micros);
        } else {
            count = equeue->WaitForEvents(eventlist, nevents, nullptr);
        }
    }
    return count;
}

int PS4_SYSV_ABI sceKernelCreateEqueue(OrbisKernelEqueue* eq, const char* name) {
    if (eq == nullptr) {
        LOG_ERROR(Kernel_Event, "Event queue is null!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    if (name == nullptr) {
        LOG_ERROR(Kernel_Event, "Event queue name is null!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    // Maximum is 32 including null terminator
    static constexpr u64 MaxEventQueueNameSize = 32;
    if (std::strlen(name) > MaxEventQueueNameSize) {
        LOG_ERROR(Kernel_Event, "Event queue name exceeds 32 bytes!");
        return ORBIS_KERNEL_ERROR_ENAMETOOLONG;
    }

    LOG_INFO(Kernel_Event, "name = {}", name);

    // Reserve a file handle for the kqueue
    auto* handles = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    OrbisKernelEqueue kqueue_handle = handles->CreateHandle();
    auto* kqueue_file = handles->GetFile(kqueue_handle);
    kqueue_file->type = Core::FileSys::FileType::Equeue;

    // Create the equeue
    auto equeue = std::make_shared<EqueueInternal>(kqueue_handle, name);
    {
        std::scoped_lock lock{kqueues_mutex};
        kqueues[kqueue_handle] = std::move(equeue);
    }
    *eq = kqueue_handle;

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteEqueue(OrbisKernelEqueue eq) {
    std::shared_ptr<EqueueInternal> equeue;
    {
        std::scoped_lock lock{kqueues_mutex};
        const auto it = kqueues.find(eq);
        if (it == kqueues.cend()) {
            return ORBIS_KERNEL_ERROR_EBADF;
        }
        // Move the owning reference out and erase the slot atomically with the allocation check so
        // two concurrent deletes cannot both observe it allocated and double-free the queue.
        equeue = std::move(it->second);
        kqueues.erase(it);
    }

    // Wake anyone blocked in WaitForEvents. Without this a waiter with an infinite timeout
    // never returns: it holds its own shared_ptr, so the queue stays alive and the thread
    // hangs for the rest of the process's life.
    equeue->MarkDeleted();

    auto* handles = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    handles->DeleteHandle(eq);
    // The EqueueInternal is released here, after the container lock is dropped; any concurrent
    // waiter that still holds a shared_ptr keeps it alive until it returns from WaitForEvents.
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelWaitEqueue(OrbisKernelEqueue eq, OrbisKernelEvent* ev, int num, int* out,
                                     OrbisKernelUseconds* timo) {
    HLE_TRACE;
    // Hold the queue alive through a shared_ptr for the whole call: the wait below runs unlocked
    // and may block, and another thread may delete (and recycle) this handle in the meantime.
    const auto equeue = FindEqueue(eq);
    if (!equeue) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    TRACE_HINT(equeue->GetName());
    LOG_TRACE(Kernel_Event, "equeue = {} num = {}", equeue->GetName(), num);

    if (ev == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }

    if (num < 1) {
        *out = 0;
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    // A compile burst blocks the GPU command processor for longer than the timeouts titles wait
    // with, so a GPU-completion watchdog gives up on work that is slow, not hung. Extend only
    // waits on queues carrying an event a compile stall defers, and only right after a compile.
    static constexpr OrbisKernelUseconds MinExtendableTimeout = 100'000; // 100 ms
    static constexpr OrbisKernelUseconds ExtendedTimeout = 10'000'000;   // 10 s
    static constexpr auto CompileWindow = std::chrono::seconds(2);
    OrbisKernelUseconds extended_timo{};
    const OrbisKernelUseconds* eff_timo = timo;
    // Below the floor the guest meant a poll, not a wait: stretching it would hang a thread that
    // was never blocking.
    if (timo != nullptr && *timo >= MinExtendableTimeout && ShaderCompileRecent(CompileWindow) &&
        equeue->HasGpuEvent()) {
        extended_timo = std::max(*timo, ExtendedTimeout);
        eff_timo = &extended_timo;
    }

    *out = equeue->WaitForEvents(ev, num, eff_timo);

    if (*out == 0) {
        return ORBIS_KERNEL_ERROR_ETIMEDOUT;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelAddHRTimerEvent(OrbisKernelEqueue eq, int id, OrbisKernelTimespec* ts,
                                          void* udata) {
    const auto equeue = FindEqueue(eq);
    if (!equeue) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    const auto total_ns = ts->tv_sec * 1000000000 + ts->tv_nsec;

    EqueueEvent event{};
    event.event.ident = id;
    event.event.filter = OrbisKernelEvent::Filter::HrTimer;
    event.event.flags = OrbisKernelEvent::Flags::Add | OrbisKernelEvent::Flags::OneShot;
    event.event.fflags = 0;
    // Data is stored as the address of a OrbisKernelBintime struct.
    OrbisKernelBintime time{ts->tv_sec, ts->tv_nsec * 0x44b82fa09};
    event.event.data = reinterpret_cast<u64>(&time);
    event.event.udata = udata;

    // HR timers cannot be implemented within the existing event queue architecture due to the
    // slowness of the notification mechanism. For instance, a 100us timer will lose its precision
    // as the trigger time drifts by +50-700%, depending on the host PC and workload. To address
    // this issue, we use a spinlock for small waits (which can be adjusted using
    // `HrTimerSpinlockThresholdUs`) and fall back to boost asio timers if the time to tick is
    // large. Even for large delays, we truncate a small portion to complete the wait
    // using the spinlock, prioritizing precision.
    if (total_ns < HrTimerSpinlockThresholdNs) {
        return equeue->AddSmallTimer(event) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOMEM;
    }

    if (!equeue->AddEvent(event)) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteHRTimerEvent(OrbisKernelEqueue eq, int id) {
    const auto equeue = FindEqueue(eq);
    if (!equeue) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    if (equeue->HasSmallTimer()) {
        return equeue->RemoveSmallTimer(id) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOENT;
    } else {
        return equeue->RemoveEvent(id, OrbisKernelEvent::Filter::HrTimer)
                   ? ORBIS_OK
                   : ORBIS_KERNEL_ERROR_ENOENT;
    }
}

int PS4_SYSV_ABI sceKernelAddTimerEvent(OrbisKernelEqueue eq, int id, OrbisKernelUseconds usec,
                                        void* udata) {
    const auto equeue = FindEqueue(eq);
    if (!equeue) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    EqueueEvent event{};
    event.event.ident = static_cast<u64>(id);
    event.event.filter = OrbisKernelEvent::Filter::Timer;
    event.event.flags = OrbisKernelEvent::Flags::Add;
    event.event.fflags = 0;
    event.event.data = usec / 1000;
    event.event.udata = udata;

    if (!equeue->AddEvent(event)) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteTimerEvent(OrbisKernelEqueue eq, int id) {
    const auto equeue = FindEqueue(eq);
    if (!equeue) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    return equeue->RemoveEvent(id, OrbisKernelEvent::Filter::Timer) ? ORBIS_OK
                                                                    : ORBIS_KERNEL_ERROR_ENOENT;
}

int PS4_SYSV_ABI sceKernelAddUserEvent(OrbisKernelEqueue eq, int id) {
    const auto equeue = FindEqueue(eq);
    if (!equeue) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    EqueueEvent event{};
    event.event.ident = id;
    event.event.filter = OrbisKernelEvent::Filter::User;
    event.event.udata = 0;
    event.event.flags = OrbisKernelEvent::Flags::Add;
    event.event.fflags = 0;
    event.event.data = 0;

    return equeue->AddEvent(event) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOMEM;
}

int PS4_SYSV_ABI sceKernelAddUserEventEdge(OrbisKernelEqueue eq, int id) {
    const auto equeue = FindEqueue(eq);
    if (!equeue) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    EqueueEvent event{};
    event.event.ident = id;
    event.event.filter = OrbisKernelEvent::Filter::User;
    event.event.udata = 0;
    event.event.flags = OrbisKernelEvent::Flags::Add | OrbisKernelEvent::Flags::Clear;
    event.event.fflags = 0;
    event.event.data = 0;

    return equeue->AddEvent(event) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOMEM;
}

void* PS4_SYSV_ABI sceKernelGetEventUserData(const OrbisKernelEvent* ev) {
    ASSERT(ev);
    return ev->udata;
}

u64 PS4_SYSV_ABI sceKernelGetEventId(const OrbisKernelEvent* ev) {
    return ev->ident;
}

int PS4_SYSV_ABI sceKernelTriggerUserEvent(OrbisKernelEqueue eq, int id, void* udata) {
    const auto equeue = FindEqueue(eq);
    if (!equeue) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    if (!equeue->TriggerEvent(id, OrbisKernelEvent::Filter::User, udata)) {
        return ORBIS_KERNEL_ERROR_ENOENT;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteUserEvent(OrbisKernelEqueue eq, int id) {
    const auto equeue = FindEqueue(eq);
    if (!equeue) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    if (!equeue->RemoveEvent(id, OrbisKernelEvent::Filter::User)) {
        return ORBIS_KERNEL_ERROR_ENOENT;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelGetEventFilter(const OrbisKernelEvent* ev) {
    return ev->filter;
}

u64 PS4_SYSV_ABI sceKernelGetEventData(const OrbisKernelEvent* ev) {
    return ev->data;
}

void RegisterEventQueue(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("nh2IFMgKTv8", "libScePosix", 1, "libkernel", posix_kqueue);
    LIB_FUNCTION("RW-GEfpnsqg", "libScePosix", 1, "libkernel", posix_kevent);
    LIB_FUNCTION("D0OdFMjp46I", "libkernel", 1, "libkernel", sceKernelCreateEqueue);
    LIB_FUNCTION("jpFjmgAC5AE", "libkernel", 1, "libkernel", sceKernelDeleteEqueue);
    LIB_FUNCTION("fzyMKs9kim0", "libkernel", 1, "libkernel", sceKernelWaitEqueue);
    LIB_FUNCTION("vz+pg2zdopI", "libkernel", 1, "libkernel", sceKernelGetEventUserData);
    LIB_FUNCTION("4R6-OvI2cEA", "libkernel", 1, "libkernel", sceKernelAddUserEvent);
    LIB_FUNCTION("WDszmSbWuDk", "libkernel", 1, "libkernel", sceKernelAddUserEventEdge);
    LIB_FUNCTION("R74tt43xP6k", "libkernel", 1, "libkernel", sceKernelAddHRTimerEvent);
    LIB_FUNCTION("J+LF6LwObXU", "libkernel", 1, "libkernel", sceKernelDeleteHRTimerEvent);
    LIB_FUNCTION("57ZK+ODEXWY", "libkernel", 1, "libkernel", sceKernelAddTimerEvent);
    LIB_FUNCTION("YWQFUyXIVdU", "libkernel", 1, "libkernel", sceKernelDeleteTimerEvent);
    LIB_FUNCTION("F6e0kwo4cnk", "libkernel", 1, "libkernel", sceKernelTriggerUserEvent);
    LIB_FUNCTION("LJDwdSNTnDg", "libkernel", 1, "libkernel", sceKernelDeleteUserEvent);
    LIB_FUNCTION("mJ7aghmgvfc", "libkernel", 1, "libkernel", sceKernelGetEventId);
    LIB_FUNCTION("23CPPI1tyBY", "libkernel", 1, "libkernel", sceKernelGetEventFilter);
    LIB_FUNCTION("kwGyyjohI50", "libkernel", 1, "libkernel", sceKernelGetEventData);
}

} // namespace Libraries::Kernel
