// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "common/logging/log.h"
#include "core/libraries/network/net.h"
#include "core/libraries/np/np_handler.h"
#include "core/libraries/np/np_matching2/np_matching2_internal.h"
#include "core/libraries/np/np_session.h"
#include "core/libraries/np/np_signaling/np_signaling_stubs.h"
#include "shadnet.pb.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
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
int SendTo(int fd, const void* buf, size_t len, const struct sockaddr* dest_addr,
           socklen_t addrlen) {
    return sendto(fd, reinterpret_cast<const char*>(buf), static_cast<int>(len), 0, dest_addr,
                  addrlen);
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
int SendTo(int fd, const void* buf, size_t len, const struct sockaddr* dest_addr,
           socklen_t addrlen) {
    return static_cast<int>(::sendto(fd, buf, len, 0, dest_addr, addrlen));
}
#endif

constexpr size_t kHeaderSize = 15;
constexpr u32 kProtocolVersion = 1;
constexpr u16 kCmdLogin = 0;
constexpr u16 kCmdGetServerFeatures = 12;
constexpr u16 kCmdGetToken = 39;
constexpr u16 kCmdContextStart = 100;
constexpr u16 kCmdGetWorldInfoList = 111;
constexpr u16 kCmdRequestSignalingInfos = 105; // protocol.h CommandType::RequestSignalingInfos
                                               // — NOT the "(17)" in matching.md, that's just
                                               // that doc's section number.

// shadnet's signaling (STUN) wire framing: every UDP datagram to/from the
// MatchingUdpPort is prefixed with a 4-byte vport header of two 0xFFFF
// halves (see stun_server.cpp's SIGNALING_VPORT_NBO/FrameSignaling). This
// is unrelated to shadnet.proto/protobuf — it's a fixed raw layout.
constexpr u16 kSignalingVport = 0xFFFF;
constexpr size_t kVportHeaderSize = 4;
constexpr u8 kStunPingCmd = 0x01;

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
                           std::string token, std::string titleId, std::string titleName) {
    Disconnect(); // stop any previous attempt for this slot first

    m_ownerUserId = ownerUserId;
    m_stopRequested.store(false, std::memory_order_release);
    m_authenticated.store(false, std::memory_order_release);

    m_thread = std::thread(&NpSession::Run, this, std::move(host), std::move(npid),
                           std::move(password), std::move(token), std::move(titleId),
                           std::move(titleName));
}

