// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <mutex>

#include <boost/container/small_vector.hpp>
#include <boost/icl/interval_set.hpp>
#include <tsl/robin_map.h>
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/hash.h"
#include "common/range_lock.h"
#include "common/scope_exit.h"
#include "common/signal_context.h"
#include "common/thread.h"
#include "core/memory.h"
#include "core/signals.h"
#include "video_core/gpu_authority_tracker.h"
#include "video_core/guest_copy_engine.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

#ifndef _WIN64
#include <sys/mman.h>
#include "common/adaptive_mutex.h"
#ifdef ENABLE_USERFAULTFD
#include <thread>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include "common/error.h"
#endif
#else
#include <windows.h>
#include "common/spin_lock.h"
#endif

#ifdef __linux__
#include "common/adaptive_mutex.h"
#else
#include "common/spin_lock.h"
#endif

namespace VideoCore {

constexpr size_t PM_PAGE_SIZE = 4_KB;
constexpr size_t PM_PAGE_BITS = 12;

struct PageManager::Impl {
    struct PageState {
        u8 num_write_watchers : 7;
        // At the moment only buffer cache can request read watchers.
        // And buffers cannot overlap, thus only 1 can exist per page.
        u8 num_read_watchers : 1;

        Core::MemoryPermission Perms() const noexcept {
            if (num_read_watchers > 0) {
                return Core::MemoryPermission::None;
            }
            if (num_write_watchers > 0) {
                return Core::MemoryPermission::Read;
            }
            return Core::MemoryPermission::ReadWrite;
        }

        template <s32 delta, bool is_read>
        u8 AddDelta() {
            if constexpr (is_read) {
                if constexpr (delta == 1) {
                    return ++num_read_watchers;
                } else if (delta == -1) {
                    ASSERT_MSG(num_read_watchers > 0, "Not enough watchers");
                    return --num_read_watchers;
                } else {
                    return num_read_watchers;
                }
            } else {
                if constexpr (delta == 1) {
                    return ++num_write_watchers;
                } else if (delta == -1) {
                    ASSERT_MSG(num_write_watchers > 0, "Not enough watchers");
                    return --num_write_watchers;
                } else {
                    return num_write_watchers;
                }
            }
        }
    };

    struct WriteObserver {
        u64 id;
        MemoryWriteCallback callback;
        void* user_data;
    };

    struct WatchedPage {
        boost::container::small_vector<WriteObserver, 2> observers;
    };

    static constexpr size_t ADDRESS_BITS = 40;
    static constexpr size_t NUM_ADDRESS_PAGES = 1ULL << (40 - PM_PAGE_BITS);
    static constexpr size_t NUM_ADDRESS_LOCKS = NUM_ADDRESS_PAGES / PAGES_PER_LOCK;
    inline static Vulkan::Rasterizer* rasterizer;

    template <s32 delta, bool is_read>
    u8 ApplyPageDelta(size_t page_index, PageState& state) {
        if constexpr (is_read) {
            std::scoped_lock lock{read_watch_mutex};
            if constexpr (delta == 1) {
                auto& ref = read_watch_refcounts[page_index];
                ++ref;
                if (ref == 1) {
                    state.num_read_watchers = 1;
                    return 1;
                }
                return static_cast<u8>(std::min<u16>(ref, 255));
            } else if constexpr (delta == -1) {
                auto it = read_watch_refcounts.find(page_index);
                if (it != read_watch_refcounts.end()) {
                    if (it.value() > 1) {
                        --it.value();
                        return static_cast<u8>(std::min<u16>(it.value(), 255));
                    }
                    read_watch_refcounts.erase(it);
                }
                state.num_read_watchers = 0;
                return 0;
            } else {
                auto it = read_watch_refcounts.find(page_index);
                return it != read_watch_refcounts.end() ? static_cast<u8>(std::min<u16>(it.value(), 255)) : 0;
            }
        } else {
            return state.AddDelta<delta, false>();
        }
    }

#ifdef ENABLE_USERFAULTFD
    Impl(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;
        uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        ASSERT_MSG(uffd != -1, "{}", Common::GetLastErrorMsg());

        // Request uffdio features from kernel.
        uffdio_api api;
        api.api = UFFD_API;
        api.features = UFFD_FEATURE_THREAD_ID;
        const int ret = ioctl(uffd, UFFDIO_API, &api);
        ASSERT(ret == 0 && api.api == UFFD_API);

        // Create uffd handler thread
        ufd_thread = std::jthread([&](std::stop_token token) { UffdHandler(token); });
    }

