// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Functional NpSignaling HLE for Bloodborne multiplayer.
//
// The SocketState machine (SocketState_tick at 0x10d8bb0) uses these APIs to manage
// P2P connections. Key functions:
//   - sceNpSignalingCreateContext: stores callback + userArg for event delivery
//   - sceNpSignalingActivateConnection: returns connId, queues ESTABLISHED+ACTIVE events
//   - sceNpSignalingGetConnectionStatus: returns peer IP:port for SocketState state 3/4
//   - sceNpSignalingDeactivateConnection: cleanup
//
// Events are delivered to the game's NpSignaling_callback (0x10c10d0) which pushes
// them into SessionOwner's queue at +0x98. The game's main thread drains this queue via
// SessionOwner_drainSignalingEvents (0x10c0bb0) and dispatches to SocketState handlers.
//
// Phase 2 fixes:
//   - Multi-context support (context-keyed callbacks, not global singleton)
//   - Idempotent ActivateConnection (same NpId -> same connId, events fire only once)
//   - ConnId-aware GetConnectionStatus (looks up by connId, not random peer)

#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/threads.h"
#include "core/libraries/libs.h"
#include "core/libraries/np/np_signaling.h"

namespace Libraries::Np::NpSignaling {

// --- Multi-context support ---
// Each sceNpSignalingCreateContext call creates a context with its own callback.
// The SCO init calls CreateContext with the game's NpSignaling_callback.

struct NpSignalingContext {
    OrbisNpSignalingHandler callback = nullptr;
    void* callback_arg = nullptr;
    bool active = false;
};

static std::unordered_map<s32, NpSignalingContext> s_contexts;
static s32 s_next_ctx_id = 1;

// --- Connection tracking ---
// Maps connId -> connection info for connId-aware GetConnectionStatus.
// Also tracks NpId hash -> existing connId for idempotent ActivateConnection.

struct ConnectionInfo {
    s32 conn_id;
    s32 ctx_id;
    u32 addr;             // network byte order
    u16 port;             // network byte order
    s32 status;           // ORBIS_NP_SIGNALING_CONN_STATUS_*
    std::string npid_str; // for logging
    bool events_fired;    // ESTABLISHED+ACTIVE already delivered?
    std::chrono::steady_clock::time_point last_event_time{}; // rate-limit re-fires
};

static std::unordered_map<s32, ConnectionInfo> s_connections; // connId -> info
static std::unordered_map<std::string, s32> s_npid_to_conn;   // npId hash -> connId
static s32 s_next_conn_id = 1;

static bool s_initialized = false;
static std::mutex s_mutex;

// --- Peer info (shared with NpMatching2 HLE) ---
static std::map<u16, NpSignalingPeerInfo> s_peers;

// --- Async threads ---
static std::vector<Kernel::PthreadT> s_async_threads;
static std::mutex s_async_mutex;

// Forward declaration (defined below, used by SetPeerInfo for deferred events)
static void DeliverSignalingEventForCtx(s32 ctx_id, s32 conn_id, u32 event_type, u32 delay_ms);

// --- Local address (configurable, set by NpMatching2 on context start) ---
static u32 s_local_addr = 0x0100007f; // 127.0.0.1 in network byte order (default)
static u16 s_local_port = 0x5B24;     // 9307 in network byte order (default)

void SetLocalAddr(u32 addr, u16 port) {
    s_local_addr = addr;
    s_local_port = port;
    LOG_INFO(Lib_NpSignaling, "SetLocalAddr: addr={:#x} port={}", addr, ntohs(port));
}

u32 GetLocalAddr() {
    return s_local_addr;
}
u16 GetLocalPort() {
    return s_local_port;
}

// --- Shared peer info API ---

void SetPeerInfo(u16 member_id, u32 addr, u16 port, const std::string& online_id) {
    // Collect deferred events to fire AFTER releasing the lock
    struct DeferredEvent {
        s32 ctx_id;
        s32 conn_id;
    };
    std::vector<DeferredEvent> deferred;

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        NpSignalingPeerInfo pi;
        pi.member_id = member_id;
        pi.addr = addr;
        pi.port = port;
        pi.status = (addr != 0) ? ORBIS_NP_SIGNALING_CONN_STATUS_ACTIVE
                                : ORBIS_NP_SIGNALING_CONN_STATUS_INACTIVE;
        pi.online_id = online_id;
        s_peers[member_id] = pi;
        LOG_INFO(Lib_NpSignaling,
                 "SetPeerInfo: member={} addr={:#x} ({}.{}.{}.{}) port={} online_id='{}'",
                 member_id, addr, (ntohl(addr) >> 24) & 0xff, (ntohl(addr) >> 16) & 0xff,
                 (ntohl(addr) >> 8) & 0xff, ntohl(addr) & 0xff, port ? ntohs(port) : 0, online_id);

        // Check for pending connections that were waiting for this peer's data.
        // ActivateConnection may have been called for this NpId before the peer joined.
        if (!online_id.empty() && addr != 0) {
            auto conn_it = s_npid_to_conn.find(online_id);
            if (conn_it != s_npid_to_conn.end()) {
                auto& ci = s_connections[conn_it->second];
                if (!ci.events_fired) {
                    // Update connection with real peer data
                    ci.addr = addr;
                    ci.port = port;
                    ci.status = ORBIS_NP_SIGNALING_CONN_STATUS_ACTIVE;
                    ci.events_fired = true;
                    deferred.push_back({ci.ctx_id, ci.conn_id});
                    LOG_INFO(Lib_NpSignaling,
                             "SetPeerInfo: resolved pending connection for '{}' -> "
                             "connId={} addr={:#x} port={}",
                             online_id, ci.conn_id, addr, ntohs(port));
                }
            }
        }
    } // s_mutex released

