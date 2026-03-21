// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <common/assert.h>
#include <common/logging/log.h>
#include <queue>
#include "core/libraries/kernel/file_system.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/np/np_signaling.h"
#include "net.h"
#include "net_error.h"
#include "sockets.h"

#ifndef _WIN32
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace Libraries::Net {

// PS4 P2P sockets multiplex multiple virtual ports (sin_vport) over a single UDP port.
// We emulate this with one real UDP socket (the "shared transport") bound to the signaling
// port. All P2PSocket instances share this socket. A 4-byte VPORT header is prepended to
// each outgoing packet:
//   [u16 src_vport (NBO)][u16 dst_vport (NBO)][payload...]
// On receive, we drain the shared socket and demultiplex packets into per-vport queues
// based on the dst_vport field.
static constexpr size_t VPORT_HEADER_SIZE = 4;
static constexpr u16 DEFAULT_SIGNALING_PORT = 9307;

#ifdef _WIN32
#define P2P_ERROR_CASE(errname)                                                                    \
    case (WSA##errname):                                                                           \
        *Libraries::Kernel::__Error() = ORBIS_NET_##errname;                                       \
        return -1;
#else
#define P2P_ERROR_CASE(errname)                                                                    \
    case (errname):                                                                                \
        *Libraries::Kernel::__Error() = ORBIS_NET_##errname;                                       \
        return -1;
#endif

static int ConvertReturnErrorCode(int retval) {
    if (retval < 0) {
#ifdef _WIN32
        switch (WSAGetLastError()) {
#else
        switch (errno) {
#endif
#ifndef _WIN32
            P2P_ERROR_CASE(EPERM)
            P2P_ERROR_CASE(ENOENT)
            P2P_ERROR_CASE(ENOMEM)
            P2P_ERROR_CASE(EEXIST)
            P2P_ERROR_CASE(ENODEV)
            P2P_ERROR_CASE(ENFILE)
            P2P_ERROR_CASE(ENOSPC)
            P2P_ERROR_CASE(EPIPE)
            P2P_ERROR_CASE(ECANCELED)
            P2P_ERROR_CASE(ENODATA)
#endif
            P2P_ERROR_CASE(EINTR)
            P2P_ERROR_CASE(EBADF)
            P2P_ERROR_CASE(EACCES)
            P2P_ERROR_CASE(EFAULT)
            P2P_ERROR_CASE(EINVAL)
            P2P_ERROR_CASE(EMFILE)
            P2P_ERROR_CASE(EWOULDBLOCK)
            P2P_ERROR_CASE(EINPROGRESS)
            P2P_ERROR_CASE(EALREADY)
            P2P_ERROR_CASE(ENOTSOCK)
            P2P_ERROR_CASE(EDESTADDRREQ)
            P2P_ERROR_CASE(EMSGSIZE)
            P2P_ERROR_CASE(EPROTOTYPE)
            P2P_ERROR_CASE(ENOPROTOOPT)
            P2P_ERROR_CASE(EPROTONOSUPPORT)
#if defined(__APPLE__) || defined(_WIN32)
            P2P_ERROR_CASE(EOPNOTSUPP)
#endif
            P2P_ERROR_CASE(EAFNOSUPPORT)
            P2P_ERROR_CASE(EADDRINUSE)
            P2P_ERROR_CASE(EADDRNOTAVAIL)
            P2P_ERROR_CASE(ENETDOWN)
            P2P_ERROR_CASE(ENETUNREACH)
            P2P_ERROR_CASE(ENETRESET)
            P2P_ERROR_CASE(ECONNABORTED)
            P2P_ERROR_CASE(ECONNRESET)
            P2P_ERROR_CASE(ENOBUFS)
            P2P_ERROR_CASE(EISCONN)
            P2P_ERROR_CASE(ENOTCONN)
            P2P_ERROR_CASE(ETIMEDOUT)
            P2P_ERROR_CASE(ECONNREFUSED)
            P2P_ERROR_CASE(ELOOP)
            P2P_ERROR_CASE(ENAMETOOLONG)
            P2P_ERROR_CASE(EHOSTUNREACH)
            P2P_ERROR_CASE(ENOTEMPTY)
        }
        *Libraries::Kernel::__Error() = ORBIS_NET_EINTERNAL;
        return -1;
    }
    return retval;
}

// ====== Shared P2P Transport ======
// One real UDP socket per process, shared by all P2PSocket instances.
// Packets are demultiplexed by the dst_vport field in the VPORT header.
namespace {

struct BufferedPacket {
    std::vector<u8> payload;
    sockaddr_in from_addr;
    u16 src_vport;
};

struct SharedTransport {
    net_socket fd =
#ifdef _WIN32
        INVALID_SOCKET;
#else
        -1;
#endif
    u16 bound_port_nbo = 0;
    int refcount = 0;
    std::mutex mutex;
    std::map<u16, std::queue<BufferedPacket>> vport_queues;

    bool IsValid() const {
#ifdef _WIN32
        return fd != INVALID_SOCKET;
#else
        return fd >= 0;
#endif
    }

    // Drain all available packets from the shared socket into per-vport queues.
    // Must be called with mutex held.
    void Drain() {
        if (!IsValid())
            return;
        u8 buf[65536];
        for (;;) {
            sockaddr_in from{};
            socklen_t fromlen = sizeof(from);
            int n = ::recvfrom(fd, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromlen);
            if (n <= 0)
                break;
            if (n < static_cast<int>(VPORT_HEADER_SIZE)) {
                LOG_WARNING(Lib_Net, "P2P: runt packet ({} bytes), dropping", n);
                continue;
            }

            u16 src_vp, dst_vp;
            memcpy(&src_vp, &buf[0], 2);
            memcpy(&dst_vp, &buf[2], 2);

            int plen = n - static_cast<int>(VPORT_HEADER_SIZE);
            BufferedPacket pkt;
            pkt.payload.assign(&buf[VPORT_HEADER_SIZE], &buf[VPORT_HEADER_SIZE] + plen);
            pkt.from_addr = from;
            pkt.src_vport = src_vp;

            LOG_INFO(Lib_Net, "P2P drain: {} bytes from {:#x}:{} src_vp={} dst_vp={}", plen,
                     ntohl(from.sin_addr.s_addr), ntohs(from.sin_port), ntohs(src_vp),
                     ntohs(dst_vp));

            vport_queues[dst_vp].push(std::move(pkt));
        }
    }
};

static SharedTransport s_transport;

static u16 GetSignalingPort() {
    const char* env = std::getenv("SHADPS4_SIGNALING_PORT");
    if (env) {
        int port = std::atoi(env);
        if (port > 0 && port < 65536)
            return static_cast<u16>(port);
    }
    return DEFAULT_SIGNALING_PORT;
}

// Initialize the shared transport if not already done.
// Must be called with s_transport.mutex held.
static bool EnsureTransport() {
    if (s_transport.IsValid())
        return true;

    net_socket fd = ::socket(AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
    if (fd == INVALID_SOCKET) {
#else
    if (fd < 0) {
#endif
        LOG_ERROR(Lib_Net, "P2P: failed to create shared UDP socket");
        return false;
    }

    // Allow port reuse for restart scenarios
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef SO_REUSEPORT
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&one), sizeof(one));
#endif

    u16 sig_port = GetSignalingPort();
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(sig_port);
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
        LOG_ERROR(Lib_Net, "P2P: failed to bind shared socket to port {}, trying ephemeral",
                  sig_port);
        bind_addr.sin_port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
            LOG_ERROR(Lib_Net, "P2P: failed to bind shared socket to ephemeral port");
#ifdef _WIN32
            closesocket(fd);
#else
            ::close(fd);
#endif
            return false;
        }
    }

    // Read back actual bound port
    sockaddr_in actual{};
    socklen_t actual_len = sizeof(actual);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &actual_len);

    // Shared socket is always non-blocking — individual P2PSocket handles blocking semantics
    int nb = 1;
#ifdef _WIN32
    ioctlsocket(fd, FIONBIO, (u_long*)&nb);
#else
    ioctl(fd, FIONBIO, &nb);
#endif

    s_transport.fd = fd;
    s_transport.bound_port_nbo = actual.sin_port;

    LOG_INFO(Lib_Net, "P2P shared transport: bound to port {} (fd={})",
             ntohs(s_transport.bound_port_nbo), fd);
    return true;
}

} // anonymous namespace

