// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstring>
#include <charconv>

#include "common/logging/log.h"
#include "core/libraries/np/np_handler.h"
#include "core/libraries/np/np_session.h"
#include "shadnet.pb.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Libraries::Np {

namespace {

#ifdef _WIN32
constexpr int kInvalidSocket = static_cast<int>(INVALID_SOCKET);
void CloseNativeSocket(int fd) {
    closesocket(fd);
}
int SendAll(int fd, const void* buf, size_t len) {
    return send(fd, reinterpret_cast<const char*>(buf), static_cast<int>(len), 0);
}
int RecvAll(int fd, void* buf, size_t len) {
    return recv(fd, reinterpret_cast<char*>(buf), static_cast<int>(len), 0);
}
#else
constexpr int kInvalidSocket = -1;
void CloseNativeSocket(int fd) {
    ::close(fd);
}
int SendAll(int fd, const void* buf, size_t len) {
    return static_cast<int>(::send(fd, buf, len, 0));
}
int RecvAll(int fd, void* buf, size_t len) {
    return static_cast<int>(::recv(fd, buf, len, 0));
}
#endif

constexpr size_t kHeaderSize = 15;
constexpr u32 kProtocolVersion = 1;
constexpr u16 kCmdLogin = 0;
constexpr u16 kCmdGetToken = 39;

enum class PacketType : u8 {
    Request = 0,
    Reply = 1,
    Notification = 2,
    ServerInfo = 3,
};

void PutU16LE(u8* p, u16 v) {
    p[0] = static_cast<u8>(v & 0xFF);
    p[1] = static_cast<u8>((v >> 8) & 0xFF);
}
void PutU32LE(u8* p, u32 v) {
    for (int i = 0; i < 4; i++)
        p[i] = static_cast<u8>((v >> (8 * i)) & 0xFF);
}
void PutU64LE(u8* p, u64 v) {
    for (int i = 0; i < 8; i++)
        p[i] = static_cast<u8>((v >> (8 * i)) & 0xFF);
}
u16 GetU16LE(const u8* p) {
    return static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8);
}
u32 GetU32LE(const u8* p) {
    u32 v = 0;
    for (int i = 0; i < 4; i++)
        v |= static_cast<u32>(p[i]) << (8 * i);
    return v;
}

// Splits "host:port"; defaults to 31313 (shadNet's unsecured TCP port) if no
// port is present or it fails to parse.
std::pair<std::string, u16> SplitHostPort(const std::string& hostPort) {
    const auto colon = hostPort.find_last_of(':');
    if (colon == std::string::npos) {
        return {hostPort, 31313};
    }
    const std::string host = hostPort.substr(0, colon);
    const std::string portStr = hostPort.substr(colon + 1);
    u16 port = 31313;
    std::from_chars(portStr.data(), portStr.data() + portStr.size(), port);
    return {host, port};
}

} // namespace

NpSession::~NpSession() {
    Disconnect();
}

void NpSession::LoginAsync(Libraries::UserService::OrbisUserServiceUserId ownerUserId,
                           std::string host, std::string npid, std::string password,
                           std::string titleId, std::string titleName) {
    Disconnect(); // stop any previous attempt for this slot first

    m_ownerUserId = ownerUserId;
    m_stopRequested.store(false, std::memory_order_release);
    m_authenticated.store(false, std::memory_order_release);

    m_thread = std::thread(&NpSession::Run, this, std::move(host), std::move(npid),
                           std::move(password), std::move(titleId), std::move(titleName));
}

void NpSession::Disconnect() {
    m_stopRequested.store(true, std::memory_order_release);
    CloseSocket();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_authenticated.store(false, std::memory_order_release);
    m_accountUserId.store(0, std::memory_order_release);
}

void NpSession::CloseSocket() {
    std::scoped_lock lk{m_socketMutex};
    if (m_sockfd != kInvalidSocket) {
        CloseNativeSocket(m_sockfd);
        m_sockfd = kInvalidSocket;
    }
}