    // Fire deferred ESTABLISHED+ACTIVE events outside the lock
    for (const auto& ev : deferred) {
        DeliverSignalingEventForCtx(ev.ctx_id, ev.conn_id, ORBIS_NP_SIGNALING_EVENT_ESTABLISHED,
                                    200);
        DeliverSignalingEventForCtx(ev.ctx_id, ev.conn_id,
                                    ORBIS_NP_SIGNALING_EVENT_MUTUAL_ACTIVATED, 400);
    }
}

NpSignalingPeerInfo GetPeerInfo(u16 member_id) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_peers.find(member_id);
    if (it != s_peers.end())
        return it->second;
    return {};
}

// Internal: caller must already hold s_mutex.
// Skips the local peer (self) so P2P address resolution finds the REMOTE peer.
static NpSignalingPeerInfo GetAnyActivePeer_locked() {
    for (const auto& [mid, pi] : s_peers) {
        if (pi.status == ORBIS_NP_SIGNALING_CONN_STATUS_ACTIVE &&
            !(pi.addr == s_local_addr && pi.port == s_local_port))
            return pi;
    }
    return {};
}

NpSignalingPeerInfo GetAnyActivePeer() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return GetAnyActivePeer_locked();
}

// --- Async event delivery ---
// Events must be delivered from a PS4 thread (valid g_curthread) with a small delay
// to ensure the caller returns first.

struct AsyncSignalingEventArgs {
    s32 ctx_id;
    s32 conn_id;
    u32 event_type;
    u32 error_code;
    OrbisNpSignalingHandler callback;
    void* callback_arg;
    u32 delay_ms;
};

static PS4_SYSV_ABI void* SignalingEventThreadFunc(void* arg) {
    auto* a = static_cast<AsyncSignalingEventArgs*>(arg);

    // Delay to ensure ActivateConnection returns first.
    // The game's SocketState machine needs to store the connId before the event fires.
    std::this_thread::sleep_for(std::chrono::milliseconds(a->delay_ms));

    if (a->callback) {
        LOG_INFO(Lib_NpSignaling, "firing signaling event: ctxId={} connId={} event={:#x}",
                 a->ctx_id, a->conn_id, a->event_type);
        a->callback(static_cast<u32>(a->ctx_id), static_cast<u32>(a->conn_id), a->event_type,
                    a->error_code, a->callback_arg);
        LOG_INFO(Lib_NpSignaling, "signaling event callback returned");
    }

    delete a;
    return nullptr;
}