void NpSession::Disconnect() {
    m_stopRequested.store(true, std::memory_order_release);
    CloseSocket();
    {
        std::scoped_lock lk{m_stunSocketMutex};
        if (m_stunSockfd != kInvalidSocket) {
            CloseNativeSocket(m_stunSockfd);
            m_stunSockfd = kInvalidSocket;
        }
    }
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
            std::string payload(remaining, '\0');
            if (remaining > 0 && !ReadExact(payload.data(), remaining)) {
                return false;
            }
            DispatchNotification(payload);
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
            LOG_ERROR(Lib_NpManager, "shadNet: command {} failed with error {:#x}", command, error);
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

u64 NpSession::SubmitRequest(u16 command, std::vector<u8> rawPayload) {
    const u32 totalSize = static_cast<u32>(kHeaderSize + rawPayload.size());
    const u64 packetId = m_nextPacketId.fetch_add(1, std::memory_order_relaxed);

    std::string packet;
    packet.resize(totalSize);
    u8* p = reinterpret_cast<u8*>(packet.data());
    p[0] = static_cast<u8>(PacketType::Request);
    PutU16LE(p + 1, command);
    PutU32LE(p + 3, totalSize);
    PutU64LE(p + 7, packetId);
    if (!rawPayload.empty()) {
        std::memcpy(p + kHeaderSize, rawPayload.data(), rawPayload.size());
    }

    {
        std::scoped_lock lk{m_pendingAsyncMutex};
        m_pendingAsync[packetId] = PendingAsyncRequest{command};
    }

    if (!WriteExact(packet.data(), packet.size())) {
        std::scoped_lock lk{m_pendingAsyncMutex};
        m_pendingAsync.erase(packetId);
        return 0;
    }
    return packetId;
}

void NpSession::HandleAsyncReply(u16 command, u64 packetId, u8 error, std::string body) {
    {
        std::scoped_lock lk{m_pendingAsyncMutex};
        m_pendingAsync.erase(packetId);
    }
    if (onAsyncReply) {
        std::vector<u8> bodyBytes(body.begin(), body.end());
        onAsyncReply(command, packetId, error, bodyBytes);
    }
}

void NpSession::Run(std::string host, std::string npid, std::string password, std::string token,
                    std::string titleId, std::string titleName) {
    // Kept in the signature for when a shadNet build with presence fields is
    // used; this build's LoginRequest doesn't have them (see comment below).
    (void)titleId;
    (void)titleName;
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
    // CONFIRM: shadnet.proto's LoginRequest needs a `token` field for this
    // to do anything server-side. If it doesn't have one yet, this is a
    // no-op until the proto is extended — check shadnet.proto before
    // relying on token-based re-auth actually working.
    if (!token.empty()) {
        loginReq.set_token(token);
    }
    // Still no title_id/title_name fields in this proto build (unchanged
    // from before).

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
        // CONFIRM: if LoginReply carries a bearer token or avatar URL field,
        // capture it here, e.g.:
        //   m_bearerToken = loginReply.token();
        //   m_avatarUrl = loginReply.avatar_url();
        // Neither is visible in the code you sent — GetBearerToken()
        // currently relies solely on the separate GetToken command below.
    }

    // Process friendList and blockList from LoginReply
    LOG_INFO(
        Lib_NpManager,
        "LoginReply: friends={}, blocked={}, friend_requests_sent={}, friend_requests_received={}",
        loginReply.friends_size(), loginReply.blocked_size(),
        loginReply.friend_requests_sent_size(), loginReply.friend_requests_received_size());

    for (const auto& friend_entry : loginReply.friends()) {
        LOG_DEBUG(Lib_NpManager, "Friend: npid='{}' online={}", friend_entry.npid(),
                  friend_entry.online());
    }

    for (const auto& blocked_npid : loginReply.blocked()) {
        LOG_DEBUG(Lib_NpManager, "Blocked: npid='{}'", blocked_npid);
    }

    if (onLoginResult) {
        NpSession::LoginResultInfo info;
        info.error = 0;
        for (const auto& friend_entry : loginReply.friends()) {
            info.friends.emplace_back(friend_entry.npid(), friend_entry.online());
        }
        for (const auto& sent : loginReply.friend_requests_sent()) {
            info.requestsSent.push_back(sent);
        }
        for (const auto& recv : loginReply.friend_requests_received()) {
            info.requestsReceived.push_back(recv);
        }
        for (const auto& blocked_npid : loginReply.blocked()) {
            info.blocked.push_back(blocked_npid);
        }
        onLoginResult(info);
    }

    shadnet::GetTokenReply tokenReply;
    if (SendCommand(kCmdGetToken, nullptr, &tokenReply) && !tokenReply.token().empty()) {
        {
            std::scoped_lock lk{m_stateMutex};
            m_bearerToken = tokenReply.token();
        }
        NpHandler::GetInstance().GetBearerToken(m_ownerUserId);
    } else {
        LOG_WARNING(Lib_NpManager,
                    "shadNet: GetToken failed for npid '{}'; WebAPI calls will "
                    "use a placeholder token",
                    npid);
    }

    // GetServerFeatures: query server capabilities (matching2_enabled, etc.)
    shadnet::ServerFeaturesReply featuresReply;
    // Use the same host as TCP connection, but UDP port 31314 (standard STUN port).
    // Resolved here (rather than down at the STUN ping call site) because the
    // matching2 MM-server registration below needs it too.
    const auto [stunHost, tcpPort] = SplitHostPort(host);
    const u16 stunPort = 31314; // Standard STUN/UDP port from shadnet protocol

    if (SendCommand(kCmdGetServerFeatures, nullptr, &featuresReply)) {
        m_matching2Enabled.store(featuresReply.matching2_enabled(), std::memory_order_release);
        LOG_INFO(Lib_NpManager, "GetServerFeatures: \"{}\" matching2_enabled= {}", npid,
                 featuresReply.matching2_enabled() ? "true" : "false");

        // Initialize matching2 signaling if enabled
        if (featuresReply.matching2_enabled()) {
            NpSignaling::Stubs::SetMatching2Enabled(true);
            // The "MM server" NpSignaling pings is this shadNet server itself
            // (stunHost:stunPort), not our own external endpoint -- that only
            // gets learned later, once StunPing() below gets a reply. Resolve
            // it with getaddrinfo since stunHost may be a hostname, not a
            // dotted-decimal address (sceNetInetPton only handles the latter).
            u32 mm_addr_nbo = 0;
            struct addrinfo mm_hints{};
            mm_hints.ai_family = AF_INET;
            mm_hints.ai_socktype = SOCK_DGRAM;
            struct addrinfo* mm_res = nullptr;
            const std::string stunPortStr = std::to_string(stunPort);
            if (getaddrinfo(stunHost.c_str(), stunPortStr.c_str(), &mm_hints, &mm_res) == 0 &&
                mm_res) {
                mm_addr_nbo =
                    reinterpret_cast<struct sockaddr_in*>(mm_res->ai_addr)->sin_addr.s_addr;
                freeaddrinfo(mm_res);
            } else {
                LOG_WARNING(Lib_NpManager,
                            "shadNet: failed to resolve MM server '{}'; matching2 "
                            "StunPing will be skipped",
                            stunHost);
            }
            NpSignaling::Stubs::SetMmServerEndpoint(mm_addr_nbo,
                                                    Libraries::Net::sceNetHtons(stunPort));

            // Set peer resolver to look up peer endpoints from matching2 context
            NpSignaling::Stubs::SetPeerResolver([](std::string_view online_id, u32* out_addr,
                                                   u16* out_port) -> bool {
                for (u32 id = 1; id <= NpMatching2::ContextManager::kMaxContexts; ++id) {
                    NpMatching2::ContextObject* ctx = NpMatching2::ContextManager::Instance().Get(
                        static_cast<NpMatching2::OrbisNpMatching2ContextId>(id));
                    if (!ctx) {
                        continue;
                    }
                    for (const auto& [member_id, peer] : ctx->peers) {
                        std::string peer_online_id(peer.online_id.data);
                        if (peer_online_id == online_id) {
                            if (out_addr) {
                                *out_addr = peer.addr;
                            }
                            if (out_port) {
                                *out_port = peer.port;
                            }
                            return true;
                        }
                    }
                }
                return false;
            });
        }
    } else {
        LOG_WARNING(Lib_NpManager, "GetServerFeatures failed for npid '{}'", npid);
    }

    // STUN ping: register external endpoint with server for P2P discovery

    // Create UDP socket for STUN pings (reused for periodic pings)
    {
        std::scoped_lock lk{m_stunSocketMutex};
        m_stunSockfd = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
        if (m_stunSockfd == kInvalidSocket) {
            LOG_ERROR(Lib_NpManager, "shadNet: failed to create STUN UDP socket");
        }
    }

    if (StunPing(stunHost, stunPort, m_stunSockfd)) {
        LOG_INFO(Lib_NpManager, "STUN ping: npid= \"{}\" ext= \"{}\" : {}", npid, ExternalIp(),
                 ExternalPort());
    } else {
        LOG_WARNING(Lib_NpManager, "STUN ping failed for npid '{}'", npid);
    }

    // ContextStart: establish matching context with server
    // Use titleId for matching scoping (passed to Run but currently unused in this build)
    const u32 defaultCtxId = 1; // Default context ID for initial connection
    if (ContextStart(defaultCtxId, titleId)) {
        LOG_INFO(Lib_NpManager, "ContextStart: ctx= {} title= \"{}\"", defaultCtxId, titleId);
    } else {
        LOG_WARNING(Lib_NpManager, "ContextStart failed for npid '{}'", npid);
    }

    // WebAPI: fetch blockList and friendList
    // These are HTTP requests to the WebAPI server (port 31315)
    // For now, we just log that these would be fetched
    LOG_INFO(Lib_NpManager, "WebAPI: blockList for \"{}\" -> total 0", npid);
    LOG_INFO(Lib_NpManager, "WebAPI: friendList for \"{}\" -> total 0", npid);

    LOG_INFO(Lib_NpManager, "shadNet: signed in as '{}' (account user_id={})", npid,
             loginReply.user_id());
    m_authenticated.store(true, std::memory_order_release);

    // Keep the connection open (and this thread alive) for as long as the
    // session is wanted; a real Matching2 client will reuse this same loop
    // to read room-event notifications. Send periodic STUN pings to keep
    // the connection alive and register external endpoint.
    u8 probe[kHeaderSize];
    auto last_stun_ping = std::chrono::steady_clock::now();
    const auto stun_ping_interval = std::chrono::milliseconds(2500); // ~2.5 seconds like fork

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        int fd_check;
        {
            std::scoped_lock lk{m_socketMutex};
            fd_check = m_sockfd;
        }
        if (fd_check == kInvalidSocket) {
            break;
        }

        // Check if it's time for a periodic STUN ping
        auto now = std::chrono::steady_clock::now();
        if (now - last_stun_ping >= stun_ping_interval) {
            const auto [stunHost, tcpPort] = SplitHostPort(host);
            const u16 stunPort = 31314;
            int stunFd;
            {
                std::scoped_lock lk{m_stunSocketMutex};
                stunFd = m_stunSockfd;
            }
            if (stunFd != kInvalidSocket) {
                StunPing(stunHost, stunPort, stunFd);
            }
            last_stun_ping = now;
        }

        // Try to read from socket with timeout to allow periodic pings
        // Use select/poll to check if data is available
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd_check, &readfds);
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; // 100ms timeout to allow periodic pings

        int select_result = select(fd_check + 1, &readfds, nullptr, nullptr, &timeout);
        if (select_result < 0) {
            break; // socket error
        }
        if (select_result == 0) {
            continue; // timeout, loop back to check for STUN ping
        }

        if (!ReadExact(probe, kHeaderSize)) {
            break; // connection dropped
        }
        // Anything arriving here today is a Notification we don't act on
        // yet; drain its payload and keep looping.
        const u32 size = GetU32LE(probe + 3);
        const size_t remaining = size > kHeaderSize ? size - kHeaderSize : 0;
        if (remaining > 0) {
            std::string payload(remaining, '\0');
            if (!ReadExact(payload.data(), remaining)) {
                break;
            }
            DispatchNotification(payload);
        }
    }

    m_authenticated.store(false, std::memory_order_release);
    CloseSocket();
}

