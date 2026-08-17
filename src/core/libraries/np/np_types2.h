// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/libraries/np/np_types.h"
#include "core/libraries/np/np_matching2/np_matching2.h"
#include "core/libraries/rtc/rtc.h"
#include "core/libraries/system/userservice.h"

namespace Libraries::Np::NpMatching2 {

// Additional type definitions not in np_matching2.h
using OrbisNpMatching2SessionType = u8;
using OrbisNpMatching2CastType = u8;

// Additional event constants not in np_matching2.h
constexpr u16 ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_ROOM_MEMBER_DATA_EXTERNAL_LIST = 0x0003;
constexpr u16 ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_ROOM_DATA_EXTERNAL_LIST = 0x0005;
constexpr u16 ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_USER_INFO_LIST = 0x0008;
constexpr u16 ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_ROOM_MEMBER_DATA_EXTERNAL_LIST_A = 0x7003;
constexpr u16 ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_ROOM_DATA_EXTERNAL_LIST_A = 0x7005;
constexpr u16 ORBIS_NP_MATCHING2_ROOM_MSG_EVENT_MESSAGE = 0x2102;

// Room message destination (not in np_matching2_types.h)
union OrbisNpMatching2RoomMessageDestination {
    OrbisNpMatching2RoomMemberId unicastTarget;

    struct {
        OrbisNpMatching2RoomMemberId* memberId;
        u64 memberIdNum;
    } multicastTarget;
};
static_assert(sizeof(OrbisNpMatching2RoomMessageDestination) == 0x10);

// Send room message request (not in np_matching2_types.h)
struct OrbisNpMatching2SendRoomMessageRequest {
    OrbisNpMatching2RoomId roomId;
    OrbisNpMatching2CastType castType;
    u8 padding[3];
    OrbisNpMatching2RoomMessageDestination dst;
    const void* msg;
    u32 msgLen;
    s32 option;
};
static_assert(sizeof(OrbisNpMatching2SendRoomMessageRequest) == 0x30);

// Room message info (not in np_matching2_types.h)
struct OrbisNpMatching2RoomMessageInfo {
    bool filtered;
    OrbisNpMatching2CastType castType;
    u8 padding[2];
    OrbisNpMatching2RoomMessageDestination* dst;
    Libraries::Np::OrbisNpId* srcMember;
    void* msg;
    u32 msgLen;
};

// Room message info A (not in np_matching2_types.h)
struct OrbisNpMatching2RoomMessageInfoA {
    bool filtered;
    OrbisNpMatching2CastType castType;
    u8 padding[2];
    OrbisNpMatching2RoomMessageDestination* dst;
    Libraries::Np::OrbisNpPeerAddressA* srcMember;
    Libraries::Np::OrbisNpOnlineId* srcOnlineId;
    void* msg;
    u32 msgLen;
};

// Room-external room member information (not in np_matching2_types.h)
struct OrbisNpMatching2RoomMemberDataExternal {
    OrbisNpMatching2RoomMemberDataExternal* next;
    Libraries::Np::OrbisNpId npId;
    Libraries::Rtc::OrbisRtcTick joinDate;
    OrbisNpMatching2Role role;
    u8 padding[7];
};

// Room-external room member information A (not in np_matching2_types.h)
struct OrbisNpMatching2RoomMemberDataExternalA {
    OrbisNpMatching2RoomMemberDataExternalA* next;
    Libraries::Np::OrbisNpPeerAddressA user;
    Libraries::Np::OrbisNpOnlineId onlineId;
    Libraries::Rtc::OrbisRtcTick joinDate;
    OrbisNpMatching2Role role;
    u8 padding[7];
};

// GetRoomDataExternalList request (not in np_matching2_types.h)
struct OrbisNpMatching2GetRoomDataExternalListRequest {
    OrbisNpMatching2RoomId* roomId;
    u64 roomIdNum;
    const OrbisNpMatching2AttributeId* attrId;
    u64 attrIdNum;
};

// GetRoomDataExternalList response (not in np_matching2_types.h)
struct OrbisNpMatching2GetRoomDataExternalListResponse {
    OrbisNpMatching2RoomDataExternal* roomDataExternal;
    u64 roomDataExternalNum;
};

// GetRoomDataExternalList response A (not in np_matching2_types.h)
struct OrbisNpMatching2GetRoomDataExternalListResponseA {
    OrbisNpMatching2RoomDataExternalA* roomDataExternal;
    u64 roomDataExternalNum;
};

// GetRoomMemberDataExternalList request (not in np_matching2_types.h)
struct OrbisNpMatching2GetRoomMemberDataExternalListRequest {
    OrbisNpMatching2RoomId roomId;
};

// GetRoomMemberDataExternalList response (not in np_matching2_types.h)
struct OrbisNpMatching2GetRoomMemberDataExternalListResponse {
    OrbisNpMatching2RoomMemberDataExternal* roomMemberDataExternal;
    u64 roomMemberDataExternalNum;
};

// GetRoomMemberDataExternalList response A (not in np_matching2_types.h)
struct OrbisNpMatching2GetRoomMemberDataExternalListResponseA {
    OrbisNpMatching2RoomMemberDataExternalA* roomMemberDataExternal;
    u64 roomMemberDataExternalNum;
};

// Session a user is currently joined to (not in np_matching2_types.h)
struct OrbisNpMatching2JoinedSessionInfo {
    OrbisNpMatching2SessionType sessionType;
    u8 padding[1];
    OrbisNpMatching2ServerId serverId;
    OrbisNpMatching2WorldId worldId;
    OrbisNpMatching2LobbyId lobbyId;
    OrbisNpMatching2RoomId roomId;
    Libraries::Rtc::OrbisRtcTick joinDate;
};

// User information (not in np_matching2_types.h)
struct OrbisNpMatching2UserInfo {
    OrbisNpMatching2UserInfo* next;
    Libraries::Np::OrbisNpId npId;
    OrbisNpMatching2BinAttr* userBinAttr;
    u64 userBinAttrNum;
    OrbisNpMatching2JoinedSessionInfo joinedSessionInfo;
    u64 joinedSessionInfoNum;
};

// User information A (not in np_matching2_types.h)
struct OrbisNpMatching2UserInfoA {
    OrbisNpMatching2UserInfoA* next;
    Libraries::Np::OrbisNpPeerAddressA user;
    Libraries::Np::OrbisNpOnlineId userOnlineId;
    OrbisNpMatching2BinAttr* userBinAttr;
    u64 userBinAttrNum;
    OrbisNpMatching2JoinedSessionInfo joinedSessionInfo;
    u64 joinedSessionInfoNum;
};

// GetUserInfoList request (not in np_matching2_types.h)
struct OrbisNpMatching2GetUserInfoListRequest {
    OrbisNpMatching2ServerId serverId;
    u8 padding[6];
    Libraries::Np::OrbisNpId* npId;
    u64 npIdNum;
    const OrbisNpMatching2AttributeId* attrId;
    u64 attrIdNum;
    s32 option;
};

// GetUserInfoList response (not in np_matching2_types.h)
struct OrbisNpMatching2GetUserInfoListResponse {
    OrbisNpMatching2UserInfo* userInfo;
    u64 userInfoNum;
};

// GetUserInfoList response A (not in np_matching2_types.h)
struct OrbisNpMatching2GetUserInfoListResponseA {
    OrbisNpMatching2UserInfoA* userInfo;
    u64 userInfoNum;
};

} // namespace Libraries::Np::NpMatching2