// ====== P2PSocket implementation ======

P2PSocket::P2PSocket(int domain, int type, int protocol) : Socket(domain, type, protocol) {
    // Initialize shared transport eagerly so IsValid()/Native() work immediately.
    // sys_socketex checks IsValid() right after construction and rejects if false.
    std::scoped_lock tlock{s_transport.mutex};
    if (EnsureTransport()) {
        sock_ = s_transport.fd;
        s_transport.refcount++;
        LOG_INFO(Lib_Net, "P2P socket handle created (shared fd={}, refcount={})", sock_,
                 s_transport.refcount);
    } else {
#ifdef _WIN32
        sock_ = INVALID_SOCKET;
#else
        sock_ = -1;
#endif
        LOG_ERROR(Lib_Net, "P2P socket handle creation failed (shared transport unavailable)");
    }
}

bool P2PSocket::IsValid() const {
#ifdef _WIN32
    return sock_ != INVALID_SOCKET;
#else
    return sock_ >= 0;
#endif
}

int P2PSocket::Close() {
    std::scoped_lock lock{m_mutex};
    if (!IsValid())
        return 0;

    LOG_INFO(Lib_Net, "Closing P2P socket (vport={})", ntohs(bound_vport_));

    {
        std::scoped_lock tlock{s_transport.mutex};
        s_transport.vport_queues.erase(bound_vport_);
        s_transport.refcount--;
        if (s_transport.refcount <= 0 && s_transport.IsValid()) {
            LOG_INFO(Lib_Net, "P2P shared transport: closing (fd={})", s_transport.fd);
#ifdef _WIN32
            closesocket(s_transport.fd);
            s_transport.fd = INVALID_SOCKET;
#else
            ::close(s_transport.fd);
            s_transport.fd = -1;
#endif
            s_transport.bound_port_nbo = 0;
            s_transport.refcount = 0;
            s_transport.vport_queues.clear();
        }
    }

#ifdef _WIN32
    sock_ = INVALID_SOCKET;
#else
    sock_ = -1;
#endif
    bound_vport_ = 0;
    return 0;
}