bool NpSession::StunPing(const std::string& stunHost, u16 stunPort, int localSockfd) {
    std::string npid;
    {
        std::scoped_lock lk{m_stateMutex};
        npid = m_npid;
    }
    if (npid.empty()) {
        LOG_WARNING(Lib_NpManager, "shadNet: StunPing called before login; no npid to send");
        return false;
    }

    struct addrinfo hints{};
    hints.ai_family = AF_INET; // STUN reply/local-ip fields here are IPv4-only
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo* res = nullptr;
    const std::string portStr = std::to_string(stunPort);
    if (getaddrinfo(stunHost.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        LOG_ERROR(Lib_NpManager, "shadNet: StunPing failed to resolve {}", stunHost);
        return false;
    }
    struct sockaddr_in dest{};
    std::memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);

    const bool ownSocket = (localSockfd == kInvalidSocket);
    int fd = localSockfd;
    if (ownSocket) {
        fd = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
        if (fd == kInvalidSocket) {
            LOG_ERROR(Lib_NpManager, "shadNet: StunPing failed to create UDP socket");
            return false;
        }
    }

    // Build request: [4-byte vport header][cmd=0x01][npid, 16B null-padded][localIp, 4B]
    // localIp isn't actually consulted server-side (only its presence is
    // length-checked), so zero-filling it is fine.
    std::array<u8, kVportHeaderSize + 1 + 16 + 4> packet{};
    PutU16LE(packet.data(), kSignalingVport);
    PutU16LE(packet.data() + 2, kSignalingVport);
    packet[kVportHeaderSize] = kStunPingCmd;
    const size_t copyLen = std::min<size_t>(npid.size(), 16);
    std::memcpy(packet.data() + kVportHeaderSize + 1, npid.data(), copyLen);
    // bytes for localIp already zero from value-init.

    const int sent = SendTo(fd, packet.data(), packet.size(),
                            reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
    if (sent != static_cast<int>(packet.size())) {
        LOG_ERROR(Lib_NpManager, "shadNet: StunPing send failed");
        if (ownSocket)
            CloseNativeSocket(fd);
        return false;
    }

    // Wait up to 2s for the reply so this never hangs the caller forever.
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    struct timeval tv{2, 0};
    const int sel = select(fd + 1, &readfds, nullptr, nullptr, &tv);
    if (sel <= 0) {
        LOG_WARNING(Lib_NpManager, "shadNet: StunPing timed out waiting for reply from {}:{}",
                    stunHost, stunPort);
        if (ownSocket)
            CloseNativeSocket(fd);
        return false;
    }

    std::array<u8, kVportHeaderSize + 4 + 2> reply{};
    const int got = RecvAll(fd, reply.data(), reply.size());
    if (ownSocket) {
        CloseNativeSocket(fd);
    }
    if (got != static_cast<int>(reply.size())) {
        LOG_ERROR(Lib_NpManager, "shadNet: StunPing got malformed reply ({} bytes)", got);
        return false;
    }
    if (GetU16LE(reply.data()) != kSignalingVport ||
        GetU16LE(reply.data() + 2) != kSignalingVport) {
        LOG_ERROR(Lib_NpManager, "shadNet: StunPing reply missing signaling vport header");
        return false;
    }

    const u8* ipBytes = reply.data() + kVportHeaderSize;
    char ipStr[16];
    std::snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u", ipBytes[0], ipBytes[1], ipBytes[2],
                  ipBytes[3]);
    const u16 extPort = (static_cast<u16>(reply[kVportHeaderSize + 4]) << 8) |
                        static_cast<u16>(reply[kVportHeaderSize + 5]);

    {
        std::scoped_lock lk{m_stateMutex};
        m_externalIp = ipStr;
    }
    m_externalPort.store(extPort, std::memory_order_release);

    LOG_INFO(Lib_NpManager, "shadNet: StunPing resolved external endpoint {}:{}", ipStr, extPort);
    return true;
}

