// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <boost/container/small_vector.hpp>
#include "common/assert.h"
#include "common/debug.h"
#include "common/signal_context.h"
#include "common/spin_lock.h"
#include "core/memory.h"
#include "core/signals.h"
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
#endif

#ifdef __linux__
#include "common/adaptive_mutex.h"
#else
#include "common/spin_lock.h"
#endif

namespace VideoCore {

constexpr size_t PAGE_SIZE = 4_KB;
constexpr size_t PAGE_BITS = 12;

struct PageManager::Impl {
    struct PageState {
        u8 watch_flags{};

        static constexpr u8 READ_COUNT_MASK = 0x0F;  // bits 0-3
        static constexpr u8 WRITE_COUNT_MASK = 0xF0; // bits 4-7

        u8 GetReadCount() const noexcept {
            return watch_flags & READ_COUNT_MASK;
        }
        u8 GetWriteCount() const noexcept {
            return (watch_flags & WRITE_COUNT_MASK) >> 4;
        }

        void IncrementReadCount() {
            auto count = GetReadCount();
            if (count < 15) {
                ++count;
                watch_flags = (watch_flags & WRITE_COUNT_MASK) | count;
            } else {
                // handle overflow
            }
        }

        void DecrementReadCount() {
            auto count = GetReadCount();
            if (count > 0) {
                --count;
                watch_flags = (watch_flags & WRITE_COUNT_MASK) | count;
            } else {
                // error
            }
        }

        // Similarly for write count:

        void IncrementWriteCount() {
            auto count = GetWriteCount();
            if (count < 15) {
                ++count;
                watch_flags = (watch_flags & READ_COUNT_MASK) | (count << 4);
            } else {
                // handle overflow
            }
        }

        void DecrementWriteCount() {
            auto count = GetWriteCount();
            if (count > 0) {
                --count;
                watch_flags = (watch_flags & READ_COUNT_MASK) | (count << 4);
            } else {
                // error
            }
        }

    template <bool is_read, s32 delta>
        u8 AddDelta() noexcept {
            static_assert(delta == 1 || delta == -1, "Delta must be +1 or -1");

            u8 read_count = GetReadCount();
            u8 write_count = GetWriteCount();

            if constexpr (is_read) {
                s32 new_count = static_cast<s32>(read_count) + delta;
                ASSERT_MSG(new_count >= 0 && new_count <= 15, "Invalid read watcher count");
                read_count = static_cast<u8>(new_count);
            } else {
                s32 new_count = static_cast<s32>(write_count) + delta;
                ASSERT_MSG(new_count >= 0 && new_count <= 15, "Invalid write watcher count");
                write_count = static_cast<u8>(new_count);
            }

            watch_flags = (write_count << 4) | read_count;

            return watch_flags;
        }

        Core::MemoryPermission WritePerm() const noexcept {
            return GetWriteCount() == 0 ? Core::MemoryPermission::Write
                                        : Core::MemoryPermission::None;
        }

        Core::MemoryPermission ReadPerm() const noexcept {
            return GetReadCount() == 0 ? Core::MemoryPermission::Read
                                       : Core::MemoryPermission::None;
        }

        Core::MemoryPermission Perms() const noexcept {
            return ReadPerm() | WritePerm();
        }
    };

    struct UpdateProtectRange {
        VAddr addr;
        u64 size;
        Core::MemoryPermission perms;
    };

    static constexpr size_t ADDRESS_BITS = 40;
    static constexpr size_t NUM_ADDRESS_PAGES = 1ULL << (40 - PAGE_BITS);
    inline static Vulkan::Rasterizer* rasterizer;
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
        // No-op
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) {
        auto* memory = Core::Memory::Instance();
        auto& impl = memory->GetAddressSpace();
        impl.Protect(address, size, perms);
    }

    static bool GuestFaultSignalHandler(void* context, void* fault_address) {
        const auto addr = reinterpret_cast<VAddr>(fault_address);
        if (Common::IsWriteError(context)) {
            return rasterizer->InvalidateMemory(addr, 1);
        } else {
            return rasterizer->ReadMemory(addr, 1);
        }
        return false;
    }

#endif
    template <s32 delta, bool is_read>
    void UpdatePageWatchers(VAddr addr, u64 size) {
        boost::container::small_vector<UpdateProtectRange, 16> update_ranges;

        size_t page = addr >> PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PAGE_SIZE);

        Core::MemoryPermission perms = Core::MemoryPermission::ReadWrite; // default

        u64 range_begin = 0;
        u64 range_bytes = 0;

        auto release_pending = [&]() {
            if (range_bytes > 0) {
                update_ranges.push_back({range_begin << PAGE_BITS, range_bytes, perms});
                range_bytes = 0;
            }
        };

for (; page != page_end; ++page) {
            PageState old_state;
            {
                std::scoped_lock lk(lock);
                PageState& state = cached_pages[page];
                old_state = state; // copy old state

                const auto old_perms = state.Perms();
                u8 old_watch_flags = state.watch_flags;

                u8 new_watch_flags = state.AddDelta<is_read, delta>();

                const auto new_perms = state.Perms();

                if (new_perms != old_perms) {
                    release_pending();
                    perms = new_perms;
                }

                // Extract relevant counts depending on is_read
                u8 old_count = 0;
                u8 new_count = 0;
                if constexpr (is_read) {
                    old_count = old_watch_flags & PageState::READ_COUNT_MASK;
                    new_count = new_watch_flags & PageState::READ_COUNT_MASK;
                } else {
                    old_count = (old_watch_flags & PageState::WRITE_COUNT_MASK) >> 4;
                    new_count = (new_watch_flags & PageState::WRITE_COUNT_MASK) >> 4;
                }

                // Check if watcher count changed from 0 to 1 or 1 to 0
                if ((new_count == 0 && old_count == 1) || (new_count == 1 && old_count == 0)) {
                    if (range_bytes == 0) {
                        range_begin = page;
                    }
                    range_bytes += PAGE_SIZE;
                } else {
                    release_pending();
                }
            }
        }
        release_pending();

        // Flush deferred protects outside lock
        for (const auto& range : update_ranges) {
            Protect(range.addr, range.size, range.perms);
        }
    }

    std::array<PageState, NUM_ADDRESS_PAGES> cached_pages{};
#ifdef __linux__
    Common::AdaptiveMutex lock;
#else
    Common::SpinLock lock;
#endif
};

PageManager::PageManager(Vulkan::Rasterizer* rasterizer_)
    : impl{std::make_unique<Impl>(rasterizer_)} {}

PageManager::~PageManager() = default;

void PageManager::OnGpuMap(VAddr address, size_t size) {
    impl->OnMap(address, size);
}

void PageManager::OnGpuUnmap(VAddr address, size_t size) {
    impl->OnUnmap(address, size);
}

template <s32 delta, bool is_read>
void PageManager::UpdatePageWatchers(VAddr addr, u64 size) const {
    impl->UpdatePageWatchers<delta, is_read>(addr, size);
}

template void PageManager::UpdatePageWatchers<1, true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<1, false>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<-1, true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<-1, false>(VAddr addr, u64 size) const;

} // namespace VideoCore