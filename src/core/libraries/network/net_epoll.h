// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/libraries/network/net.h"

#include <deque>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <wepoll.h>
#endif

#if defined(__linux__) || defined(__APPLE__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

namespace Libraries::Net {

#ifdef _WIN32
using epoll_handle = HANDLE;
#else
using epoll_handle = int;
#endif

struct Epoll {
    std::vector<std::pair<u32 /*netId*/, OrbisNetEpollEvent>> events{};
    std::string name;
    epoll_handle epoll_fd;
    std::deque<u32> async_resolutions{};
    int abort_fd{-1}; // eventfd for sceNetEpollAbort wakeup

    Epoll(const Epoll&) = delete;
    Epoll& operator=(const Epoll&) = delete;

    Epoll(Epoll&& other) noexcept
        : events(std::move(other.events)), name(std::move(other.name)), epoll_fd(other.epoll_fd),
          async_resolutions(std::move(other.async_resolutions)), abort_fd(other.abort_fd),
          destroyed(other.destroyed) {
#ifdef _WIN32
        other.epoll_fd = nullptr;
#else
        other.epoll_fd = -1;
#endif
        other.abort_fd = -1;
        other.destroyed = true;
    }

    Epoll& operator=(Epoll&& other) noexcept {
        if (this != &other) {
            Destroy();
            events = std::move(other.events);
            name = std::move(other.name);
            epoll_fd = other.epoll_fd;
            async_resolutions = std::move(other.async_resolutions);
            abort_fd = other.abort_fd;
            destroyed = other.destroyed;
#ifdef _WIN32
            other.epoll_fd = nullptr;
#else
            other.epoll_fd = -1;
#endif
            other.abort_fd = -1;
            other.destroyed = true;
        }
        return *this;
    }

    explicit Epoll(const char* name_) : name(name_), epoll_fd(epoll_create1(0)) {
#ifdef _WIN32
        ASSERT(epoll_fd != nullptr);
#else
        ASSERT(epoll_fd != -1);

        // Create eventfd for abort signaling
        abort_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (abort_fd >= 0) {
            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.fd = abort_fd;
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, abort_fd, &ev);
        }
#endif
        if (name_ == nullptr) {
            name = "anon";
        }
    }

    // Signal abort to wake any thread blocked in epoll_wait
    void Abort() {
#ifndef _WIN32
        if (abort_fd >= 0) {
            uint64_t val = 1;
            ::write(abort_fd, &val, sizeof(val));
        }
#endif
    }

    // Drain the abort eventfd (call after epoll_wait returns)
    void DrainAbort() {
#ifndef _WIN32
        if (abort_fd >= 0) {
            uint64_t val;
            ::read(abort_fd, &val, sizeof(val));
        }
#endif
    }

    bool Destroyed() const noexcept {
        return destroyed;
    }

    void Destroy() noexcept {
        events.clear();
#ifdef _WIN32
        epoll_close(epoll_fd);
        epoll_fd = nullptr;
#else
        if (abort_fd >= 0) {
            close(abort_fd);
            abort_fd = -1;
        }
        close(epoll_fd);
        epoll_fd = -1;
#endif
        name = "";
        destroyed = true;
    }

private:
    bool destroyed{};
};

u32 ConvertEpollEventsIn(u32 orbis_events);
u32 ConvertEpollEventsOut(u32 epoll_events);

class EpollTable {
public:
    EpollTable() = default;
    virtual ~EpollTable() = default;

    int CreateHandle(const char* name);
    void DeleteHandle(int d);
    Epoll* GetEpoll(int d);

private:
    std::vector<Epoll> epolls;
    std::mutex m_mutex;
};

} // namespace Libraries::Net