void NpSession::DispatchNotification(const std::string& payload) {
    if (payload.empty()) {
        return;
    }

    // Placeholder framing assumption: [u16 LE notifyType][protobuf blob]
    // matching the same pattern SendCommand() uses for requests. REPLACE
    // with the real layout once shadnet.proto / notification framing docs
    // are available.
    if (payload.size() < 2) {
        LOG_WARNING(Lib_NpManager, "shadNet: notification payload too small ({} bytes)",
                    payload.size());
        return;
    }
    const u16 notifyType = GetU16LE(reinterpret_cast<const u8*>(payload.data()));
    const char* blob = payload.data() + 2;
    const size_t blobLen = payload.size() - 2;

    // CONFIRM: these notifyType values are placeholders — replace with the
    // actual constants once known (likely defined alongside kCmdLogin etc.,
    // or in a shared protocol.h).
    constexpr u16 kNotifyFriendQuery = 1000;
    constexpr u16 kNotifyFriendNew = 1001;
    constexpr u16 kNotifyFriendLost = 1002;
    constexpr u16 kNotifyFriendStatus = 1003;
    constexpr u16 kNotifyWebApiPushEvent = 1004;

    // CONFIRM: Notification message types not in shadnet.proto yet
    // switch (notifyType) {
    // case kNotifyFriendQuery: {
    //     shadnet::NotifyFriendQuery n; // CONFIRM message name
    //     if (n.ParseFromArray(blob, static_cast<int>(blobLen)) && onFriendQuery) {
    //         onFriendQuery(NotifyFriendQuery{n.from_npid()});
    //     }
    //     break;
    // }
    // case kNotifyFriendNew: {
    //     shadnet::NotifyFriendNew n;
    //     if (n.ParseFromArray(blob, static_cast<int>(blobLen)) && onFriendNew) {
    //         onFriendNew(NotifyFriendNew{n.npid(), n.online()});
    //     }
    //     break;
    // }
    // case kNotifyFriendLost: {
    //     shadnet::NotifyFriendLost n;
    //     if (n.ParseFromArray(blob, static_cast<int>(blobLen)) && onFriendLost) {
    //         onFriendLost(NotifyFriendLost{n.npid()});
    //     }
    //     break;
    // }
    // case kNotifyFriendStatus: {
    //     shadnet::NotifyFriendStatus n;
    //     if (n.ParseFromArray(blob, static_cast<int>(blobLen)) && onFriendStatus) {
    //         onFriendStatus(NotifyFriendStatus{n.npid(), n.online()});
    //     }
    //     break;
    // }
    // case kNotifyWebApiPushEvent: {
    //     shadnet::NotifyWebApiPushEvent n;
    //     if (n.ParseFromArray(blob, static_cast<int>(blobLen)) && onWebApiPushEvent) {
    //         NotifyWebApiPushEvent ev;
    //         ev.npServiceName = n.np_service_name();
    //         ev.npServiceLabel = n.np_service_label();
    //         ev.dataType = n.data_type();
    //         ev.fromNpid = n.from_npid();
    //         ev.toNpid = n.to_npid();
    //         ev.data.assign(n.data().begin(), n.data().end());
    //         for (const auto& kv : n.extd_data()) {
    //             ev.extdData.emplace_back(kv.key(), kv.value());
    //         }
    //         onWebApiPushEvent(ev);
    //     }
    //     break;
    // }
    // default:
    //     LOG_DEBUG(Lib_NpManager, "shadNet: unrecognized notification type {}", notifyType);
    //     break;
    // }
    LOG_DEBUG(Lib_NpManager, "shadNet: notification type {} received (handler not implemented yet)", notifyType);
}