int P2PSocket::Bind(const OrbisNetSockaddr* addr, u32 addrlen) {
    std::scoped_lock lock{m_mutex};

    if (!IsValid()) {
        *Libraries::Kernel::__Error() = ORBIS_NET_EBADF;
        return -1;
    }

    const auto* orbis_addr = reinterpret_cast<const OrbisNetSockaddrIn*>(addr);
    bound_vport_ = orbis_addr->sin_vport;

    LOG_INFO(Lib_Net, "P2P bind: game requested port={} vport={}, using shared signaling transport",
             ntohs(orbis_addr->sin_port), ntohs(bound_vport_));

    {
        std::scoped_lock tlock{s_transport.mutex};
        s_transport.vport_queues[bound_vport_]; // create receive queue for this vport
    }

    LOG_INFO(Lib_Net, "P2P socket bound: native port={} vport={}",
             ntohs(s_transport.bound_port_nbo), ntohs(bound_vport_));
    return 0;
}

int P2PSocket::SendPacket(const void* msg, u32 len, int flags, const OrbisNetSockaddr* to,
                          u32 tolen) {
    std::scoped_lock lock{m_mutex};
    if (!IsValid()) {
        *Libraries::Kernel::__Error() = ORBIS_NET_EBADF;
        return -1;
    }
    if (to == nullptr) {
        *Libraries::Kernel::__Error() = ORBIS_NET_EDESTADDRREQ;
        return -1;
    }

    const auto* orbis_to = reinterpret_cast<const OrbisNetSockaddrIn*>(to);

    // On real PS4, P2P sockets auto-resolve addr=0/port=0 to the signaling peer.
    // When the game sends to 0.0.0.0:0, look up the active peer from NpSignaling.
    OrbisNetSockaddrIn resolved_to = *orbis_to;
    if (orbis_to->sin_addr == 0 && orbis_to->sin_port == 0) {
        auto peer = Libraries::Np::NpSignaling::GetAnyActivePeer();
        if (peer.addr != 0 && peer.port != 0) {
            resolved_to.sin_addr = peer.addr;
            resolved_to.sin_port = peer.port;
            LOG_INFO(Lib_Net, "P2P sendto: resolved 0.0.0.0:0 ? {:#x}:{} via NpSignaling",
                     ntohl(peer.addr), ntohs(peer.port));
        }
    }
    orbis_to = &resolved_to;

    // Build framed packet: [src_vport(2)][dst_vport(2)][payload(len)]
    std::vector<u8> framed(VPORT_HEADER_SIZE + len);
    memcpy(&framed[0], &bound_vport_, sizeof(u16));
    memcpy(&framed[2], &orbis_to->sin_vport, sizeof(u16));
    memcpy(&framed[VPORT_HEADER_SIZE], msg, len);

    // Build native destination (addr + port only; vport is in the header)
    sockaddr_in native_to{};
    native_to.sin_family = AF_INET;
    native_to.sin_port = orbis_to->sin_port;
    memcpy(&native_to.sin_addr, &orbis_to->sin_addr, 4);

    int native_flags = 0;
#ifndef _WIN32
    if (flags & ORBIS_NET_MSG_DONTWAIT)
        native_flags |= MSG_DONTWAIT;
#endif

    net_socket fd;
    {
        std::scoped_lock tlock{s_transport.mutex};
        fd = s_transport.fd;
    }

    LOG_INFO(Lib_Net,
             "P2P sendto: attempting {} bytes to addr={:#x} ({}.{}.{}.{}) port={} vport={} "
             "src_vport={} fd={} native_flags={:#x}",
             len, ntohl(orbis_to->sin_addr), (ntohl(orbis_to->sin_addr) >> 24) & 0xff,
             (ntohl(orbis_to->sin_addr) >> 16) & 0xff, (ntohl(orbis_to->sin_addr) >> 8) & 0xff,
             ntohl(orbis_to->sin_addr) & 0xff, ntohs(orbis_to->sin_port),
             ntohs(orbis_to->sin_vport), ntohs(bound_vport_), fd, native_flags);

    int res = ::sendto(fd, reinterpret_cast<const char*>(framed.data()), framed.size(),
                       native_flags, reinterpret_cast<sockaddr*>(&native_to), sizeof(native_to));
    if (res < 0) {
        int err = errno;
        LOG_ERROR(Lib_Net,
                  "P2P sendto FAILED: errno={} ({}) dst_addr={:#x} ({}.{}.{}.{}) "
                  "dst_port={} dst_vport={} src_vport={} len={} fd={}",
                  err, strerror(err), ntohl(orbis_to->sin_addr),
                  (ntohl(orbis_to->sin_addr) >> 24) & 0xff,
                  (ntohl(orbis_to->sin_addr) >> 16) & 0xff, (ntohl(orbis_to->sin_addr) >> 8) & 0xff,
                  ntohl(orbis_to->sin_addr) & 0xff, ntohs(orbis_to->sin_port),
                  ntohs(orbis_to->sin_vport), ntohs(bound_vport_), len, fd);
        return ConvertReturnErrorCode(res);
    }

    // Return payload bytes sent (excluding VPORT header)
    int payload_sent = res - static_cast<int>(VPORT_HEADER_SIZE);
    if (payload_sent < 0)
        payload_sent = 0;

    LOG_INFO(Lib_Net, "P2P sendto OK: {} bytes to {:#x}:{} vport={}", payload_sent,
             ntohl(orbis_to->sin_addr), ntohs(orbis_to->sin_port), ntohs(orbis_to->sin_vport));

    return payload_sent;
}