static void DeliverSignalingEventForCtx(s32 ctx_id, s32 conn_id, u32 event_type,
                                        u32 delay_ms = 200) {
    OrbisNpSignalingHandler callback = nullptr;
    void* callback_arg = nullptr;

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_contexts.find(ctx_id);
        if (it != s_contexts.end() && it->second.active) {
            callback = it->second.callback;
            callback_arg = it->second.callback_arg;
        }
    }

    if (!callback) {
        LOG_WARNING(Lib_NpSignaling,
                    "DeliverSignalingEventForCtx: no active callback for ctx={}, event={:#x}",
                    ctx_id, event_type);
        return;
    }

    auto* args = new AsyncSignalingEventArgs{ctx_id,   conn_id,      event_type, 0,
                                             callback, callback_arg, delay_ms};

    Kernel::PthreadT thread = nullptr;
    int ret = Kernel::posix_pthread_create(&thread, nullptr, SignalingEventThreadFunc, args);
    if (ret != 0) {
        LOG_ERROR(Lib_NpSignaling, "failed to create event delivery thread: {}", ret);
        delete args;
    } else {
        std::lock_guard<std::mutex> lock(s_async_mutex);
        s_async_threads.push_back(thread);
    }
}

// --- PS4 API implementations ---

s32 PS4_SYSV_ABI sceNpSignalingInitialize() {
    LOG_INFO(Lib_NpSignaling, "called");
    s_initialized = true;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingTerminate() {
    LOG_INFO(Lib_NpSignaling, "called");
    std::lock_guard<std::mutex> lock(s_mutex);
    s_initialized = false;
    s_contexts.clear();
    s_connections.clear();
    s_npid_to_conn.clear();
    s_peers.clear();
    s_next_ctx_id = 1;
    s_next_conn_id = 1;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingCreateContext(s32 param_1, void* param_2, void* param_3,
                                             s32* context_id) {
    // param_1: npId pointer (passed as integer due to PS4 ABI)
    // param_2: callback function pointer
    // param_3: callback userArg (SessionOwner pointer for SCO init)
    // context_id: output context ID

    std::lock_guard<std::mutex> lock(s_mutex);

    s32 ctx_id = s_next_ctx_id++;
    NpSignalingContext ctx;
    ctx.callback = reinterpret_cast<OrbisNpSignalingHandler>(param_2);
    ctx.callback_arg = param_3;
    ctx.active = true;
    s_contexts[ctx_id] = ctx;

    if (context_id) {
        *context_id = ctx_id;
    }

    LOG_INFO(Lib_NpSignaling, "context created: id={} callback={} arg={} (total contexts={})",
             ctx_id, param_2, param_3, s_contexts.size());
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingCreateContextA() {
    LOG_ERROR(Lib_NpSignaling, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingActivateConnection(s32 ctxId, void* npId, s32* connId) {
    // On first call for a given NpId: assigns connId.
    //   - If peer data is available (peer already joined): fires ESTABLISHED+ACTIVE immediately.
    //   - If peer data is NOT available yet: defers events until SetPeerInfo provides real data.
    // On subsequent calls with same NpId: returns existing connId, re-fires events.
    //   Re-firing is needed because SocketState entries created AFTER the initial
    //   ESTABLISHED event (e.g., via DeferredForceStart) miss it and get stuck at state 4.

    // Extract NpId string for dedup key (first 16 bytes, null-terminated)
    std::string npid_key;
    if (npId) {
        npid_key = std::string(reinterpret_cast<const char*>(npId), 16);
        // Trim to null terminator
        auto pos = npid_key.find('\0');
        if (pos != std::string::npos) {
            npid_key.resize(pos);
        }
    }

    bool need_events = false;
    s32 cid = 0;

    {
        std::lock_guard<std::mutex> lock(s_mutex);

        // Check if we already have a connection for this NpId
        auto existing = s_npid_to_conn.find(npid_key);
        if (existing != s_npid_to_conn.end()) {
            cid = existing->second;
            if (connId) {
                *connId = cid;
            }
            // Re-fire events on idempotent calls: SocketState entries created
            // AFTER the initial ESTABLISHED event need fresh events to set +0xac.
            // Rate-limit to one re-fire per 150ms window — all entries in one tick
            // pass call ActivateConnection within <1ms, so only ONE re-fire per batch.
            auto conn_it = s_connections.find(cid);
            if (conn_it != s_connections.end() && conn_it->second.events_fired) {
                auto now = std::chrono::steady_clock::now();
                auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - conn_it->second.last_event_time)
                                 .count();
                if (since >= 150) {
                    conn_it->second.last_event_time = now;
                    need_events = true;
                }
            }
        } else {

            // First activation for this NpId — assign new connId
            cid = s_next_conn_id++;
            if (connId) {
                *connId = cid;
            }

            // Look for a SPECIFIC peer matching this NpId (by online_id).
            // If the peer hasn't joined yet, we defer events until SetPeerInfo resolves it.
            NpSignalingPeerInfo matched_peer{};
            for (const auto& [mid, pi] : s_peers) {
                if (pi.online_id == npid_key && pi.addr != 0) {
                    matched_peer = pi;
                    break;
                }
            }

            ConnectionInfo ci;
            ci.conn_id = cid;
            ci.ctx_id = ctxId;
            ci.npid_str = npid_key;

            if (matched_peer.addr != 0) {
                // Peer already known — store real data, fire events immediately
                ci.addr = matched_peer.addr;
                ci.port = matched_peer.port;
                ci.status = ORBIS_NP_SIGNALING_CONN_STATUS_ACTIVE;
                ci.events_fired = true;
                ci.last_event_time = std::chrono::steady_clock::now();
                need_events = true;

                LOG_INFO(Lib_NpSignaling,
                         "ActivateConnection: NEW ctxId={} npId='{}' -> connId={} "
                         "peer_addr={:#x} ({}.{}.{}.{}) peer_port={} (peer already known)",
                         ctxId, npid_key, cid, matched_peer.addr,
                         (ntohl(matched_peer.addr) >> 24) & 0xff,
                         (ntohl(matched_peer.addr) >> 16) & 0xff,
                         (ntohl(matched_peer.addr) >> 8) & 0xff, ntohl(matched_peer.addr) & 0xff,
                         ntohs(matched_peer.port));
            } else {
                // Peer NOT yet known — defer events until SetPeerInfo resolves this NpId
                ci.addr = 0;
                ci.port = 0;
                ci.status = ORBIS_NP_SIGNALING_CONN_STATUS_PENDING;
                ci.events_fired = false;

                LOG_INFO(Lib_NpSignaling,
                         "ActivateConnection: NEW ctxId={} npId='{}' -> connId={} "
                         "(peer not yet known, events DEFERRED until SetPeerInfo)",
                         ctxId, npid_key, cid);
            }

            s_connections[cid] = ci;
            if (!npid_key.empty()) {
                s_npid_to_conn[npid_key] = cid;
            }
        } // end else (new connection)
    } // s_mutex released here BEFORE event delivery

    if (need_events) {
        // Fire ESTABLISHED+ACTIVE events (only when peer is already known)
        // MUST be called WITHOUT s_mutex held — DeliverSignalingEventForCtx locks s_mutex
        DeliverSignalingEventForCtx(ctxId, cid, ORBIS_NP_SIGNALING_EVENT_ESTABLISHED, 200);
        DeliverSignalingEventForCtx(ctxId, cid, ORBIS_NP_SIGNALING_EVENT_MUTUAL_ACTIVATED, 400);
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingActivateConnectionA() {
    LOG_ERROR(Lib_NpSignaling, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingGetConnectionStatus(s32 ctxId, s32 connId, s32* connStatus,
                                                   u32* peerAddr, u16* peerPort) {
    // CONNID-AWARE: SocketState state 5 passes a SPECIFIC connId (+0x10)
    // and expects the MATCHING peer's addr/port.
    // NOTE: The SocketState does NOT poll — it calls this ONCE per state visit.
    // If status != ACTIVE(2), the SocketState goes to error state 0xb.

    std::lock_guard<std::mutex> lock(s_mutex);

    // Diagnostic: log every call to trace SocketState behavior
    static int s_call_count = 0;
    s_call_count++;

    auto it = s_connections.find(connId);
    if (it != s_connections.end()) {
        s32 status = it->second.status;
        u32 addr = it->second.addr;
        u16 port = it->second.port;

        // If connection is still pending (peer not yet joined), report as inactive
        // so the SocketState machine keeps polling instead of using stale data
        if (status == ORBIS_NP_SIGNALING_CONN_STATUS_PENDING) {
            if (connStatus)
                *connStatus = ORBIS_NP_SIGNALING_CONN_STATUS_INACTIVE;
            if (peerAddr)
                *peerAddr = 0;
            if (peerPort)
                *peerPort = 0;
            LOG_WARNING(Lib_NpSignaling,
                        "GetConnectionStatus: connId={} PENDING -> returning INACTIVE "
                        "(call#={} npid='{}')",
                        connId, s_call_count, it->second.npid_str);
        } else {
            if (connStatus)
                *connStatus = status;
            if (peerAddr)
                *peerAddr = addr;
            if (peerPort)
                *peerPort = port;
            LOG_INFO(Lib_NpSignaling,
                     "GetConnectionStatus: connId={} status={} addr={:#x} ({}.{}.{}.{}) "
                     "port={} npid='{}' (call#={})",
                     connId, status, addr, (ntohl(addr) >> 24) & 0xff, (ntohl(addr) >> 16) & 0xff,
                     (ntohl(addr) >> 8) & 0xff, ntohl(addr) & 0xff, port ? ntohs(port) : 0,
                     it->second.npid_str, s_call_count);
        }
        return ORBIS_OK;
    }

    // Fallback: try any active peer (backwards compat with old synthetic flow)
    NpSignalingPeerInfo peer{};
    for (const auto& [mid, pi] : s_peers) {
        if (pi.status == ORBIS_NP_SIGNALING_CONN_STATUS_ACTIVE) {
            peer = pi;
            break;
        }
    }

    if (peer.status == ORBIS_NP_SIGNALING_CONN_STATUS_ACTIVE) {
        if (connStatus)
            *connStatus = ORBIS_NP_SIGNALING_CONN_STATUS_ACTIVE;
        if (peerAddr)
            *peerAddr = peer.addr;
        if (peerPort)
            *peerPort = peer.port;
        LOG_WARNING(Lib_NpSignaling,
                    "GetConnectionStatus: connId={} not found, using fallback peer "
                    "addr={:#x} port={}",
                    connId, peer.addr, ntohs(peer.port));
    } else {
        if (connStatus)
            *connStatus = ORBIS_NP_SIGNALING_CONN_STATUS_INACTIVE;
        LOG_WARNING(Lib_NpSignaling, "GetConnectionStatus: connId={} not found, no active peer",
                    connId);
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingDeactivateConnection(s32 ctxId, s32 connId) {
    std::lock_guard<std::mutex> lock(s_mutex);

    auto it = s_connections.find(connId);
    if (it != s_connections.end()) {
        LOG_INFO(Lib_NpSignaling,
                 "DeactivateConnection: ctxId={} connId={} npid='{}' "
                 "addr={:#x} status={} events_fired={}",
                 ctxId, connId, it->second.npid_str, it->second.addr, it->second.status,
                 it->second.events_fired);
        // Remove from NpId dedup map
        if (!it->second.npid_str.empty()) {
            s_npid_to_conn.erase(it->second.npid_str);
        }
        s_connections.erase(it);
    } else {
        LOG_WARNING(Lib_NpSignaling, "DeactivateConnection: ctxId={} connId={} NOT FOUND", ctxId,
                    connId);
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingDeleteContext(s32 ctxId) {
    LOG_INFO(Lib_NpSignaling, "called ctxId={}", ctxId);

    std::lock_guard<std::mutex> lock(s_mutex);
    s_contexts.erase(ctxId);

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingCancelPeerNetInfo() {
    LOG_ERROR(Lib_NpSignaling, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingGetConnectionFromNpId() {
    LOG_ERROR(Lib_NpSignaling, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingGetConnectionFromPeerAddress() {
    LOG_ERROR(Lib_NpSignaling, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingGetConnectionFromPeerAddressA() {
    LOG_ERROR(Lib_NpSignaling, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingGetConnectionInfo(s32 ctxId, s32 connId, s32 infoType, void* info) {
    LOG_INFO(Lib_NpSignaling, "called ctxId={} connId={} infoType={}", ctxId, connId, infoType);

    if (!info) {
        LOG_ERROR(Lib_NpSignaling, "GetConnectionInfo: null output pointer");
        return ORBIS_OK;
    }

    // Try connId-aware lookup first, fallback to any peer
    ConnectionInfo* ci = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_connections.find(connId);
        if (it != s_connections.end()) {
            ci = &it->second;
        }
    }

    NpSignalingPeerInfo peer{};
    if (!ci) {
        peer = GetAnyActivePeer();
    }

    switch (infoType) {
    case ORBIS_NP_SIGNALING_CONN_INFO_RTT:
        *reinterpret_cast<s32*>(info) = 10;
        break;

    case ORBIS_NP_SIGNALING_CONN_INFO_BANDWIDTH:
        *reinterpret_cast<s32*>(info) = 10000000;
        break;

    case ORBIS_NP_SIGNALING_CONN_INFO_PEER_NP_ID:
        std::memset(info, 0, 36);
        if (ci && !ci->npid_str.empty()) {
            std::strncpy(reinterpret_cast<char*>(info), ci->npid_str.c_str(), 15);
        } else {
            std::strncpy(reinterpret_cast<char*>(info), "shadPS4_peer", 15);
        }
        break;

    case ORBIS_NP_SIGNALING_CONN_INFO_PEER_ADDR: {
        u32 out_addr;
        u16 out_port;
        const char* src;
        if (ci) {
            out_addr = ci->addr;
            out_port = ci->port;
            src = "connId-lookup";
        } else if (peer.status == ORBIS_NP_SIGNALING_CONN_STATUS_ACTIVE) {
            out_addr = peer.addr;
            out_port = peer.port;
            src = "fallback-peer";
        } else {
            out_addr = s_local_addr;
            out_port = s_local_port;
            src = "local-addr";
        }
        *reinterpret_cast<u32*>(info) = out_addr;
        *reinterpret_cast<u16*>(reinterpret_cast<u8*>(info) + 4) = out_port;
        LOG_INFO(Lib_NpSignaling,
                 "GetConnectionInfo PEER_ADDR: connId={} addr={:#x} ({}.{}.{}.{}) "
                 "port={} source={}",
                 connId, out_addr, (ntohl(out_addr) >> 24) & 0xff, (ntohl(out_addr) >> 16) & 0xff,
                 (ntohl(out_addr) >> 8) & 0xff, ntohl(out_addr) & 0xff, ntohs(out_port), src);
        break;
    }

    case ORBIS_NP_SIGNALING_CONN_INFO_MAPPED_ADDR: {
        u32 out_addr;
        u16 out_port;
        const char* src;
        if (ci) {
            out_addr = ci->addr;
            out_port = ci->port;
            src = "connId-lookup";
        } else if (peer.status == ORBIS_NP_SIGNALING_CONN_STATUS_ACTIVE) {
            out_addr = peer.addr;
            out_port = peer.port;
            src = "fallback-peer";
        } else {
            out_addr = s_local_addr;
            out_port = s_local_port;
            src = "local-addr";
        }
        *reinterpret_cast<u32*>(info) = out_addr;
        *reinterpret_cast<u16*>(reinterpret_cast<u8*>(info) + 4) = out_port;
        LOG_INFO(Lib_NpSignaling,
                 "GetConnectionInfo MAPPED_ADDR: connId={} addr={:#x} ({}.{}.{}.{}) "
                 "port={} source={}",
                 connId, out_addr, (ntohl(out_addr) >> 24) & 0xff, (ntohl(out_addr) >> 16) & 0xff,
                 (ntohl(out_addr) >> 8) & 0xff, ntohl(out_addr) & 0xff, ntohs(out_port), src);
        break;
    }

    case ORBIS_NP_SIGNALING_CONN_INFO_PACKET_LOSS:
        *reinterpret_cast<u32*>(info) = 0;
        break;

    default:
        LOG_WARNING(Lib_NpSignaling, "GetConnectionInfo: unknown infoType={}", infoType);
        break;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingGetConnectionInfoA() {
    LOG_ERROR(Lib_NpSignaling, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingGetConnectionStatistics() {
    LOG_ERROR(Lib_NpSignaling, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingGetContextOption() {
    LOG_ERROR(Lib_NpSignaling, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingGetLocalNetInfo(s32 ctxId, OrbisNpSignalingNetInfo* info) {
    LOG_INFO(Lib_NpSignaling, "called ctxId={}", ctxId);

    if (info) {
        std::memset(info, 0, sizeof(*info));
        info->size = sizeof(OrbisNpSignalingNetInfo); // 0x18
        info->localAddr = s_local_addr;
        info->mappedAddr = s_local_addr;
        info->natStatus = 2; // NAT Type 2 (moderate)
        LOG_INFO(Lib_NpSignaling,
                 "GetLocalNetInfo: localAddr={:#x} ({}.{}.{}.{}) mappedAddr={:#x} "
                 "natStatus=2 localPort={}",
                 s_local_addr, (ntohl(s_local_addr) >> 24) & 0xff,
                 (ntohl(s_local_addr) >> 16) & 0xff, (ntohl(s_local_addr) >> 8) & 0xff,
                 ntohl(s_local_addr) & 0xff, s_local_addr, ntohs(s_local_port));
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingGetMemoryInfo() {
    LOG_ERROR(Lib_NpSignaling, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingGetPeerNetInfo() {
    LOG_ERROR(Lib_NpSignaling, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingGetPeerNetInfoA() {
    LOG_ERROR(Lib_NpSignaling, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingGetPeerNetInfoResult() {
    LOG_ERROR(Lib_NpSignaling, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingSetContextOption() {
    LOG_ERROR(Lib_NpSignaling, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpSignalingTerminateConnection() {
    LOG_ERROR(Lib_NpSignaling, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("0UvTFeomAUM", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingActivateConnection);
    LIB_FUNCTION("ZPLavCKqAB0", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingActivateConnectionA);
    LIB_FUNCTION("X1G4kkN2R-8", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingCancelPeerNetInfo);
    LIB_FUNCTION("5yYjEdd4t8Y", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingCreateContext);
    LIB_FUNCTION("dDLNFdY8dws", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingCreateContextA);
    LIB_FUNCTION("6UEembipgrM", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingDeactivateConnection);
    LIB_FUNCTION("hx+LIg-1koI", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingDeleteContext);
    LIB_FUNCTION("GQ0hqmzj0F4", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingGetConnectionFromNpId);
    LIB_FUNCTION("CkPxQjSm018", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingGetConnectionFromPeerAddress);
    LIB_FUNCTION("B7cT9aVby7A", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingGetConnectionFromPeerAddressA);
    LIB_FUNCTION("AN3h0EBSX7A", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingGetConnectionInfo);
    LIB_FUNCTION("rcylknsUDwg", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingGetConnectionInfoA);
    LIB_FUNCTION("C6ZNCDTj00Y", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingGetConnectionStatistics);
    LIB_FUNCTION("bD-JizUb3JM", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingGetConnectionStatus);
    LIB_FUNCTION("npU5V56id34", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingGetContextOption);
    LIB_FUNCTION("U8AQMlOFBc8", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingGetLocalNetInfo);
    LIB_FUNCTION("tOpqyDyMje4", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingGetMemoryInfo);
    LIB_FUNCTION("zFgFHId7vAE", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingGetPeerNetInfo);
    LIB_FUNCTION("Shr7bZq8QHY", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingGetPeerNetInfoA);
    LIB_FUNCTION("2HajCEGgG4s", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingGetPeerNetInfoResult);
    LIB_FUNCTION("3KOuC4RmZZU", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingInitialize);
    LIB_FUNCTION("IHRDvZodPYY", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingSetContextOption);
    LIB_FUNCTION("NPhw0UXaNrk", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingTerminate);
    LIB_FUNCTION("b4qaXPzMJxo", "libSceNpSignaling", 1, "libSceNpSignaling",
                 sceNpSignalingTerminateConnection);
}

} // namespace Libraries::Np::NpSignaling