bool NpSession::RequestSignalingInfos(const std::string& targetNpid, PeerEndpoint& out) {
    if (!IsAuthenticated()) {
        LOG_WARNING(Lib_NpManager, "shadNet: RequestSignalingInfos called before login");
        return false;
    }

    // NOTE: this issues a request/reply exchange on the same TCP socket the
    // background Run() thread's idle loop is blocked reading from for
    // notifications. Calling this concurrently with incoming notification
    // traffic can interleave reads and corrupt the stream. shadnet currently
    // sends no notifications for this command, so in practice this is safe
    // today, but the real fix is to route this call through a request queue
    // owned by the Run() thread rather than calling SendCommand() directly
    // from an arbitrary caller thread. Flagging this rather than silently
    // shipping the race — worth fixing before this sees concurrent load.
    shadnet::RequestSignalingInfosRequest req;
    req.set_target_npid(targetNpid);

    shadnet::RequestSignalingInfosReply reply;
    if (!SendCommand(kCmdRequestSignalingInfos, &req, &reply)) {
        LOG_WARNING(Lib_NpManager, "shadNet: RequestSignalingInfos failed for npid '{}'",
                    targetNpid);
        return false;
    }

    out.npid = reply.target_npid();
    out.ip = reply.target_ip();
    out.port = static_cast<u16>(reply.target_port());
    out.memberId = reply.target_member_id();
    return true;
}

