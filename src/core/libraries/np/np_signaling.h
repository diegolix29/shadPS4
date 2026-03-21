// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Np::NpSignaling {

// Functional NpSignaling HLE for Bloodborne multiplayer.
// The SocketState machine (SocketState_tick at 0x10d8bb0) calls these functions
// to manage P2P connections. The game's callback at NpSignaling_callback (0x10c10d0)
// processes events and pushes them to the SessionOwner's event queue for processing
// by the SocketState handler.

// Bloodborne RE confirms 5 params at 0x10c099c.
using OrbisNpSignalingHandler = PS4_SYSV_ABI void (*)(u32 ctxId, u32 connId, s32 event,
                                                      s32 errorCode, void* userArg);

constexpr s32 ORBIS_NP_SIGNALING_EVENT_DEAD = 0;
constexpr s32 ORBIS_NP_SIGNALING_EVENT_ESTABLISHED = 1;
constexpr s32 ORBIS_NP_SIGNALING_EVENT_NETINFO_ERROR = 2;
constexpr s32 ORBIS_NP_SIGNALING_EVENT_NETINFO_RESULT = 3;
constexpr s32 ORBIS_NP_SIGNALING_EVENT_PEER_ACTIVATED = 10;
constexpr s32 ORBIS_NP_SIGNALING_EVENT_PEER_DEACTIVATED = 11;
constexpr s32 ORBIS_NP_SIGNALING_EVENT_MUTUAL_ACTIVATED = 12;

constexpr s32 ORBIS_NP_SIGNALING_CONN_STATUS_INACTIVE = 0;
constexpr s32 ORBIS_NP_SIGNALING_CONN_STATUS_PENDING = 1;
constexpr s32 ORBIS_NP_SIGNALING_CONN_STATUS_ACTIVE = 2;

constexpr s32 ORBIS_NP_SIGNALING_CONN_INFO_RTT = 1;
constexpr s32 ORBIS_NP_SIGNALING_CONN_INFO_BANDWIDTH = 2;
constexpr s32 ORBIS_NP_SIGNALING_CONN_INFO_PEER_NP_ID = 3;
constexpr s32 ORBIS_NP_SIGNALING_CONN_INFO_PEER_ADDR = 4;
constexpr s32 ORBIS_NP_SIGNALING_CONN_INFO_MAPPED_ADDR = 5;
constexpr s32 ORBIS_NP_SIGNALING_CONN_INFO_PACKET_LOSS = 6;

struct OrbisNpSignalingNetInfo {
    u64 size;       // +0x00: must be sizeof(OrbisNpSignalingNetInfo) = 0x18
    u32 localAddr;  // +0x08
    u32 mappedAddr; // +0x0C
    s32 natStatus;  // +0x10
    u32 _pad_14;    // +0x14
};
static_assert(sizeof(OrbisNpSignalingNetInfo) == 0x18);

// --- Shared peer info (set by NpMatching2 HLE, read by NpSignaling HLE) ---
// Both HLE modules need access to peer connection info.
struct NpSignalingPeerInfo {
    u32 addr = 0; // IPv4 in network byte order
    u16 port = 0; // port in network byte order
    s32 status = ORBIS_NP_SIGNALING_CONN_STATUS_INACTIVE;
    u16 member_id = 0;
    std::string online_id; // PSN online ID for NpId matching
};

// Set peer info from NpMatching2 HLE when a room join occurs.
// online_id is used to match pending ActivateConnection requests and fire deferred events.
void SetPeerInfo(u16 member_id, u32 addr, u16 port, const std::string& online_id = "");

// Get peer info for signaling status queries
NpSignalingPeerInfo GetPeerInfo(u16 member_id = 0);

// Get any active peer (for when we don't know the member_id)
NpSignalingPeerInfo GetAnyActivePeer();

// Set/get local signaling address (called by NpMatching2 HLE on context start)
void SetLocalAddr(u32 addr, u16 port);
u32 GetLocalAddr();
u16 GetLocalPort();

// --- PS4 API functions ---

s32 PS4_SYSV_ABI sceNpSignalingActivateConnection(s32 ctxId, void* npId, s32* connId);
s32 PS4_SYSV_ABI sceNpSignalingActivateConnectionA();
s32 PS4_SYSV_ABI sceNpSignalingCancelPeerNetInfo();
s32 PS4_SYSV_ABI sceNpSignalingCreateContext(s32 param_1, void* param_2, void* param_3,
                                             s32* context_id);
s32 PS4_SYSV_ABI sceNpSignalingCreateContextA();
s32 PS4_SYSV_ABI sceNpSignalingDeactivateConnection(s32 ctxId, s32 connId);
s32 PS4_SYSV_ABI sceNpSignalingDeleteContext(s32 ctxId);
s32 PS4_SYSV_ABI sceNpSignalingGetConnectionFromNpId();
s32 PS4_SYSV_ABI sceNpSignalingGetConnectionFromPeerAddress();
s32 PS4_SYSV_ABI sceNpSignalingGetConnectionFromPeerAddressA();
s32 PS4_SYSV_ABI sceNpSignalingGetConnectionInfo(s32 ctxId, s32 connId, s32 infoType, void* info);
s32 PS4_SYSV_ABI sceNpSignalingGetConnectionInfoA();
s32 PS4_SYSV_ABI sceNpSignalingGetConnectionStatistics();
s32 PS4_SYSV_ABI sceNpSignalingGetConnectionStatus(s32 ctxId, s32 connId, s32* connStatus,
                                                   u32* peerAddr, u16* peerPort);
s32 PS4_SYSV_ABI sceNpSignalingGetContextOption();
s32 PS4_SYSV_ABI sceNpSignalingGetLocalNetInfo(s32 ctxId, OrbisNpSignalingNetInfo* info);
s32 PS4_SYSV_ABI sceNpSignalingGetMemoryInfo();
s32 PS4_SYSV_ABI sceNpSignalingGetPeerNetInfo();
s32 PS4_SYSV_ABI sceNpSignalingGetPeerNetInfoA();
s32 PS4_SYSV_ABI sceNpSignalingGetPeerNetInfoResult();
s32 PS4_SYSV_ABI sceNpSignalingInitialize();
s32 PS4_SYSV_ABI sceNpSignalingSetContextOption();
s32 PS4_SYSV_ABI sceNpSignalingTerminate();
s32 PS4_SYSV_ABI sceNpSignalingTerminateConnection();

void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::Np::NpSignaling