// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <common/assert.h>
#include "common/types.h"
#include "core/libraries/kernel/kernel.h"
#include "net.h"
#include "net_error.h"
#include "sockets.h"

namespace Libraries::Net {

int P2PSocket::Close() {
    std::scoped_lock lock(m_mutex);
    is_bound = is_listening = is_connected = false;
    recv_queue.clear();
    LOG_INFO(Lib_Net, "P2PSocket::Close: dummy socket closed");
    return 0;
}

int P2PSocket::SetSocketOptions(int level, int optname, const void* optval, u32 optlen) {
    LOG_INFO(Lib_Net, "P2PSocket::SetSocketOptions: level=%d, opt=%d (ignored)", level, optname);
    return 0;
}

int P2PSocket::GetSocketOptions(int level, int optname, void* optval, u32* optlen) {
    LOG_INFO(Lib_Net, "P2PSocket::GetSocketOptions: level=%d, opt=%d (dummy zero)", level, optname);
    if (optval && optlen && *optlen >= sizeof(int)) {
        *reinterpret_cast<int*>(optval) = 0;
        *optlen = sizeof(int);
        return 0;
    }
    return -1;
}

int P2PSocket::Bind(const OrbisNetSockaddr* addr, u32 addrlen) {
    std::scoped_lock lock(m_mutex);
    if (!addr || addrlen < sizeof(OrbisNetSockaddr))
        return -1;
    std::memcpy(&bound_addr, addr, sizeof(OrbisNetSockaddr));
    is_bound = true;
    LOG_INFO(Lib_Net, "P2PSocket::Bind: dummy socket bound");
    return 0;
}

int P2PSocket::Listen(int backlog) {
    std::scoped_lock lock(m_mutex);
    if (!is_bound)
        return -1;
    is_listening = true;
    LOG_INFO(Lib_Net, "P2PSocket::Listen: dummy socket listening");
    return 0;
}

int P2PSocket::SendMessage(const OrbisNetMsghdr* msg, int flags) {
    LOG_ERROR(Lib_Net, "(STUBBED) called");
    *Libraries::Kernel::__Error() = ORBIS_NET_EAGAIN;
    return -1;
}

int P2PSocket::SendPacket(const void* msg, u32 len, int flags, const OrbisNetSockaddr* to,
                          u32 tolen) {
    std::scoped_lock lock(m_mutex);
    LOG_INFO(Lib_Net, "P2PSocket::SendPacket: sent %u bytes (dummy, discarded)", len);
    *Libraries::Kernel::__Error() = ORBIS_NET_EAGAIN;
    return len; // simulate success
}

int P2PSocket::ReceiveMessage(OrbisNetMsghdr* msg, int flags) {
    LOG_ERROR(Lib_Net, "(STUBBED) called");
    *Libraries::Kernel::__Error() = ORBIS_NET_EAGAIN;
    return -1;
}

int P2PSocket::ReceivePacket(void* buf, u32 len, int flags, OrbisNetSockaddr* from, u32* fromlen) {
    std::scoped_lock lock(m_mutex);
    if (recv_queue.empty())
        *Libraries::Kernel::__Error() = ORBIS_NET_EAGAIN;
    return -1; // simulate no data available

    auto& packet = recv_queue.front();
    u32 copy_len = std::min(len, static_cast<u32>(packet.size()));
    std::memcpy(buf, packet.data(), copy_len);
    if (from && fromlen && *fromlen >= sizeof(OrbisNetSockaddr)) {
        std::memcpy(from, &peer_addr, sizeof(OrbisNetSockaddr));
        *fromlen = sizeof(OrbisNetSockaddr);
    }
    recv_queue.erase(recv_queue.begin());
    return copy_len;
}

SocketPtr P2PSocket::Accept(OrbisNetSockaddr* addr, u32* addrlen) {
    std::scoped_lock lock(m_mutex);
    if (!is_listening)
        *Libraries::Kernel::__Error() = ORBIS_NET_EAGAIN;
    return nullptr;

    auto new_socket = std::make_shared<P2PSocket>(0, 0, 0);
    new_socket->is_connected = true;

    if (addr && addrlen && *addrlen >= sizeof(OrbisNetSockaddr)) {
        std::memcpy(addr, &peer_addr, sizeof(OrbisNetSockaddr));
        *addrlen = sizeof(OrbisNetSockaddr);
    }

    LOG_INFO(Lib_Net, "P2PSocket::Accept: dummy socket accepted");
    return new_socket;
}

int P2PSocket::Connect(const OrbisNetSockaddr* addr, u32 namelen) {
    std::scoped_lock lock(m_mutex);
    if (!addr || namelen < sizeof(OrbisNetSockaddr))
        return -1;
    std::memcpy(&peer_addr, addr, sizeof(OrbisNetSockaddr));
    is_connected = true;
    LOG_INFO(Lib_Net, "P2PSocket::Connect: dummy connection established");
    return 0;
}

int P2PSocket::GetSocketAddress(OrbisNetSockaddr* name, u32* namelen) {
    std::scoped_lock lock(m_mutex);
    if (!name || !namelen || *namelen < sizeof(OrbisNetSockaddr))
        return -1;
    std::memcpy(name, &bound_addr, sizeof(OrbisNetSockaddr));
    *namelen = sizeof(OrbisNetSockaddr);
    return 0;
}

int P2PSocket::GetPeerName(OrbisNetSockaddr* addr, u32* namelen) {
    LOG_ERROR(Lib_Net, "(STUBBED) called");
    return 0;
}
int P2PSocket::fstat(Libraries::Kernel::OrbisKernelStat* stat) {
    LOG_ERROR(Lib_Net, "(STUBBED) called");
    return 0;
}
} // namespace Libraries::Net