bool NpSession::ContextStart(u32 ctxId, const std::string& titleId) {
    if (!IsAuthenticated()) {
        LOG_WARNING(Lib_NpManager, "shadNet: ContextStart called before login");
        return false;
    }

    shadnet::ContextStartRequest req;
    req.set_ctx_id(ctxId);

    // ContextStart reply is just an error byte, no meaningful body
    if (!SendCommand(kCmdContextStart, &req, nullptr)) {
        LOG_WARNING(Lib_NpManager, "shadNet: ContextStart failed for ctx={} title={}", ctxId,
                    titleId);
        return false;
    }

    LOG_INFO(Lib_NpManager, "ContextStart: \"{}\" ctx= {} title= \"{}\" key= \"{}\"", Npid(), ctxId,
             titleId, titleId);
    return true;
}

bool NpSession::GetWorldInfoList() {
    if (!IsAuthenticated()) {
        LOG_WARNING(Lib_NpManager, "shadNet: GetWorldInfoList called before login");
        return false;
    }

    shadnet::GetWorldInfoListRequest req;
    req.set_server_id(1); // Default server ID

    shadnet::GetWorldInfoListReply reply;
    if (!SendCommand(kCmdGetWorldInfoList, &req, &reply)) {
        LOG_WARNING(Lib_NpManager, "shadNet: GetWorldInfoList failed for npid '{}'", Npid());
        return false;
    }

    LOG_INFO(Lib_NpManager, "GetWorldInfoList: \"{}\" worlds= {}", Npid(), reply.worlds_size());

    for (const auto& world : reply.worlds()) {
        LOG_DEBUG(Lib_NpManager, "World: id={} lobbies={} max_members={}", world.world_id(),
                  world.lobbies_num(), world.max_lobby_members());
    }

    return true;
}

