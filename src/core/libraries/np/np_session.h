// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <google/protobuf/message_lite.h>

#include "common/types.h"
#include "core/libraries/system/userservice.h"

namespace Libraries::Np {

// A real client for shadNet's TCP protocol (see documentation/protocol.md in
// the shadNet repo): 15-byte header framing, protobuf3 payloads, spoken over
// a single persistent connection per logged-in PS4 user slot.
//
// Phase 1 of this session only drives account-level commands (Login,
// GetToken) so the rest of the emulator can report a real signed-in state
// and a real per-account WebAPI bearer token (see np_handler.h). It does not
// yet speak the Matching2 room commands (12-23) — that's the next piece to
// build on top of the connection this class keeps open.
//
// All network I/O happens on an internal background thread so HLE syscall
// handlers never block on it; callers poll IsAuthenticated().
class NpSession {
public:
    NpSession() = default;
    ~NpSession();

    NpSession(const NpSession&) = delete;
    NpSession& operator=(const NpSession&) = delete;

    // Starts (or restarts) the background connect/login thread. Safe to call
    // from an HLE handler; returns immediately.
    //  - host: "host:port" as stored in Config (e.g. GetShadnetServer())
    //  - npid/password: this slot's shadNet account credentials
    //  - titleId/titleName: currently-running title, forwarded in
    //    LoginRequest so the server can resolve matchmaking/presence scoping
    void LoginAsync(Libraries::UserService::OrbisUserServiceUserId ownerUserId, std::string host,
                    std::string npid, std::string password, std::string titleId,
                    std::string titleName);

    // Closes the connection and stops the background thread. Safe to call
    // even if never logged in.
    void Disconnect();

    bool IsAuthenticated() const {
        return m_authenticated.load(std::memory_order_acquire);
    }

    u64 AccountUserId() const {
        return m_accountUserId.load(std::memory_order_acquire);
    }

    std::string Npid() const {
        std::scoped_lock lk{m_stateMutex};
        return m_npid;
    }

    // Sends a STUN ping (shadnet's signaling protocol, see documentation/
    // signaling.md) to `stunHost:stunPort` over the given local UDP socket,
    // recording this npid's external IP:port with the server so it can be
    // handed out later via RequestSignalingInfos. `localSockfd` should be
    // the same OS socket the game's P2P transport already owns (so the
    // external mapping STUN discovers matches the one peers will actually
    // send to) — pass -1 to have this call open and use its own temporary
    // socket instead. Returns false on any I/O failure or timeout.
    bool StunPing(const std::string& stunHost, u16 stunPort, int localSockfd = -1);

    std::string ExternalIp() const {
        std::scoped_lock lk{m_stateMutex};
        return m_externalIp;
    }
    u16 ExternalPort() const {
        return m_externalPort.load(std::memory_order_acquire);
    }

    // Looks up a peer's UDP endpoint by npid via the authenticated TCP
    // session (command RequestSignalingInfos). Must be called after a
    // successful LoginAsync (IsAuthenticated() == true). Returns false if
    // not authenticated, the peer is unknown, or the request fails.
    struct PeerEndpoint {
        std::string npid;
        std::string ip;
        u16 port = 0;
        u32 memberId = 0;
    };
    bool RequestSignalingInfos(const std::string& targetNpid, PeerEndpoint& out);

private:
    void Run(std::string host, std::string npid, std::string password, std::string titleId,
             std::string titleName);
    void CloseSocket();

    // Raw framed I/O helpers. Return false on any socket error/EOF/version
    // mismatch, at which point the caller should give up on this attempt.
    bool ReadExact(void* buf, size_t len);
    bool WriteExact(const void* buf, size_t len);
    bool ReadServerInfo();

    // Sends a Request with an optional protobuf3 payload (pass nullptr for
    // commands with no request body, e.g. GetToken) and waits for the
    // matching Reply. On success (error byte == 0) parses the reply body
    // into `replyOut` (may be nullptr for replies with no meaningful body)
    // and returns true.
    bool SendCommand(u16 command, const google::protobuf::MessageLite* request,
                     google::protobuf::MessageLite* replyOut);

    std::thread m_thread;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_authenticated{false};
    std::atomic<u64> m_accountUserId{0};
    std::atomic<u64> m_nextPacketId{1};

    mutable std::mutex m_stateMutex;
    std::string m_npid;
    std::string m_externalIp;
    std::atomic<u16> m_externalPort{0};

    std::mutex m_socketMutex;
    int m_sockfd = -1;

    Libraries::UserService::OrbisUserServiceUserId m_ownerUserId = -1;
};

} // namespace Libraries::Np