bool NpSession::ReadExact(void* buf, size_t len) {
    u8* p = reinterpret_cast<u8*>(buf);
    size_t got = 0;
    while (got < len) {
        if (m_stopRequested.load(std::memory_order_acquire)) {
            return false;
        }
        int fd;
        {
            std::scoped_lock lk{m_socketMutex};
            fd = m_sockfd;
        }
        if (fd == kInvalidSocket) {
            return false;
        }
        const int n = RecvAll(fd, p + got, len - got);
        if (n <= 0) {
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

bool NpSession::WriteExact(const void* buf, size_t len) {
    const u8* p = reinterpret_cast<const u8*>(buf);
    size_t sent = 0;
    while (sent < len) {
        int fd;
        {
            std::scoped_lock lk{m_socketMutex};
            fd = m_sockfd;
        }
        if (fd == kInvalidSocket) {
            return false;
        }
        const int n = SendAll(fd, p + sent, len - sent);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool NpSession::ReadServerInfo() {
    u8 header[kHeaderSize];
    if (!ReadExact(header, kHeaderSize)) {
        return false;
    }
    if (static_cast<PacketType>(header[0]) != PacketType::ServerInfo) {
        LOG_ERROR(Lib_NpManager, "shadNet: expected ServerInfo packet, got type {}", header[0]);
        return false;
    }
    const u32 size = GetU32LE(header + 3);
    const size_t payloadLen = size > kHeaderSize ? size - kHeaderSize : 0;
    if (payloadLen != sizeof(u32)) {
        LOG_ERROR(Lib_NpManager, "shadNet: unexpected ServerInfo payload size {}", payloadLen);
        return false;
    }
    u8 versionBytes[4];
    if (!ReadExact(versionBytes, sizeof(versionBytes))) {
        return false;
    }
    const u32 version = GetU32LE(versionBytes);
    if (version != kProtocolVersion) {
        LOG_ERROR(Lib_NpManager, "shadNet: protocol version mismatch (server={}, client={})",
                  version, kProtocolVersion);
        return false;
    }
    return true;
}

bool NpSession::SendCommand(u16 command, const google::protobuf::MessageLite* request,
                            google::protobuf::MessageLite* replyOut) {
    std::string reqBlob;
    if (request) {
        if (!request->SerializeToString(&reqBlob)) {
            LOG_ERROR(Lib_NpManager, "shadNet: failed to serialize request for command {}",
                      command);
            return false;
        }
    }

    const bool hasBlob = request != nullptr;
    const u32 payloadLen = hasBlob ? static_cast<u32>(4 + reqBlob.size()) : 0;
    const u32 totalSize = static_cast<u32>(kHeaderSize + payloadLen);
    const u64 packetId = m_nextPacketId.fetch_add(1, std::memory_order_relaxed);

    std::string packet;
    packet.resize(totalSize);
    u8* p = reinterpret_cast<u8*>(packet.data());
    p[0] = static_cast<u8>(PacketType::Request);
    PutU16LE(p + 1, command);
    PutU32LE(p + 3, totalSize);
    PutU64LE(p + 7, packetId);
    if (hasBlob) {
        PutU32LE(p + kHeaderSize, static_cast<u32>(reqBlob.size()));
        std::memcpy(p + kHeaderSize + 4, reqBlob.data(), reqBlob.size());
    }

    if (!WriteExact(packet.data(), packet.size())) {
        return false;
    }

    // Read replies until we see the one matching our packetId. Notifications
    // (packetId == 0) can legitimately arrive interleaved and are discarded
    // here — this session doesn't act on them yet (that's matchmaking's job,
    // built on top of this connection later).
    for (;;) {
        u8 header[kHeaderSize];
        if (!ReadExact(header, kHeaderSize)) {
            return false;
        }
        const auto type = static_cast<PacketType>(header[0]);
        const u16 replyCommand = GetU16LE(header + 1);
        const u32 size = GetU32LE(header + 3);
        const size_t remaining = size > kHeaderSize ? size - kHeaderSize : 0;

        if (type == PacketType::Notification) {
            // Drain and discard.
            std::string discard(remaining, '\0');
            if (remaining > 0 && !ReadExact(discard.data(), remaining)) {
                return false;
            }
            continue;
        }

        if (type != PacketType::Reply) {
            LOG_ERROR(Lib_NpManager, "shadNet: unexpected packet type {} while awaiting reply",
                      header[0]);
            return false;
        }

        if (remaining < 1) {
            LOG_ERROR(Lib_NpManager, "shadNet: reply for command {} missing error byte",
                      replyCommand);
            return false;
        }

        std::string body(remaining, '\0');
        if (!ReadExact(body.data(), remaining)) {
            return false;
        }

        const u8 error = static_cast<u8>(body[0]);
        if (replyCommand != command) {
            // Reply to a different in-flight command than the one we sent;
            // shouldn't happen given this session only issues one command at
            // a time, but don't get stuck if it does.
            continue;
        }

        if (error != 0) {
            LOG_ERROR(Lib_NpManager, "shadNet: command {} failed with error {:#x}", command,
                      error);
            return false;
        }

        if (replyOut) {
            if (body.size() < 5) {
                LOG_ERROR(Lib_NpManager, "shadNet: reply for command {} has no body", command);
                return false;
            }
            const u32 blobLen = GetU32LE(reinterpret_cast<const u8*>(body.data()) + 1);
            if (body.size() < 5 + blobLen) {
                LOG_ERROR(Lib_NpManager, "shadNet: truncated reply body for command {}", command);
                return false;
            }
            if (!replyOut->ParseFromArray(body.data() + 5, static_cast<int>(blobLen))) {
                LOG_ERROR(Lib_NpManager, "shadNet: failed to parse reply for command {}", command);
                return false;
            }
        }
        return true;
    }
}

void NpSession::Run(std::string host, std::string npid, std::string password,
                    std::string titleId, std::string titleName) {
    if (npid.empty() || password.empty()) {
        LOG_WARNING(Lib_NpManager, "shadNet: no credentials configured for this user slot; "
                                   "skipping login");
        return;
    }

    const auto [hostname, port] = SplitHostPort(host);

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    if (getaddrinfo(hostname.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        LOG_ERROR(Lib_NpManager, "shadNet: failed to resolve {}", hostname);
        return;
    }

    int fd = kInvalidSocket;
    for (auto* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = static_cast<int>(socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (fd == kInvalidSocket) {
            continue;
        }
        if (connect(fd, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            break;
        }
        CloseNativeSocket(fd);
        fd = kInvalidSocket;
    }
    freeaddrinfo(res);

    if (fd == kInvalidSocket) {
        LOG_ERROR(Lib_NpManager, "shadNet: failed to connect to {}:{}", hostname, port);
        return;
    }

    {
        std::scoped_lock lk{m_socketMutex};
        m_sockfd = fd;
    }

    if (!ReadServerInfo()) {
        LOG_ERROR(Lib_NpManager, "shadNet: handshake failed for {}:{}", hostname, port);
        CloseSocket();
        return;
    }

    shadnet::LoginRequest loginReq;
    loginReq.set_npid(npid);
    loginReq.set_password(password);

    shadnet::LoginReply loginReply;
    if (!SendCommand(kCmdLogin, &loginReq, &loginReply)) {
        LOG_ERROR(Lib_NpManager, "shadNet: login failed for npid '{}'", npid);
        CloseSocket();
        return;
    }

    m_accountUserId.store(loginReply.user_id(), std::memory_order_release);
    {
        std::scoped_lock lk{m_stateMutex};
        m_npid = npid;
    }

    shadnet::GetTokenReply tokenReply;
    if (SendCommand(kCmdGetToken, nullptr, &tokenReply) && !tokenReply.token().empty()) {
        NpHandler::GetInstance().SetBearerToken(m_ownerUserId, tokenReply.token());
    } else {
        // Non-fatal: WebAPI calls fall back to NpHandler's generated UUID,
        // which third-party (non-shadNet) WebAPI hosts accept anyway. Real
        // shadNet WebAPI calls will 401 until this succeeds.
        LOG_WARNING(Lib_NpManager, "shadNet: GetToken failed for npid '{}'; WebAPI calls will "
                                   "use a placeholder token",
                    npid);
    }

    LOG_INFO(Lib_NpManager, "shadNet: signed in as '{}' (account user_id={})", npid,
             loginReply.user_id());
    m_authenticated.store(true, std::memory_order_release);

    // Keep the connection open (and this thread alive) for as long as the
    // session is wanted; a real Matching2 client will reuse this same loop
    // to read room-event notifications. For now we just idle until told to
    // stop, periodically checking the socket is still alive.
    u8 probe[kHeaderSize];
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        int fd_check;
        {
            std::scoped_lock lk{m_socketMutex};
            fd_check = m_sockfd;
        }
        if (fd_check == kInvalidSocket) {
            break;
        }
        if (!ReadExact(probe, kHeaderSize)) {
            break; // connection dropped
        }
        // Anything arriving here today is a Notification we don't act on
        // yet; drain its payload and keep looping.
        const u32 size = GetU32LE(probe + 3);
        const size_t remaining = size > kHeaderSize ? size - kHeaderSize : 0;
        if (remaining > 0) {
            std::string discard(remaining, '\0');
            if (!ReadExact(discard.data(), remaining)) {
                break;
            }
        }
    }

    m_authenticated.store(false, std::memory_order_release);
    CloseSocket();
}

} // namespace Libraries::Np