namespace {
constexpr u16 kCmdAddFriend = 40;    // CONFIRM
constexpr u16 kCmdRemoveFriend = 41; // CONFIRM
constexpr u16 kCmdAddBlock = 42;     // CONFIRM
constexpr u16 kCmdRemoveBlock = 43;  // CONFIRM
} // namespace

void NpSession::AddFriend(const std::string& npid) {
    if (!IsAuthenticated()) {
        return;
    }
    // CONFIRM: AddFriendRequest not in shadnet.proto yet
    // shadnet::AddFriendRequest req;
    // req.set_npid(npid);
    // SendCommand(kCmdAddFriend, &req, nullptr);
}

void NpSession::RemoveFriend(const std::string& npid) {
    if (!IsAuthenticated()) {
        return;
    }
    // CONFIRM: RemoveFriendRequest not in shadnet.proto yet
    // shadnet::RemoveFriendRequest req;
    // req.set_npid(npid);
    // SendCommand(kCmdRemoveFriend, &req, nullptr);
}

void NpSession::AddBlock(const std::string& npid) {
    if (!IsAuthenticated()) {
        return;
    }
    // CONFIRM: AddBlockRequest not in shadnet.proto yet
    // shadnet::AddBlockRequest req;
    // req.set_npid(npid);
    // SendCommand(kCmdAddBlock, &req, nullptr);
}

void NpSession::RemoveBlock(const std::string& npid) {
    if (!IsAuthenticated()) {
        return;
    }
    // CONFIRM: RemoveBlockRequest not in shadnet.proto yet
    // shadnet::RemoveBlockRequest req;
    // req.set_npid(npid);
    // SendCommand(kCmdRemoveBlock, &req, nullptr);
}

void NpSession::SetAppearOffline(bool enable) {
    m_appearOffline.store(enable, std::memory_order_release);
    // CONFIRM: does shadnet.proto's LoginRequest (or a separate toggle
    // command) carry an appear-offline flag? ShadNetClient::
    // SetAppearOffline() is called pre-auth in np_handler.cpp
    // ("caches on the client" per that comment), then presumably sent as
    // part of/after login. Nothing in np_session.cpp today sends this bit —
    // needs a real command ID once you confirm where it lives on the wire.
}

} // namespace Libraries::Np