    void OnMap(VAddr address, size_t size) {
        uffdio_register reg;
        reg.range.start = address;
        reg.range.len = size;
        reg.mode = UFFDIO_REGISTER_MODE_WP;
        const int ret = ioctl(uffd, UFFDIO_REGISTER, &reg);
        ASSERT_MSG(ret != -1, "Uffdio register failed");
    }

    void OnUnmap(VAddr address, size_t size) {
        uffdio_range range;
        range.start = address;
        range.len = size;
        const int ret = ioctl(uffd, UFFDIO_UNREGISTER, &range);
        ASSERT_MSG(ret != -1, "Uffdio unregister failed");
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) {
        bool allow_write = True(perms & Core::MemoryPermission::Write);
        uffdio_writeprotect wp;
        wp.range.start = address;
        wp.range.len = size;
        wp.mode = allow_write ? 0 : UFFDIO_WRITEPROTECT_MODE_WP;
        const int ret = ioctl(uffd, UFFDIO_WRITEPROTECT, &wp);
        ASSERT_MSG(ret != -1, "Uffdio writeprotect failed with error: {}",
                   Common::GetLastErrorMsg());
    }

    void UffdHandler(std::stop_token token) {
        while (!token.stop_requested()) {
            pollfd pollfd;
            pollfd.fd = uffd;
            pollfd.events = POLLIN;

            // Block until the descriptor is ready for data reads.
            const int pollres = poll(&pollfd, 1, -1);
            switch (pollres) {
            case -1:
                perror("Poll userfaultfd");
                continue;
                break;
            case 0:
                continue;
            case 1:
                break;
            default:
                UNREACHABLE_MSG("Unexpected number of descriptors {} out of poll", pollres);
            }

            // We don't want an error condition to have occured.
            ASSERT_MSG(!(pollfd.revents & POLLERR), "POLLERR on userfaultfd");

            // We waited until there is data to read, we don't care about anything else.
            if (!(pollfd.revents & POLLIN)) {
                continue;
            }

            // Read message from kernel.
            uffd_msg msg;
            const int readret = read(uffd, &msg, sizeof(msg));
            ASSERT_MSG(readret != -1 || errno == EAGAIN, "Unexpected result of uffd read");
            if (errno == EAGAIN) {
                continue;
            }
            ASSERT_MSG(readret == sizeof(msg), "Unexpected short read, exiting");
            ASSERT(msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP);

            // Notify rasterizer about the fault.
            const VAddr addr = msg.arg.pagefault.address;
            rasterizer->InvalidateMemory(addr, 1);
        }
    }

    std::jthread ufd_thread;
    int uffd;
#else
    Impl(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;

        // Should be called first.
        constexpr auto priority = std::numeric_limits<u32>::min();
        Core::Signals::Instance()->RegisterAccessViolationHandler(GuestFaultSignalHandler,
                                                                  priority);
    }

    void OnMap(VAddr address, size_t size) {
        // No-op
    }

    void OnUnmap(VAddr address, size_t size) {
        VideoCore::GpuAuthorityTracker::Instance().HandleUnmap(address, size);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) {
        RENDERER_TRACE;
        auto* memory = Core::Memory::Instance();
        auto& impl = memory->GetAddressSpace();
        ASSERT_MSG(perms != Core::MemoryPermission::Write,
                   "Attempted to protect region as write-only which is not a valid permission");
        impl.Protect(address, size, perms);
    }

    static bool GuestFaultSignalHandler(void* context, void* fault_address) {
        const auto addr = reinterpret_cast<VAddr>(fault_address);
        if (Common::IsWriteError(context)) {
            VideoCore::GpuAuthorityTracker::Instance().HandleCpuWrite(addr, 8);
            const bool handled_write = rasterizer->InvalidateMemory(addr, 8);
            if (handled_write) {
                rasterizer->OnCpuWriteFault(addr);
            }
            // After handling write-watchers, check if the page still has
            // semantic read-watchers. If so, disarm them to prevent a
            // livelock where the page stays at PAGE_NOACCESS because
            // num_read_watchers > 0 even though no write-watchers exist.
            const bool handled_read_watch = rasterizer->HandleWriteFaultOnReadWatchedPage(addr, 8, context);
            if (!handled_write && !handled_read_watch) {
                // Watchdog: if neither handler resolved the fault, force-unprotect the page
                // to prevent infinite re-execution of the faulting instruction.
                static thread_local VAddr last_fault_page{};
                static thread_local u32 fault_repeat_count{};
                const VAddr fault_page = addr & ~0xFFFULL;
                if (fault_page == last_fault_page) {
                    ++fault_repeat_count;
                    if (fault_repeat_count >= 16) {
                        auto* memory = Core::Memory::Instance();
                        if (memory) {
                            memory->GetAddressSpace().Protect(
                                fault_page, 4096, Core::MemoryPermission::ReadWrite);
                        }
                        fault_repeat_count = 0;
                        last_fault_page = 0;
                        return true;
                    }
                } else {
                    last_fault_page = fault_page;
                    fault_repeat_count = 1;
                }
                return false;
            }
            return true;
        } else {
            if (VideoCore::GpuAuthorityTracker::Instance().HandleCpuRead(addr, 8)) {
                return true;
            }
            return rasterizer->ReadMemory(addr, 8, context);
        }
        return false;
    }
#endif