namespace Libraries::Np::NpSignaling {

using OrbisNpSignalingContextId = s32;
using OrbisNpSignalingConnectionId = s32;
using OrbisNpSignalingRequestId = u32;

// Signaling handler callback function
using OrbisNpSignalingHandler = PS4_SYSV_ABI void (*)(u32 ctxId, u32 connId, s32 event,
                                                      s32 errorCode, void* userArg);

// Signaling event
constexpr s32 ORBIS_NP_SIGNALING_EVENT_DEAD = 0;
constexpr s32 ORBIS_NP_SIGNALING_EVENT_ESTABLISHED = 1;
constexpr s32 ORBIS_NP_SIGNALING_EVENT_NETINFO_ERROR = 2;
constexpr s32 ORBIS_NP_SIGNALING_EVENT_NETINFO_RESULT = 3;
constexpr s32 ORBIS_NP_SIGNALING_EVENT_PEER_ACTIVATED = 10;
constexpr s32 ORBIS_NP_SIGNALING_EVENT_PEER_DEACTIVATED = 11;
constexpr s32 ORBIS_NP_SIGNALING_EVENT_MUTUAL_ACTIVATED = 12;

// Connection status
constexpr s32 ORBIS_NP_SIGNALING_CONN_STATUS_INACTIVE = 0;
constexpr s32 ORBIS_NP_SIGNALING_CONN_STATUS_PENDING = 1;
constexpr s32 ORBIS_NP_SIGNALING_CONN_STATUS_ACTIVE = 2;

// Type of connection information to acquire
constexpr s32 ORBIS_NP_SIGNALING_CONN_INFO_RTT = 1;
constexpr s32 ORBIS_NP_SIGNALING_CONN_INFO_BANDWIDTH = 2;
constexpr s32 ORBIS_NP_SIGNALING_CONN_INFO_PEER_NP_ID = 3;
constexpr s32 ORBIS_NP_SIGNALING_CONN_INFO_PEER_ADDR = 4;
constexpr s32 ORBIS_NP_SIGNALING_CONN_INFO_MAPPED_ADDR = 5;
constexpr s32 ORBIS_NP_SIGNALING_CONN_INFO_PACKET_LOSS = 6;
constexpr s32 ORBIS_NP_SIGNALING_CONN_INFO_PEER_ADDRESS_A = 7;

// Context option
constexpr s32 ORBIS_NP_SIGNALING_CONTEXT_OPTION_FLAG = 1;

// Network information
struct OrbisNpSignalingNetInfo {
    u64 size;
    u32 localAddr;
    u32 mappedAddr;
    s32 natStatus;
    u32 _pad_14;
};
static_assert(sizeof(OrbisNpSignalingNetInfo) == 0x18);

// Account-id / platform pair
struct OrbisNpSignalingAccountPlatformPair {
    u64 accountId;
    u32 platformType;
    u32 _pad_0c;
};
static_assert(sizeof(OrbisNpSignalingAccountPlatformPair) == 0x10);

// Memory information
struct OrbisNpSignalingMemoryInfo {
    u64 currentInUse;
    u64 peakInUse;
    u64 maxSystemSize;
};
static_assert(sizeof(OrbisNpSignalingMemoryInfo) == 0x18);

// Connection statistics
struct OrbisNpSignalingConnectionStatistics {
    u32 peakConnectionCount;
    u32 activeConnectionCount;
    u32 transientConnectionCount;
    u32 establishedConnectionCount;
};
static_assert(sizeof(OrbisNpSignalingConnectionStatistics) == 0x10);

} // namespace Libraries::Np::NpSignaling