int P2PSocket::ReceivePacket(void* buf, u32 len, int flags, OrbisNetSockaddr* from, u32* fromlen) {
    std::scoped_lock lock{receive_mutex};
    if (!IsValid()) {
        *Libraries::Kernel::__Error() = ORBIS_NET_EBADF;
        return -1;
    }

    std::scoped_lock tlock{s_transport.mutex};

    // Drain any available packets from the shared socket into per-vport queues
    s_transport.Drain();

    // Check our vport queue
    auto it = s_transport.vport_queues.find(bound_vport_);
    if (it == s_transport.vport_queues.end() || it->second.empty()) {
        *Libraries::Kernel::__Error() = ORBIS_NET_EWOULDBLOCK;
        return -1;
    }

    auto& pkt = it->second.front();

    // Copy payload to user buffer
    int copy_len = std::min(static_cast<int>(pkt.payload.size()), static_cast<int>(len));
    memcpy(buf, pkt.payload.data(), copy_len);

    // Fill in source address with vport from the VPORT header
    if (from != nullptr) {
        auto* orbis_from = reinterpret_cast<OrbisNetSockaddrIn*>(from);
        memset(orbis_from, 0, sizeof(OrbisNetSockaddrIn));
        orbis_from->sin_len = sizeof(OrbisNetSockaddrIn);
        orbis_from->sin_family = AF_INET;
        orbis_from->sin_port = pkt.from_addr.sin_port;
        memcpy(&orbis_from->sin_addr, &pkt.from_addr.sin_addr, 4);
        orbis_from->sin_vport = pkt.src_vport;

        if (fromlen) {
            *fromlen = sizeof(OrbisNetSockaddrIn);
        }
    }

    LOG_INFO(Lib_Net, "P2P recvfrom: {} bytes from {:#x}:{} src_vp={} dst_vp={}", copy_len,
             ntohl(pkt.from_addr.sin_addr.s_addr), ntohs(pkt.from_addr.sin_port),
             ntohs(pkt.src_vport), ntohs(bound_vport_));

    if (!(flags & ORBIS_NET_MSG_PEEK)) {
        it->second.pop();
    }

    return copy_len;
}