    template <bool track, bool is_read>
    void UpdatePageWatchers(VAddr addr, u64 size) {
        RENDERER_TRACE;

        if constexpr (track && is_read) {
            // Copy workers must never touch a page after its read access is revoked.
            auto& copy_engine = GuestCopyEngine::Instance();
            copy_engine.BeginReadProtect(addr, size);
            SCOPE_EXIT {
                copy_engine.EndReadProtect(addr, size);
            };
            UpdatePageWatchersImpl<track, is_read>(addr, size);
        } else {
            UpdatePageWatchersImpl<track, is_read>(addr, size);
        }
    }

    template <bool track, bool is_read>
    void UpdatePageWatchersImpl(VAddr addr, u64 size) {
        size_t page = addr >> PM_PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PM_PAGE_SIZE);

        // Acquire locks for the range of pages
        const auto lock_start = locks.begin() + (page / PAGES_PER_LOCK);
        const auto lock_end = locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);

        auto perms = cached_pages[page].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                Protect(range_begin << PM_PAGE_BITS, range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate requested pages
        const u64 aligned_addr = page << PM_PAGE_BITS;
        const u64 aligned_end = page_end << PM_PAGE_BITS;
        if (!rasterizer->IsMapped(aligned_addr, aligned_end - aligned_addr)) {
            LOG_WARNING(Render,
                        "Tracking memory region {:#x} - {:#x} which is not fully GPU mapped.",
                        aligned_addr, aligned_end);
        }

        for (; page != page_end; ++page) {
            PageState& state = cached_pages[page];

            // Apply the change to the page state
            const u8 new_count = ApplyPageDelta<track ? 1 : -1, is_read>(page, state);

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // Only start a new range if the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current range up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    template <bool track, bool is_read>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) {
        RENDERER_TRACE;
        auto start_range = mask.FirstRange();
        auto end_range = mask.LastRange();

        if constexpr (track && is_read) {
            const VAddr begin = base_addr + (start_range.first << PM_PAGE_BITS);
            const u64 size = (end_range.second - start_range.first) << PM_PAGE_BITS;
            auto& copy_engine = GuestCopyEngine::Instance();
            copy_engine.BeginReadProtect(begin, size);
            SCOPE_EXIT {
                copy_engine.EndReadProtect(begin, size);
            };
            UpdatePageWatchersForRegionImpl<track, is_read>(base_addr, mask, start_range,
                                                            end_range);
        } else {
            UpdatePageWatchersForRegionImpl<track, is_read>(base_addr, mask, start_range,
                                                            end_range);
        }
    }

    template <bool track, bool is_read, typename Range>
    void UpdatePageWatchersForRegionImpl(VAddr base_addr, RegionBits& mask, Range start_range,
                                         Range end_range) {

        if (start_range.second == end_range.second) {
            // if all pages are contiguous, use the regular UpdatePageWatchers
            const VAddr start_addr = base_addr + (start_range.first << PM_PAGE_BITS);
            const u64 size = (start_range.second - start_range.first) << PM_PAGE_BITS;
            return UpdatePageWatchersImpl<track, is_read>(start_addr, size);
        }

        size_t base_page = (base_addr >> PM_PAGE_BITS);
        ASSERT(base_page % PAGES_PER_LOCK == 0);
        std::scoped_lock lk(locks[base_page / PAGES_PER_LOCK]);
        auto perms = cached_pages[base_page + start_range.first].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                Protect((range_begin << PM_PAGE_BITS), range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate pages
        for (size_t page = start_range.first; page < end_range.second; ++page) {
            PageState& state = cached_pages[base_page + page];
            const bool update = mask.Get(page);

            // Apply the change to the page state
            const u8 new_count =
                update ? ApplyPageDelta<track ? 1 : -1, is_read>(base_page + page, state)
                       : ApplyPageDelta<0, is_read>(base_page + page, state);

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // If the page is not being updated, skip it
            if (!update) {
                continue;
            }

            // If the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = base_page + page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current range up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    void GpuMap(VAddr address, size_t size) {
        std::scoped_lock lock{mapping_mutex};
        OnMap(address, size);
        gpu_mappings += decltype(gpu_mappings)::interval_type::right_open(address, address + size);
        NotifyWrite(address, size, MemoryWriteSource::Map);
    }

    void GpuUnmap(VAddr address, size_t size) {
        std::scoped_lock lock{mapping_mutex};
        NotifyWrite(address, size, MemoryWriteSource::Unmap);
        OnUnmap(address, size);
        gpu_mappings -= decltype(gpu_mappings)::interval_type::right_open(address, address + size);
    }

    [[nodiscard]] MemoryWriteWatch ArmWriteWatch(VAddr address, MemoryWriteCallback callback,
                                                 void* user_data) {
        if (callback == nullptr) [[unlikely]] {
            return {};
        }

        const VAddr page = PageManager::GetPageAddr(address);
        std::scoped_lock mapping_lock{mapping_mutex};
        const auto page_range =
            decltype(gpu_mappings)::interval_type::right_open(page, page + PM_PAGE_SIZE);
        if (!boost::icl::contains(gpu_mappings, page_range)) [[unlikely]] {
            return {};
        }

        std::scoped_lock lock{write_watch_mutex};
        auto [it, inserted] = watched_pages.try_emplace(page);
        if (inserted) {
            UpdatePageWatchers<true, false>(page, PM_PAGE_SIZE);
        }

        u64 id = next_watch_id++;
        if (id == 0) [[unlikely]] {
            id = next_watch_id++;
        }
        it.value().observers.push_back(WriteObserver{
            .id = id,
            .callback = callback,
            .user_data = user_data,
        });
        active_write_watches.fetch_add(1, std::memory_order_release);
        return MemoryWriteWatch{
            .page = page,
            .id = id,
            .epoch = memory_epoch.load(std::memory_order_acquire),
        };
    }

    bool CancelWriteWatch(MemoryWriteWatch watch) {
        if (!watch) {
            return false;
        }

        std::scoped_lock lock{write_watch_mutex};
        const auto page_it = watched_pages.find(watch.page);
        if (page_it == watched_pages.end()) {
            return false;
        }
        auto& observers = page_it.value().observers;
        const auto observer_it = std::ranges::find(observers, watch.id, &WriteObserver::id);
        if (observer_it == observers.end()) {
            return false;
        }

        observers.erase(observer_it);
        active_write_watches.fetch_sub(1, std::memory_order_release);
        if (observers.empty()) {
            UpdatePageWatchers<false, false>(watch.page, PM_PAGE_SIZE);
            watched_pages.erase(page_it);
        }
        return true;
    }

    MemoryWriteNotifyResult NotifyWrite(VAddr address, u64 size, MemoryWriteSource source) {
        if (size == 0 || size - 1 > std::numeric_limits<VAddr>::max() - address) [[unlikely]] {
            return {};
        }

        const bool had_active_watches =
            active_write_watches.load(std::memory_order_acquire) != 0;
        if (!had_active_watches) [[likely]] {
            return {};
        }
        return NotifyWriteSlow(address, size, source);
    }

    SHAD_NO_INLINE MemoryWriteNotifyResult NotifyWriteSlow(VAddr address, u64 size,
                                                           MemoryWriteSource source) {
        // Preserve the original post-dispatch watch check. Watches can be armed or cancelled
        // between the fast-path snapshot and this slow-path entry.
        const bool had_active_watches =
            active_write_watches.load(std::memory_order_acquire) != 0;
        const VAddr first_page = PageManager::GetPageAddr(address);
        const VAddr last_page = PageManager::GetPageAddr(address + size - 1);
        const u64 page_count = ((last_page - first_page) >> PM_PAGE_BITS) + 1;

        if (!had_active_watches) [[likely]] {
            return {};
        }
        const u64 epoch = memory_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;

        std::scoped_lock lock{write_watch_mutex};
        boost::container::small_vector<VAddr, 8> pages;
        if (page_count <= pages.capacity()) {
            for (VAddr page = first_page;; page += PM_PAGE_SIZE) {
                if (watched_pages.contains(page)) {
                    pages.push_back(page);
                }
                if (page == last_page) {
                    break;
                }
            }
        } else {
            pages.reserve(watched_pages.size());
            for (const auto& [page, watched] : watched_pages) {
                if (page >= first_page && page <= last_page) {
                    pages.push_back(page);
                }
            }
        }

        u64 callback_count{};
        for (const VAddr page : pages) {
            auto page_it = watched_pages.find(page);
            if (page_it == watched_pages.end()) {
                continue;
            }
            auto observers = std::move(page_it.value().observers);
            watched_pages.erase(page_it);
            active_write_watches.fetch_sub(observers.size(), std::memory_order_release);
            UpdatePageWatchers<false, false>(page, PM_PAGE_SIZE);
            callback_count += observers.size();
            for (const auto& observer : observers) {
                observer.callback(observer.user_data, page, epoch, source);
            }
        }

        return {
            .matched_pages = static_cast<u32>(pages.size()),
            .callbacks = static_cast<u32>(callback_count),
            .had_active_watches = true,
        };
    }

    bool HasReadWatchers(VAddr address, u64 size) const noexcept {
        const size_t first = address >> PM_PAGE_BITS;
        const size_t last = std::min<size_t>((address + size - 1) >> PM_PAGE_BITS,
                                             cached_pages.size() - 1);
        for (size_t page = first; page <= last; ++page) {
            // Racy by design: callers order this read against arming through the copy engine.
            const u8 raw = std::atomic_ref<u8>{const_cast<u8&>(
                               reinterpret_cast<const u8&>(cached_pages[page]))}
                               .load(std::memory_order_relaxed);
            if (std::bit_cast<PageState>(raw).num_read_watchers != 0) {
                return true;
            }
        }
        return false;
    }

    bool HasReadWatcher(VAddr address) const {
        const size_t page = address >> PM_PAGE_BITS;
        if (page >= cached_pages.size()) return false;
        std::scoped_lock lock{read_watch_mutex};
        return cached_pages[page].num_read_watchers > 0;
    }

    void TemporarilyUnprotect(VAddr address, u64 size) {
        const size_t page = address >> PM_PAGE_BITS;
        const VAddr page_addr = page << PM_PAGE_BITS;
        Core::MemoryPermission perms = Core::MemoryPermission::ReadWrite;
        if (page < cached_pages.size() && cached_pages[page].num_write_watchers > 0) {
            perms = Core::MemoryPermission::Read;
        }
        Protect(page_addr, 4096, perms);
    }

    std::array<PageState, NUM_ADDRESS_PAGES> cached_pages{};
#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
    using LockType = Common::AdaptiveMutex;
#else
    using LockType = Common::SpinLock;
#endif
    std::array<LockType, NUM_ADDRESS_LOCKS> locks{};
    std::mutex mapping_mutex;
    boost::icl::interval_set<VAddr> gpu_mappings;
    mutable std::mutex read_watch_mutex;
    tsl::robin_map<size_t, u16, IntegerKeyHash> read_watch_refcounts;
    std::mutex write_watch_mutex;
    tsl::robin_map<VAddr, WatchedPage> watched_pages;
    std::atomic<u64> active_write_watches{};
    std::atomic<u64> memory_epoch{1};
    u64 next_watch_id{1};
};

PageManager::PageManager(Vulkan::Rasterizer* rasterizer_)
    : impl{std::make_unique<Impl>(rasterizer_)} {}

PageManager::~PageManager() = default;

void PageManager::OnGpuMap(VAddr address, size_t size) {
    impl->GpuMap(address, size);
}

void PageManager::OnGpuUnmap(VAddr address, size_t size) {
    impl->GpuUnmap(address, size);
}

MemoryWriteWatch PageManager::ArmWriteWatch(VAddr address, MemoryWriteCallback callback,
                                            void* user_data) {
    return impl->ArmWriteWatch(address, callback, user_data);
}

bool PageManager::CancelWriteWatch(MemoryWriteWatch watch) {
    return impl->CancelWriteWatch(watch);
}

MemoryWriteNotifyResult PageManager::NotifyWrite(VAddr address, u64 size,
                                                 MemoryWriteSource source) {
    return impl->NotifyWrite(address, size, source);
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchers(VAddr addr, u64 size) const {
    impl->UpdatePageWatchers<track, is_read>(addr, size);
}

bool PageManager::HasReadWatcher(VAddr address) const {
    return impl->HasReadWatcher(address);
}

bool PageManager::HasReadWatchers(VAddr address, u64 size) const noexcept {
    return impl->HasReadWatchers(address, size);
}

void PageManager::TemporarilyUnprotect(VAddr address, u64 size) const {
    impl->TemporarilyUnprotect(address, size);
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const {
    impl->UpdatePageWatchersForRegion<track, is_read>(base_addr, mask);
}

template void PageManager::UpdatePageWatchers<true, true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<true, false>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<false, true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<false, false>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchersForRegion<true, true>(VAddr base_addr,
                                                                   RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<true, false>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, true>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, false>(VAddr base_addr,
                                                                     RegionBits& mask) const;

} // namespace VideoCore