int P2PSocket::SetSocketOptions(int level, int optname, const void* optval, u32 optlen) {
    std::scoped_lock lock{m_mutex};

    if (level == ORBIS_NET_SOL_SOCKET) {
        switch (optname) {
        case ORBIS_NET_SO_NBIO: {
            if (optlen < sizeof(int)) {
                *Libraries::Kernel::__Error() = ORBIS_NET_EINVAL;
                return -1;
            }
            memcpy(&sockopt_so_nbio_, optval, sizeof(int));
            LOG_INFO(Lib_Net, "P2P SO_NBIO = {}", sockopt_so_nbio_);
            // Shared socket is always non-blocking; we handle blocking semantics per-socket
            return 0;
        }
        case ORBIS_NET_SO_BROADCAST: {
            if (!IsValid())
                return 0;
            std::scoped_lock tlock{s_transport.mutex};
            return ConvertReturnErrorCode(
                setsockopt(s_transport.fd, SOL_SOCKET, SO_BROADCAST, (const char*)optval, optlen));
        }
        case ORBIS_NET_SO_SNDBUF: {
            if (!IsValid())
                return 0;
            std::scoped_lock tlock{s_transport.mutex};
            return ConvertReturnErrorCode(
                setsockopt(s_transport.fd, SOL_SOCKET, SO_SNDBUF, (const char*)optval, optlen));
        }
        case ORBIS_NET_SO_RCVBUF: {
            if (!IsValid())
                return 0;
            std::scoped_lock tlock{s_transport.mutex};
            return ConvertReturnErrorCode(
                setsockopt(s_transport.fd, SOL_SOCKET, SO_RCVBUF, (const char*)optval, optlen));
        }
        case ORBIS_NET_SO_REUSEADDR: {
            // Shared socket already has SO_REUSEADDR set
            LOG_INFO(Lib_Net, "P2P SO_REUSEADDR (shared transport, no-op)");
            return 0;
        }
        case ORBIS_NET_SO_SNDTIMEO:
        case ORBIS_NET_SO_RCVTIMEO: {
            // Shared socket is always non-blocking; timeouts handled per-socket if needed
            LOG_INFO(Lib_Net, "P2P SO_{} (shared transport, stored locally)",
                     (optname == ORBIS_NET_SO_SNDTIMEO) ? "SNDTIMEO" : "RCVTIMEO");
            return 0;
        }
        default:
            LOG_WARNING(Lib_Net, "P2P setsockopt: unhandled SOL_SOCKET option {:#x}", optname);
            return 0;
        }
    }

    LOG_WARNING(Lib_Net, "P2P setsockopt: unhandled level={} optname={:#x}", level, optname);
    return 0;
}

int P2PSocket::GetSocketOptions(int level, int optname, void* optval, u32* optlen) {
    std::scoped_lock lock{m_mutex};

    if (level == ORBIS_NET_SOL_SOCKET) {
        switch (optname) {
        case ORBIS_NET_SO_NBIO: {
            if (*optlen < sizeof(int)) {
                *optlen = sizeof(int);
                *Libraries::Kernel::__Error() = ORBIS_NET_EFAULT;
                return -1;
            }
            *optlen = sizeof(int);
            *(int*)optval = sockopt_so_nbio_;
            return 0;
        }
        case ORBIS_NET_SO_ERROR: {
            if (!IsValid()) {
                *(int*)optval = 0;
                *optlen = sizeof(int);
                return 0;
            }
            std::scoped_lock tlock{s_transport.mutex};
            socklen_t optlen_temp = *optlen;
            auto retval = ConvertReturnErrorCode(
                getsockopt(s_transport.fd, SOL_SOCKET, SO_ERROR, (char*)optval, &optlen_temp));
            *optlen = optlen_temp;
            return retval;
        }
        case ORBIS_NET_SO_TYPE: {
            if (*optlen >= sizeof(int)) {
                *(int*)optval = socket_type;
                *optlen = sizeof(int);
            }
            return 0;
        }
        default:
            LOG_WARNING(Lib_Net, "P2P getsockopt: unhandled SOL_SOCKET option {:#x}", optname);
            if (*optlen >= sizeof(int)) {
                *(int*)optval = 0;
                *optlen = sizeof(int);
            }
            return 0;
        }
    }

    LOG_WARNING(Lib_Net, "P2P getsockopt: unhandled level={} optname={:#x}", level, optname);
    return 0;
}

int P2PSocket::GetSocketAddress(OrbisNetSockaddr* name, u32* namelen) {
    std::scoped_lock lock{m_mutex};
    if (!IsValid() || name == nullptr) {
        return 0;
    }

    // Return the shared transport's bound address with this socket's vport
    auto* orbis_addr = reinterpret_cast<OrbisNetSockaddrIn*>(name);
    memset(orbis_addr, 0, sizeof(OrbisNetSockaddrIn));
    orbis_addr->sin_len = sizeof(OrbisNetSockaddrIn);
    orbis_addr->sin_family = AF_INET;

    {
        std::scoped_lock tlock{s_transport.mutex};
        sockaddr_in native_addr{};
        socklen_t native_len = sizeof(native_addr);
        if (::getsockname(s_transport.fd, reinterpret_cast<sockaddr*>(&native_addr), &native_len) ==
            0) {
            orbis_addr->sin_port = native_addr.sin_port;
            memcpy(&orbis_addr->sin_addr, &native_addr.sin_addr, 4);
        }
    }

    orbis_addr->sin_vport = bound_vport_;

    if (namelen) {
        *namelen = sizeof(OrbisNetSockaddrIn);
    }

    return 0;
}

int P2PSocket::Connect(const OrbisNetSockaddr* addr, u32 namelen) {
    // P2P UDP sockets don't truly connect — connectionless datagram
    LOG_INFO(Lib_Net, "P2P Connect called (no-op for UDP P2P)");
    return 0;
}

int P2PSocket::Listen(int backlog) {
    LOG_WARNING(Lib_Net, "P2P Listen called (not applicable for DGRAM)");
    return 0;
}

int P2PSocket::SendMessage(const OrbisNetMsghdr* msg, int flags) {
    LOG_ERROR(Lib_Net, "(STUBBED) P2P SendMessage called");
    *Libraries::Kernel::__Error() = ORBIS_NET_EAGAIN;
    return -1;
}

int P2PSocket::ReceiveMessage(OrbisNetMsghdr* msg, int flags) {
    LOG_ERROR(Lib_Net, "(STUBBED) P2P ReceiveMessage called");
    *Libraries::Kernel::__Error() = ORBIS_NET_EAGAIN;
    return -1;
}

SocketPtr P2PSocket::Accept(OrbisNetSockaddr* addr, u32* addrlen) {
    LOG_ERROR(Lib_Net, "P2P Accept called (not applicable for DGRAM)");
    *Libraries::Kernel::__Error() = ORBIS_NET_EOPNOTSUPP;
    return nullptr;
}

int P2PSocket::GetPeerName(OrbisNetSockaddr* addr, u32* namelen) {
    LOG_WARNING(Lib_Net, "(STUBBED) P2P GetPeerName called");
    return 0;
}

int P2PSocket::fstat(Libraries::Kernel::OrbisKernelStat* sb) {
    if (sb) {
        sb->st_mode = 0000777u | 0140000u;
    }
    return 0;
}

bool P2PSocket::HasQueuedData() {
    std::scoped_lock tlock{s_transport.mutex};
    auto it = s_transport.vport_queues.find(bound_vport_);
    return it != s_transport.vport_queues.end() && !it->second.empty();
}

void DrainP2PTransport() {
    std::scoped_lock lock{s_transport.mutex};
    s_transport.Drain();
}

} // namespace Libraries::Net
