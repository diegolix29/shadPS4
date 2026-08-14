// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/bloodborne_re.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/cpu_patches.h"
#include "core/debugger.h"
#include "core/file_format/psf.h"

namespace Core::Bloodborne {
namespace {

enum class TraceKind : u8 {
    SosCondition,
    SosValidityDispatch,
    SosValidityCode,
    SosEvent,
    MatchingEvent,
    MatchingSignal,
    SummonRequest,
    GetSosRequest,
    GetSosArea,
    GetSosInsert,
    SosStatus,
    SosMessenger,
    ActionFlags,
    UseItem,
    UseItemExecution,
    GoodsActionSubmit,
    GoodsParamLookup,
    BellAvailability,
    NetworkAreaRegion,
    MultiplayerInsert,
    SosPlacement,
    GlobalState,
    ReceivedPlacementConditional,
    ReceivedPlacementSet,
    PlacementSelect,
    WarpCommand,
    WarpResolved,
    WarpLocalApply,
    StageWarp,
    RespawnPointWarp,
    StageWarpDescriptor,
    StageWarpAcknowledge,
    StageWarpRespawnResolve,
    StageWarpTransformResolve,
    StageWarpPlacement,
    SummonPoint,
    MapReload,
};

struct TraceSite {
    std::string_view name;
    u64 offset;
    TraceKind kind;
    std::array<u8, 16> prologue;
    u8 prologue_size;
};

constexpr std::array<u8, 16> StandardR15Prologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
constexpr std::array<u8, 16> StandardR14Prologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x56};
constexpr std::array<u8, 16> StandardRbxPrologue{0x55, 0x48, 0x89, 0xE5, 0x53};
constexpr std::array<u8, 16> ReturnAndPadding{0xC3, 0x66, 0x66, 0x66, 0x66, 0x66,
                                              0x66, 0x2E, 0x0F, 0x1F, 0x84};
constexpr u64 GlobalStatePointerOffset = 0x05556678;
constexpr u64 BeckoningAreaComparisonOffset = 0x0157F8C8;
constexpr std::array<u8, 6> BeckoningAreaComparison{0x0F, 0x87, 0x90, 0x00, 0x00, 0x00};
constexpr std::array<u8, 6> BeckoningSignedAreaComparison{0x0F, 0x8F, 0x90, 0x00, 0x00, 0x00};
constexpr u64 BeckoningAreaFlagResultOffset = 0x0157F960;
constexpr std::array<u8, 2> BeckoningAreaFlagResult{0x34, 0x01};
constexpr std::array<u8, 2> BeckoningAllowedAreaFlagResult{0x31, 0xC0};
constexpr u64 ActiveBellAreaComparisonOffset = 0x015068BB;
constexpr std::array<u8, 2> ActiveBellAreaComparison{0x77, 0x4A};
constexpr std::array<u8, 2> ActiveBellSignedAreaComparison{0x7F, 0x4A};
constexpr u64 ActiveBellAreaFlagResultOffset = 0x015068FC;
constexpr std::array<u8, 5> ActiveBellAreaFlagResult{0x88, 0xC3, 0x80, 0xF3, 0x01};
constexpr std::array<u8, 5> ActiveBellAllowedAreaFlagResult{0xB3, 0x01, 0x90, 0x90, 0x90};
constexpr u64 SosStatusAreaRestrictionOffset = 0x018700D3;
constexpr std::array<u8, 16> SosStatusAreaRestriction{
    0x84, 0xC0, 0x41, 0xBD, 0xFF, 0xFF, 0xFF, 0xFF, 0x44, 0x0F, 0x44, 0xEB, 0x41, 0xC1, 0xED, 0x1F,
};
constexpr std::array<u8, 16> SosStatusAllowedArea{
    0x45, 0x31, 0xED, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
};

constexpr auto Sites = std::to_array<TraceSite>({
    {"IsValidSos.Update", 0x01308DC0, TraceKind::SosCondition, StandardR15Prologue, 6},
    {"IsValidSos.NativeValidityDispatch",
     0x01308DF9,
     TraceKind::SosValidityDispatch,
     {0x48, 0x8B, 0x03, 0x48, 0x89, 0xDF},
     6},
    {"IsValidSos.ValidityCode",
     0x01308E20,
     TraceKind::SosValidityCode,
     {0x49, 0x63, 0xCF, 0x48, 0x69, 0xC9, 0x67, 0x66, 0x66, 0x66},
     10},
    {"IsValidSos.Condition", 0x013091B0, TraceKind::SosCondition, StandardR14Prologue, 6},
    {"Condition.SelfSos", 0x0130B030, TraceKind::SosCondition, StandardR15Prologue, 6},
    {"Condition.SOS", 0x0130C590, TraceKind::SosCondition, StandardR15Prologue, 6},
    {"Condition.SosMenu", 0x0130CDE0, TraceKind::SosCondition, StandardR15Prologue, 6},

    {"OnEvent.CallSOS.Dispatch",
     0x01389A3B,
     TraceKind::SosEvent,
     {0xBE, 0xDA, 0x0F, 0x00, 0x00, 0xBA, 0x01, 0x00, 0x00, 0x00},
     10},
    {"OnEvent.CallBlackSOS.Dispatch",
     0x01389A6B,
     TraceKind::SosEvent,
     {0xBE, 0xDA, 0x0F, 0x00, 0x00, 0xBA, 0x02, 0x00, 0x00, 0x00},
     10},
    {"OnEvent.CallDragonewtSOS.Dispatch",
     0x01389A9B,
     TraceKind::SosEvent,
     {0xBE, 0xDA, 0x0F, 0x00, 0x00, 0xBA, 0x03, 0x00, 0x00, 0x00},
     10},
    {"Call.WhiteSos.Dispatch",
     0x01389AE2,
     TraceKind::SosEvent,
     {0x31, 0xF6, 0xBA, 0xEA, 0x22, 0x02, 0x00},
     7},
    {"Call.BlackSos.Dispatch",
     0x01389B22,
     TraceKind::SosEvent,
     {0x31, 0xF6, 0xBA, 0xEB, 0x22, 0x02, 0x00},
     7},

    {"OnMatchingCheck", 0x013808B0, TraceKind::MatchingEvent, StandardR15Prologue, 6},
    {"OnMatchingError", 0x013809F0, TraceKind::MatchingEvent, StandardR15Prologue, 6},
    {"OnSummonResult.Empty", 0x01384500, TraceKind::MatchingSignal, ReturnAndPadding, 16},
    {"OnSummonResult.Move", 0x01384510, TraceKind::MatchingSignal, ReturnAndPadding, 16},
    {"OnSummonResult.TimeOut", 0x01384520, TraceKind::MatchingSignal, ReturnAndPadding, 16},
    {"OnSummonResult.OtherError", 0x01384530, TraceKind::MatchingEvent, StandardRbxPrologue, 5},
    {"ForceSummonFail", 0x01384990, TraceKind::MatchingSignal, ReturnAndPadding, 16},
    {"ForceSummonSuccess", 0x013849A0, TraceKind::MatchingEvent, StandardR15Prologue, 6},

    {"CSRequestSummon.Init", 0x014B47C0, TraceKind::SummonRequest, StandardR15Prologue, 6},
    {"CSRequestSummon.Update", 0x014B4890, TraceKind::SummonRequest, StandardRbxPrologue, 5},
    {"CSRequestSummon.CheckWait", 0x014B48C0, TraceKind::SummonRequest, StandardR15Prologue, 6},
    {"CSRequestSummon.CheckComplete",
     0x014B4AE0,
     TraceKind::SummonRequest,
     {0xC7, 0x47, 0x54, 0xFF, 0xFF, 0xFF, 0xFF},
     7},

    {"CSRequestGetSos.Init", 0x01E5FAF0, TraceKind::GetSosRequest, StandardR15Prologue, 6},
    {"CSRequestGetSos.UpdateSeamless", 0x01E5FBF0, TraceKind::GetSosRequest, StandardR14Prologue,
     6},
    {"CSRequestGetSos.BuildRequest", 0x01E5FCE0, TraceKind::GetSosRequest, StandardR15Prologue, 6},
    {"CSRequestGetSos.SelectedArea",
     0x01E60270,
     TraceKind::GetSosArea,
     {0x8B, 0x02, 0x89, 0x85, 0xE8, 0xFE, 0xFF, 0xFF},
     8},
    {"CSRequestGetSos.InsertType", 0x01E5F970, TraceKind::GetSosInsert, StandardR15Prologue, 6},
    {"SosStatus.Update", 0x01872360, TraceKind::SosStatus, StandardR15Prologue, 6},
    {"SosStatus.NativeResult",
     0x0187239E,
     TraceKind::SosStatus,
     {0x83, 0xBD, 0x64, 0xFF, 0xFF, 0xFF, 0x00},
     7},

    {"Messenger.SosSign.Init", 0x01CC9460, TraceKind::SosMessenger, StandardRbxPrologue, 5},
    {"Messenger.SosSign.Wait", 0x01CC94B0, TraceKind::SosMessenger, StandardRbxPrologue, 5},
    {"Messenger.SosSign.Finish", 0x01CC94E0, TraceKind::SosMessenger, StandardR15Prologue, 6},

    {"ChrAction.Flags.Update",
     0x01A0F910,
     TraceKind::ActionFlags,
     {0x48, 0x8B, 0x47, 0x10, 0x48, 0x8B, 0x4F, 0x18},
     8},
    {"ChrAction.UseItem.Pressed",
     0x018F751C,
     TraceKind::UseItem,
     {0x49, 0x8B, 0x45, 0x00, 0x4C, 0x89, 0xEF},
     7},
    {"ChrAction.UseItem.Inventory",
     0x018F7529,
     TraceKind::UseItem,
     {0x48, 0x8D, 0xB8, 0xD0, 0x01, 0x00, 0x00},
     7},
    {"ChrAction.UseItem.ResolvedGoods",
     0x018F7535,
     TraceKind::UseItem,
     {0x41, 0x8B, 0x8D, 0x7C, 0x04, 0x00, 0x00},
     7},
    {"ChrAction.UseItem.NativeApply", 0x018F9720, TraceKind::UseItemExecution, StandardR15Prologue,
     6},
    {"ChrAction.UseItem.GoodsExecutor", 0x018C9160, TraceKind::UseItemExecution,
     StandardR15Prologue, 6},
    {"ChrAction.UseItem.GoodsExecutorResult",
     0x018C9364,
     TraceKind::UseItemExecution,
     {0x48, 0x8B, 0x03, 0x48, 0x3B, 0x45, 0xD0},
     7},
    {"ChrAction.UseItem.ActionSubmit", 0x01FF0FA0, TraceKind::GoodsActionSubmit,
     StandardR15Prologue, 6},
    {"GameParam.Goods.Lookup", 0x01F1E3D0, TraceKind::GoodsParamLookup, StandardR15Prologue, 6},
    {"Beckoning.Availability.Area",
     0x0157F8BA,
     TraceKind::BellAvailability,
     {0x84, 0xC0, 0x48, 0x0F, 0x45, 0xD9},
     6},
    {"Beckoning.Availability.NativeResult",
     0x0157F962,
     TraceKind::BellAvailability,
     {0x89, 0x85, 0x10, 0xFE, 0xFF, 0xFF},
     6},
    {"Network.Area.RegionResult",
     0x013DEAC8,
     TraceKind::NetworkAreaRegion,
     {0x45, 0x89, 0x7E, 0x44, 0xB0, 0x01},
     6},

    {"CSMultiPlayerIns.Init", 0x01E4FCF0, TraceKind::MultiplayerInsert, StandardRbxPrologue, 5},
    {"CSMultiPlayerIns.StartNotifyWait", 0x01E4FD50, TraceKind::MultiplayerInsert,
     StandardR15Prologue, 6},
    {"CSMultiPlayerIns.StartWait", 0x01E4FF70, TraceKind::MultiplayerInsert, StandardR14Prologue,
     6},
    {"CSMultiPlayerIns.FirstSyncWait", 0x01E50090, TraceKind::MultiplayerInsert,
     StandardR15Prologue, 6},
    {"CSMultiPlayerIns.Create", 0x01E505E0, TraceKind::MultiplayerInsert, StandardR15Prologue, 6},
    {"CSMultiPlayerIns.CreateWait", 0x01E50830, TraceKind::MultiplayerInsert, StandardRbxPrologue,
     5},
    {"CSMultiPlayerIns.Update",
     0x01E50860,
     TraceKind::MultiplayerInsert,
     {0x83, 0xBF, 0xE8, 0x00, 0x00, 0x00, 0x00},
     7},
    {"CSMultiPlayerIns.WaitExit", 0x01E50880, TraceKind::MultiplayerInsert, StandardR15Prologue, 6},
    {"CSMultiPlayerIns.Delete", 0x01E50A70, TraceKind::MultiplayerInsert, StandardR15Prologue, 6},
    {"CSMultiPlayerIns.DeleteWait",
     0x01E50D70,
     TraceKind::MultiplayerInsert,
     {0x8B, 0x87, 0xE8, 0x00, 0x00, 0x00, 0x00},
     6},
    {"CSMultiPlayerIns.PlacementCopy",
     0x01E4FF1E,
     TraceKind::GlobalState,
     {0x89, 0x88, 0xC4, 0x14, 0x00, 0x00},
     6},

    {"SetSosSignWarp",
     0x0132E267,
     TraceKind::GlobalState,
     {0xC6, 0x80, 0x20, 0x15, 0x00, 0x00, 0x01},
     7},
    {"SetSummonedPos",
     0x01332BB7,
     TraceKind::GlobalState,
     {0xC6, 0x40, 0x20, 0x01, 0xB0, 0x01, 0x00},
     6},
    {"SetSosSignPos.NativeCopy",
     0x01333C43,
     TraceKind::SosPlacement,
     {0x89, 0x88, 0xC0, 0x14, 0x00, 0x00},
     6},
    {"SummonedPlacement.SelectMap",
     0x01332C1B,
     TraceKind::PlacementSelect,
     {0x80, 0xB9, 0x20, 0x15, 0x00, 0x00, 0x00},
     7},
    {"SummonedMapReload.Native", 0x0131E5C0, TraceKind::MapReload, StandardR15Prologue, 6},
    {"SummonPoint.Resolve", 0x01582E70, TraceKind::SummonPoint, StandardR15Prologue, 6},

    {"ReceivedSummonPlacement.ConditionalSet",
     0x0156E6E7,
     TraceKind::ReceivedPlacementConditional,
     {0x41, 0x80, 0xB8, 0x48, 0x16, 0x00, 0x00, 0x00},
     8},
    {"ReceivedSummonPlacement.Set",
     0x0156E7A7,
     TraceKind::ReceivedPlacementSet,
     {0x8B, 0x12, 0x8B, 0x09, 0xC5, 0xF8, 0x28, 0x07},
     8},

    {"Event.WarpNextStage",
     0x0132E010,
     TraceKind::StageWarp,
     {0x55, 0x48, 0x89, 0xE5, 0xC1, 0xE6},
     6},
    {"Event.WarpNextStage_Bonfire",
     0x0132E050,
     TraceKind::StageWarp,
     {0x55, 0x48, 0x89, 0xE5, 0x48, 0x63, 0xC6},
     7},
    {"Warp.RespawnPointParam", 0x013CDF30, TraceKind::RespawnPointWarp, StandardR15Prologue, 6},
    {"Warp.StageTransition", 0x013CDE30, TraceKind::StageWarp, StandardR15Prologue, 6},
    {"Warp.StageDescriptor",
     0x01944D76,
     TraceKind::StageWarpDescriptor,
     {0x8B, 0x88, 0x3C, 0x15, 0x00, 0x00},
     6},
    {"Warp.StageTransition.AcknowledgeCheck",
     0x0194100A,
     TraceKind::StageWarpAcknowledge,
     {0x80, 0xBD, 0x68, 0xFE, 0xFF, 0xFF, 0x00},
     7},
    {"Warp.RespawnPoint.Resolve",
     0x01938F65,
     TraceKind::StageWarpRespawnResolve,
     {0x41, 0x8B, 0x85, 0xA4, 0x00, 0x00, 0x00},
     7},
    {"Warp.RespawnTransform.Resolve",
     0x0154AFC0,
     TraceKind::StageWarpTransformResolve,
     {0x55, 0x53, 0x83, 0xF9, 0xFF, 0x0F, 0x84, 0x9D, 0x00, 0x00, 0x00},
     11},
    {"Warp.RespawnPlacement.Apply", 0x01939C80, TraceKind::StageWarpPlacement, StandardR14Prologue,
     6},

    {"PlayerWarp.Dispatch.RemoteWarpPlayer.Result", 0x01FB2BE0, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Dispatch.RemoteResetPlayer.Result", 0x01FB2C60, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Dispatch.GetPlayerInfo.Result", 0x01FB2CE0, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Dispatch.RemoteWarpWorldPos.Result", 0x01FB2D60, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Dispatch.RemoteWarpMapStudioPos.Result", 0x01FB2DE0, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Dispatch.RemoteWarpPlayer.Void", 0x01FB2E60, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Dispatch.RemoteResetPlayer.Void", 0x01FB2EE0, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Dispatch.GetPlayerInfo.Void", 0x01FB2F60, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Dispatch.RemoteWarpWorldPos.Void", 0x01FB2FE0, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Dispatch.RemoteWarpMapStudioPos.Void", 0x01FB3060, TraceKind::WarpCommand,
     StandardR15Prologue, 6},

    {"PlayerWarp.Executor.RemoteWarpPlayer", 0x0154D180, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Executor.RemoteResetPlayer", 0x0154D5E0, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Executor.GetPlayerInfo", 0x0154D7A0, TraceKind::WarpCommand, StandardR15Prologue,
     6},
    {"PlayerWarp.Executor.RemoteWarpWorldPos", 0x0154DC50, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.Executor.RemoteWarpMapStudioPos", 0x0154E0B0, TraceKind::WarpCommand,
     StandardR15Prologue, 6},
    {"PlayerWarp.NativeDispatch", 0x0154EA30, TraceKind::WarpResolved, StandardR15Prologue, 6},
    {"PlayerWarp.LocalApply", 0x0154B110, TraceKind::WarpLocalApply, StandardR15Prologue, 6},
});

std::array<std::atomic<u64>, Sites.size()> site_hits{};
std::atomic<u64> event_sequence{};
std::atomic<s32> observed_sos_area{-1};
std::mutex capture_mutex;
std::ofstream capture_file;
std::filesystem::path capture_path;
uintptr_t image_base{};
bool installed{};
bool negative_area_patch_installed{};
bool area_flag_patch_installed{};
bool active_bell_negative_area_patch_installed{};
bool active_bell_area_flag_patch_installed{};
bool sos_status_area_patch_installed{};

bool EnvFlagEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::string_view{value} != "0";
}

std::string BytesToHex(std::span<const u8> bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const u8 byte : bytes) {
        out << std::setw(2) << static_cast<u32>(byte);
    }
    return out.str();
}

template <typename T>
T ReadValue(u64 address, size_t offset) {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address + offset), sizeof(value));
    return value;
}

void WriteHex(std::ostream& out, u64 value) {
    out << "\"0x" << std::hex << value << std::dec << '\"';
}

void WriteObjectState(std::ostream& out, u64 object) {
    if (object < 0x10000) {
        return;
    }
    out << ",\"object\":{";
    out << "\"state_54\":";
    WriteHex(out, ReadValue<u32>(object, 0x54));
    out << ",\"c8\":";
    WriteHex(out, ReadValue<u64>(object, 0xC8));
    out << ",\"d0\":";
    WriteHex(out, ReadValue<u64>(object, 0xD0));
    out << ",\"d8\":";
    WriteHex(out, ReadValue<u32>(object, 0xD8));
    out << ",\"e0\":";
    WriteHex(out, ReadValue<u64>(object, 0xE0));
    out << ",\"e8\":";
    WriteHex(out, ReadValue<u32>(object, 0xE8));
    out << ",\"ec\":";
    WriteHex(out, ReadValue<u32>(object, 0xEC));
    out << ",\"f0\":";
    WriteHex(out, ReadValue<u8>(object, 0xF0));
    out << ",\"f8\":";
    WriteHex(out, ReadValue<u32>(object, 0xF8));
    out << '}';
}

void WriteGetSosState(std::ostream& out, u64 object) {
    if (object < 0x10000) {
        return;
    }
    out << ",\"get_sos\":{";
    const u64 begin = ReadValue<u64>(object, 0xD0);
    const u64 current = ReadValue<u64>(object, 0xD8);
    const u64 end = ReadValue<u64>(object, 0xE0);
    out << "\"begin\":";
    WriteHex(out, begin);
    out << ",\"current\":";
    WriteHex(out, current);
    out << ",\"end\":";
    WriteHex(out, end);
    if (begin >= 0x10000 && current >= begin && current <= end &&
        (current - begin) % sizeof(s32) == 0) {
        const size_t count = std::min<size_t>((current - begin) / sizeof(s32), 16);
        out << ",\"summon_types\":[";
        for (size_t index = 0; index < count; ++index) {
            if (index != 0) {
                out << ',';
            }
            out << ReadValue<s32>(begin, index * sizeof(s32));
        }
        out << ']';
    }
    out << ",\"request_active\":" << static_cast<u32>(ReadValue<u8>(object, 0x120));
    out << ",\"retry_count\":" << ReadValue<u32>(object, 0x124);
    out << '}';
}

void WriteMultiplayerEffects(std::ostream& out, u64 player) {
    if (player < 0x10000) {
        return;
    }

    const u64 manager = ReadValue<u64>(player, 0x1C8);
    if (manager < 0x10000) {
        return;
    }

    out << ",\"multiplayer_effects\":{";
    out << "\"manager\":";
    WriteHex(out, manager);
    out << ",\"mask\":";
    WriteHex(out, ReadValue<u32>(manager, 0x20));
    out << ",\"entries\":[";

    bool first = true;
    u64 node = ReadValue<u64>(manager, 0x08);
    for (size_t index = 0; node >= 0x10000 && index < 512; ++index) {
        const u64 next = ReadValue<u64>(node, 0x58);
        const u32 flags = ReadValue<u32>(node, 0x1C);
        const u64 param = ReadValue<u64>(node, 0x48);
        if ((flags & 0x800C0003U) == 0 && param >= 0x10000) {
            const s32 effect_id = ReadValue<s32>(node, 0x40);
            const u16 special_state = ReadValue<u16>(param, 0x156);
            if ((special_state >= 188 && special_state <= 190) || effect_id == 9003 ||
                effect_id == 9004) {
                if (!first) {
                    out << ',';
                }
                first = false;
                out << "{\"effect_id\":" << effect_id << ",\"special_state\":" << special_state
                    << '}';
            }
        }
        if (next == node) {
            break;
        }
        node = next;
    }
    out << "]}";
}

void WriteMessengerState(std::ostream& out, u64 object) {
    if (object < 0x10000) {
        return;
    }
    out << ",\"messenger\":{";
    out << "\"request\":";
    WriteHex(out, ReadValue<u64>(object, 0xE0));
    out << ",\"waiting\":" << static_cast<u32>(ReadValue<u8>(object, 0xF0));
    out << ",\"flags_144\":";
    WriteHex(out, ReadValue<u8>(object, 0x144));
    out << '}';
}

bool HasActionTransition(u64 object) {
    if (object < 0x10000) {
        return false;
    }
    const u64 current = ReadValue<u64>(object, 0x10);
    const u64 previous = ReadValue<u64>(object, 0x18);
    return current != previous;
}

void WriteActionFlags(std::ostream& out, u64 object, u64 stack_pointer) {
    if (object < 0x10000 || stack_pointer < 0x10000) {
        return;
    }
    const u64 current = ReadValue<u64>(object, 0x10);
    const u64 previous = ReadValue<u64>(object, 0x18);
    const u64 return_address = ReadValue<u64>(stack_pointer, 0);
    out << ",\"action_flags\":{";
    out << "\"module\":";
    WriteHex(out, object);
    out << ",\"return_address\":";
    WriteHex(out, return_address);
    if (return_address >= image_base &&
        return_address - image_base < MemoryPatcher::g_eboot_image_size) {
        out << ",\"caller_offset\":\"0x" << std::hex << return_address - image_base << std::dec
            << '\"';
    }
    out << ",\"current\":";
    WriteHex(out, current);
    out << ",\"previous\":";
    WriteHex(out, previous);
    out << ",\"pressed\":";
    WriteHex(out, current & ~previous);
    out << ",\"released\":";
    WriteHex(out, previous & ~current);
    out << ",\"field_30\":";
    WriteHex(out, ReadValue<u64>(object, 0x30));
    out << ",\"field_38\":";
    WriteHex(out, ReadValue<u64>(object, 0x38));
    out << ",\"field_40\":";
    WriteHex(out, ReadValue<u64>(object, 0x40));
    out << ",\"field_48\":";
    WriteHex(out, ReadValue<u64>(object, 0x48));
    out << ",\"state_90\":";
    WriteHex(out, ReadValue<u32>(object, 0x90));
    out << ",\"state_94\":";
    WriteHex(out, ReadValue<u32>(object, 0x94));
    out << ",\"flags_9c\":";
    WriteHex(out, ReadValue<u8>(object, 0x9C));
    out << '}';
}

void WritePlayerItemState(std::ostream& out, u64 player) {
    out << "\"player\":";
    WriteHex(out, player);
    if (player >= 0x10000) {
        const u64 vtable = ReadValue<u64>(player, 0);
        out << ",\"vtable\":";
        WriteHex(out, vtable);
        if (vtable >= 0x10000) {
            out << ",\"vfunc_1c0\":";
            WriteHex(out, ReadValue<u64>(vtable, 0x1C0));
            out << ",\"vfunc_1c8\":";
            WriteHex(out, ReadValue<u64>(vtable, 0x1C8));
        }
        out << ",\"entity_60\":";
        WriteHex(out, ReadValue<u64>(player, 0x60));
        out << ",\"handle_90\":";
        WriteHex(out, ReadValue<u32>(player, 0x90));
        out << ",\"active_goods\":";
        WriteHex(out, ReadValue<u32>(player, 0x1C0));
        out << ",\"resolved_goods\":";
        WriteHex(out, ReadValue<u32>(player, 0x478));
        out << ",\"override_goods\":";
        WriteHex(out, ReadValue<u32>(player, 0x47C));
        out << ",\"queued_goods\":";
        WriteHex(out, ReadValue<u32>(player, 0x488));
        out << ",\"item_state\":";
        WriteHex(out, ReadValue<u32>(player, 0x490));
        WriteMultiplayerEffects(out, player);
    }
}

void WriteUseItemState(std::ostream& out, const TraceSite& site,
                       const GuestRegisterSnapshot& registers) {
    out << ",\"use_item\":{";
    WritePlayerItemState(out, registers.r13);
    out << ",\"rax\":";
    WriteHex(out, registers.rax);
    out << ",\"eax_signed\":" << static_cast<s64>(static_cast<s32>(registers.rax));
    if (site.offset == 0x018F751C && registers.rax >= 0x10000) {
        const u64 current = ReadValue<u64>(registers.rax, 0x10);
        const u64 previous = ReadValue<u64>(registers.rax, 0x18);
        out << ",\"action_current\":";
        WriteHex(out, current);
        out << ",\"action_previous\":";
        WriteHex(out, previous);
        out << ",\"action_pressed\":";
        WriteHex(out, ReadValue<u64>(registers.rax, 0x20));
    }
    if (site.offset == 0x018F7529 && registers.rax >= 0x10000) {
        out << ",\"inventory\":";
        WriteHex(out, registers.rax);
        out << ",\"inventory_selection\":[";
        for (size_t index = 0; index < 8; ++index) {
            if (index != 0) {
                out << ',';
            }
            WriteHex(out, ReadValue<u64>(registers.rax, 0x1D0 + index * sizeof(u64)));
        }
        out << ']';
    }
    out << '}';
}

bool IsTrackedGoods(s32 goods_id) {
    constexpr std::array TrackedGoods{100, 111, 200, 201, 205, 206, 225, 226};
    return std::ranges::find(TrackedGoods, goods_id) != TrackedGoods.end();
}

s32 GetUseItemExecutionGoods(const TraceSite& site, const GuestRegisterSnapshot& registers) {
    if (site.offset == 0x018C9364) {
        return registers.rbp >= 0xD8 ? ReadValue<s32>(registers.rbp - 0xD8, 0) : -1;
    }
    return static_cast<s32>(registers.rsi);
}

void WriteUseItemExecution(std::ostream& out, const TraceSite& site,
                           const GuestRegisterSnapshot& registers) {
    const bool is_result = site.offset == 0x018C9364;
    const u64 player = is_result ? registers.r14 : registers.rdi;
    const s32 goods_id = GetUseItemExecutionGoods(site, registers);
    const s32 argument = static_cast<s32>(is_result ? registers.rbx : registers.rdx);
    out << ",\"use_item_execution\":{";
    WritePlayerItemState(out, player);
    out << ",\"goods_id\":" << goods_id;
    out << ",\"argument\":" << argument;
    if (is_result) {
        out << ",\"result\":" << static_cast<u32>(static_cast<u8>(registers.r12));
    }
    out << '}';
}

void WriteGoodsActionSubmit(std::ostream& out, const GuestRegisterSnapshot& registers) {
    out << ",\"goods_action\":{";
    out << "\"player\":";
    WriteHex(out, registers.rdi);
    out << ",\"goods_id\":" << static_cast<s32>(registers.rsi);
    out << ",\"argument\":" << static_cast<s32>(registers.rdx);
    out << ",\"sp_effect_id\":" << static_cast<s32>(registers.rcx);
    out << ",\"executor_allowed\":" << static_cast<u32>(static_cast<u8>(registers.r8));
    WriteMultiplayerEffects(out, registers.rdi);
    out << '}';
}

void WriteGoodsParamLookup(std::ostream& out, const GuestRegisterSnapshot& registers) {
    const s32 goods_id = static_cast<s32>(registers.rsi);
    const u64 return_address = ReadValue<u64>(registers.rsp, 0);
    out << ",\"goods_lookup\":{";
    out << "\"goods_id\":" << goods_id;
    out << ",\"sos_area\":" << observed_sos_area.load(std::memory_order_relaxed);
    out << ",\"result_storage\":";
    WriteHex(out, registers.rdi);
    out << ",\"return_address\":";
    WriteHex(out, return_address);
    if (return_address >= image_base &&
        return_address - image_base < MemoryPatcher::g_eboot_image_size) {
        out << ",\"caller_offset\":\"0x" << std::hex << return_address - image_base << std::dec
            << '\"';
    }
    out << '}';
}

void WriteBellAvailability(std::ostream& out, const TraceSite& site,
                           const GuestRegisterSnapshot& registers) {
    const bool is_native_result = site.offset == 0x0157F962;
    const u64 selected_area = static_cast<u8>(registers.rax) != 0 ? registers.rcx : registers.rbx;
    const s32 area =
        is_native_result ? static_cast<s32>(registers.rbx) : ReadValue<s32>(selected_area, 0);
    out << ",\"bell_availability\":{";
    out << "\"area\":" << area;
    out << ",\"observed_sos_area\":" << observed_sos_area.load(std::memory_order_relaxed);
    out << ",\"player\":";
    WriteHex(out, registers.r14);
    if (is_native_result) {
        out << ",\"native_blocked\":" << static_cast<u32>(static_cast<u8>(registers.rax));
    }
    WriteMultiplayerEffects(out, registers.r14);
    out << '}';
}

void WriteQwords(std::ostream& out, std::string_view key, u64 address, size_t count) {
    if (address < 0x10000) {
        return;
    }
    out << ",\"" << key << "\":[";
    for (size_t index = 0; index < count; ++index) {
        if (index != 0) {
            out << ',';
        }
        WriteHex(out, ReadValue<u64>(address, index * sizeof(u64)));
    }
    out << ']';
}

void WriteFloatVector(std::ostream& out, std::string_view key, u64 address) {
    if (address < 0x10000) {
        return;
    }
    out << ",\"" << key << "\":[";
    for (size_t index = 0; index < 4; ++index) {
        if (index != 0) {
            out << ',';
        }
        const float value = ReadValue<float>(address, index * sizeof(float));
        if (std::isfinite(value)) {
            out << std::setprecision(9) << value;
        } else {
            out << "null";
        }
    }
    out << ']';
}

void WriteMapId(std::ostream& out, std::string_view key, u64 address) {
    if (address < 0x10000) {
        return;
    }
    const u32 map_id = ReadValue<u32>(address, 0);
    out << ",\"" << key << "\":";
    WriteHex(out, map_id);
    out << ",\"" << key << "_parts\":[" << ((map_id >> 24) & 0xFF) << ',' << ((map_id >> 16) & 0xFF)
        << ',' << ((map_id >> 8) & 0xFF) << ',' << (map_id & 0xFF) << ']';
}

void WriteU32(std::ostream& out, std::string_view key, u64 address) {
    if (address < 0x10000) {
        return;
    }
    out << ",\"" << key << "\":";
    WriteHex(out, ReadValue<u32>(address, 0));
}

void WritePlacementState(std::ostream& out, u64 state) {
    WriteQwords(out, "global_qwords", state, 6);
    WriteQwords(out, "sos_placement_qwords", state + 0x14A0, 5);
    WriteQwords(out, "summoned_placement_qwords", state + 0x14D0, 5);
    WriteQwords(out, "warp_qwords", state + 0x1518, 3);
    WriteQwords(out, "received_placement_qwords", state + 0x1620, 6);
}

u64 GetGlobalState();

void WriteStageWarpState(std::ostream& out, u64 state) {
    if (state < 0x10000) {
        return;
    }
    out << ",\"stage_warp_state\":{";
    out << "\"state\":";
    WriteHex(out, state);
    out << ",\"request_active\":" << static_cast<u32>(ReadValue<u8>(state, 0x08));
    const u32 map_id = ReadValue<u32>(state, 0x0C);
    out << ",\"map_id\":";
    WriteHex(out, map_id);
    out << ",\"map_parts\":[" << ((map_id >> 24) & 0xFF) << ',' << ((map_id >> 16) & 0xFF) << ','
        << ((map_id >> 8) & 0xFF) << ',' << (map_id & 0xFF) << ']';
    out << ",\"warp_info_id\":" << ReadValue<s32>(state, 0x10);
    out << ",\"mode_1538\":" << ReadValue<s32>(state, 0x1538);
    out << ",\"respawn_point_id_153c\":" << ReadValue<s32>(state, 0x153C);
    out << ",\"auxiliary_id_1540\":" << ReadValue<s32>(state, 0x1540);
    out << ",\"warp_fields_153c\":";
    WriteHex(out, ReadValue<u64>(state, 0x153C));
    out << ",\"state_1592\":" << static_cast<u32>(ReadValue<u8>(state, 0x1592));
    out << ",\"transition_16f8\":";
    WriteHex(out, ReadValue<u64>(state, 0x16F8));
    out << '}';
}

void WriteStageWarp(std::ostream& out, const TraceSite& site,
                    const GuestRegisterSnapshot& registers) {
    out << ",\"stage_warp\":{";
    if (site.offset == 0x0132E010) {
        const u32 map_id = (static_cast<u32>(registers.rsi) & 0xFF) << 24 |
                           (static_cast<u32>(registers.rdx) & 0xFF) << 16 |
                           (static_cast<u32>(registers.rcx) & 0xFF) << 8 |
                           (static_cast<u32>(registers.r8) & 0xFF);
        out << "\"source\":\"event\",\"map_id\":";
        WriteHex(out, map_id);
        out << ",\"map_parts\":[" << (registers.rsi & 0xFF) << ',' << (registers.rdx & 0xFF) << ','
            << (registers.rcx & 0xFF) << ',' << (registers.r8 & 0xFF) << ']';
        out << ",\"warp_info_id\":" << static_cast<s32>(registers.r9);
    } else if (site.offset == 0x0132E050) {
        const s32 warp_info_id = static_cast<s32>(registers.rsi);
        const s32 area = warp_info_id / 100000;
        const s32 block = (warp_info_id / 10000) % 10;
        out << "\"source\":\"bonfire_event\",\"warp_info_id\":" << warp_info_id;
        out << ",\"derived_map_parts\":[" << area << ',' << block << ",0,0]";
    } else {
        out << "\"source\":\"transition\"";
    }
    out << '}';
    WriteStageWarpState(out, GetGlobalState());
}

void WriteStageWarpDescriptor(std::ostream& out, const GuestRegisterSnapshot& registers) {
    out << ",\"stage_warp_descriptor\":{";
    out << "\"descriptor\":";
    WriteHex(out, registers.r9);
    out << ",\"mode\":" << static_cast<s32>(registers.r13);
    if (registers.r9 >= 0x10000) {
        out << ",\"request_type\":" << ReadValue<s32>(registers.r9, 0x54);
        out << ",\"request_subtype\":" << ReadValue<s32>(registers.r9, 0x58);
        out << ",\"descriptor_mode\":" << ReadValue<s32>(registers.r9, 0x98);
        out << ",\"previous_respawn_point\":" << ReadValue<s32>(registers.r9, 0x80);
        out << ",\"previous_auxiliary_id\":" << ReadValue<s32>(registers.r9, 0x84);
    }
    out << '}';
    WriteStageWarpState(out, registers.rax);
}

void WriteStageWarpAcknowledge(std::ostream& out, const GuestRegisterSnapshot& registers) {
    out << ",\"stage_warp_acknowledge\":{";
    out << "\"state\":";
    WriteHex(out, registers.rcx);
    const bool frame_valid = registers.rbp >= 0x198 + 0x10000;
    if (frame_valid) {
        const bool will_clear_request = ReadValue<u8>(registers.rbp - 0x198, 0) != 0;
        out << ",\"will_clear_request\":" << (will_clear_request ? "true" : "false");
    }
    out << '}';
    WriteStageWarpState(out, registers.rcx);
}

void WriteStageWarpTask(std::ostream& out, u64 task, bool include_resolved_transform) {
    if (task < 0x10000) {
        return;
    }

    const u32 map_id = ReadValue<u32>(task, 0x58);
    out << ",\"stage_warp_task\":{";
    out << "\"task\":";
    WriteHex(out, task);
    out << ",\"map_id\":";
    WriteHex(out, map_id);
    out << ",\"map_parts\":[" << ((map_id >> 24) & 0xFF) << ',' << ((map_id >> 16) & 0xFF) << ','
        << ((map_id >> 8) & 0xFF) << ',' << (map_id & 0xFF) << ']';
    out << ",\"request_type\":" << ReadValue<s32>(task, 0xA4);
    out << ",\"request_subtype\":" << ReadValue<s32>(task, 0xA8);
    out << ",\"respawn_point_id\":" << ReadValue<s32>(task, 0xD0);
    out << ",\"auxiliary_id\":" << ReadValue<s32>(task, 0xD4);
    out << ",\"mode\":" << ReadValue<s32>(task, 0xE8);
    out << '}';
    if (include_resolved_transform) {
        WriteFloatVector(out, "resolved_position", task + 0xF0);
        const float facing = ReadValue<float>(task, 0x100);
        out << ",\"resolved_facing\":";
        if (std::isfinite(facing)) {
            out << std::setprecision(9) << facing;
        } else {
            out << "null";
        }
    }
}

void WriteRespawnTransformResolve(std::ostream& out, const GuestRegisterSnapshot& registers) {
    out << ",\"respawn_transform_resolve\":{";
    out << "\"map_resource\":";
    WriteHex(out, registers.rdi);
    out << ",\"position_output\":";
    WriteHex(out, registers.rsi);
    out << ",\"orientation_output\":";
    WriteHex(out, registers.rdx);
    out << ",\"respawn_point_id\":" << static_cast<s32>(registers.rcx);
    out << '}';
}

u64 GetGlobalState() {
    const u64 image_size = MemoryPatcher::g_eboot_image_size;
    if (image_base == 0 || image_size < sizeof(u64) ||
        GlobalStatePointerOffset > image_size - sizeof(u64)) {
        return 0;
    }
    return ReadValue<u64>(image_base + GlobalStatePointerOffset, 0);
}

void PS4_SYSV_ABI TraceEntry(u64 tag, const GuestRegisterSnapshot* registers) {
    if (tag >= Sites.size() || registers == nullptr) {
        return;
    }

    const auto& site = Sites[tag];
    if (site.kind == TraceKind::ActionFlags && !HasActionTransition(registers->rdi)) {
        return;
    }
    if (site.kind == TraceKind::GoodsParamLookup &&
        !IsTrackedGoods(static_cast<s32>(registers->rsi))) {
        return;
    }
    if ((site.kind == TraceKind::UseItemExecution || site.kind == TraceKind::GoodsActionSubmit) &&
        !IsTrackedGoods(GetUseItemExecutionGoods(site, *registers))) {
        return;
    }
    if (site.kind == TraceKind::SosValidityCode) {
        observed_sos_area.store(static_cast<s32>(registers->rax), std::memory_order_relaxed);
    }

    const u64 hit = site_hits[tag].fetch_add(1, std::memory_order_relaxed) + 1;
    const u64 dense_capture_limit =
        site.kind == TraceKind::ActionFlags || site.kind == TraceKind::GoodsParamLookup ? 2048 : 64;
    if (hit > dense_capture_limit && hit % 300 != 0) {
        return;
    }

    const u64 sequence = event_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    try {
        std::scoped_lock lock{capture_mutex};
        if (!capture_file) {
            return;
        }
        capture_file << "{\"type\":\"entry\",\"seq\":" << sequence << ",\"time_ms\":" << timestamp
                     << ",\"site\":\"" << site.name << "\",\"offset\":\"0x" << std::hex
                     << site.offset << std::dec << "\",\"hit\":" << hit << ",\"registers\":{";
        capture_file << "\"rax\":";
        WriteHex(capture_file, registers->rax);
        capture_file << ",\"rdi\":";
        WriteHex(capture_file, registers->rdi);
        capture_file << ",\"rsi\":";
        WriteHex(capture_file, registers->rsi);
        capture_file << ",\"rdx\":";
        WriteHex(capture_file, registers->rdx);
        capture_file << ",\"rcx\":";
        WriteHex(capture_file, registers->rcx);
        capture_file << ",\"r8\":";
        WriteHex(capture_file, registers->r8);
        capture_file << ",\"r9\":";
        WriteHex(capture_file, registers->r9);
        capture_file << ",\"rsp\":";
        WriteHex(capture_file, registers->rsp);
        capture_file << '}';

        switch (site.kind) {
        case TraceKind::SosCondition:
            WriteQwords(capture_file, "condition_qwords", registers->rdi, 8);
            break;
        case TraceKind::SosValidityDispatch: {
            capture_file << ",\"validity_dispatch\":{";
            capture_file << "\"player\":";
            WriteHex(capture_file, registers->rbx);
            const u64 vtable = ReadValue<u64>(registers->rbx, 0);
            const u64 function = ReadValue<u64>(vtable, 0x628);
            capture_file << ",\"vtable\":";
            WriteHex(capture_file, vtable);
            capture_file << ",\"function\":";
            WriteHex(capture_file, function);
            if (function >= image_base &&
                function - image_base < MemoryPatcher::g_eboot_image_size) {
                capture_file << ",\"function_offset\":\"0x" << std::hex << function - image_base
                             << std::dec << '\"';
            }
            capture_file << '}';
            break;
        }
        case TraceKind::SosValidityCode:
            capture_file << ",\"validity_code\":{";
            capture_file << "\"previous\":" << static_cast<s64>(static_cast<s32>(registers->r15));
            capture_file << ",\"current\":" << static_cast<s64>(static_cast<s32>(registers->rax));
            capture_file << ",\"object\":";
            WriteHex(capture_file, registers->r14);
            capture_file << ",\"source\":";
            WriteHex(capture_file, registers->rbx);
            capture_file << '}';
            WriteQwords(capture_file, "condition_qwords", registers->r14, 6);
            break;
        case TraceKind::SosEvent: {
            const u64 state = GetGlobalState();
            capture_file << ",\"sos_event\":{";
            capture_file << "\"global_state\":";
            WriteHex(capture_file, state);
            if (state >= 0x10000) {
                capture_file << ",\"warp_1520\":";
                WriteHex(capture_file, ReadValue<u8>(state, 0x1520));
                capture_file << ",\"menu_1544\":";
                WriteHex(capture_file, ReadValue<u8>(state, 0x1544));
                capture_file << ",\"transition_1547\":";
                WriteHex(capture_file, ReadValue<u8>(state, 0x1547));
                capture_file << ",\"active_1578\":";
                WriteHex(capture_file, ReadValue<u8>(state, 0x1578));
            }
            capture_file << '}';
            WritePlacementState(capture_file, state);
            break;
        }
        case TraceKind::MatchingEvent:
            WriteQwords(capture_file, "event_qwords", registers->rdx, 6);
            break;
        case TraceKind::MatchingSignal:
            break;
        case TraceKind::SummonRequest:
        case TraceKind::MultiplayerInsert:
            WriteObjectState(capture_file, registers->rdi);
            break;
        case TraceKind::GetSosRequest:
            WriteGetSosState(capture_file, registers->rdi);
            break;
        case TraceKind::GetSosArea:
            capture_file << ",\"get_sos_area\":{";
            capture_file << "\"player\":";
            WriteHex(capture_file, registers->r13);
            capture_file << ",\"source\":";
            WriteHex(capture_file, registers->rdx);
            if (registers->rdx >= 0x10000) {
                capture_file << ",\"value\":"
                             << static_cast<s64>(ReadValue<s32>(registers->rdx, 0));
            }
            capture_file << '}';
            break;
        case TraceKind::GetSosInsert:
            capture_file << ",\"get_sos_insert\":{";
            capture_file << "\"event_code\":" << static_cast<s64>(static_cast<s32>(registers->rsi));
            capture_file << ",\"observed_sos_area\":"
                         << observed_sos_area.load(std::memory_order_relaxed);
            capture_file << '}';
            WriteGetSosState(capture_file, registers->rdi);
            break;
        case TraceKind::SosStatus:
            capture_file << ",\"sos_status\":{";
            capture_file << "\"object\":";
            WriteHex(capture_file, registers->rbx);
            capture_file << ",\"observed_sos_area\":"
                         << observed_sos_area.load(std::memory_order_relaxed);
            if (site.name == "SosStatus.NativeResult") {
                capture_file << ",\"primary_present\":" << ReadValue<s32>(registers->rbp - 0x9C, 0);
                capture_file << ",\"primary_value\":" << ReadValue<s32>(registers->rbp - 0x90, 0);
                capture_file << ",\"secondary_present\":"
                             << ReadValue<s32>(registers->rbp - 0x84, 0);
                capture_file << ",\"secondary_value\":" << ReadValue<s32>(registers->rbp - 0x78, 0);
                capture_file << ",\"event_bits\":"
                             << static_cast<u32>(ReadValue<u8>(registers->rbp - 0x6C, 0));
            }
            capture_file << '}';
            break;
        case TraceKind::SosMessenger:
            WriteMessengerState(capture_file, registers->rdi);
            break;
        case TraceKind::ActionFlags:
            WriteActionFlags(capture_file, registers->rdi, registers->rsp);
            break;
        case TraceKind::UseItem:
            WriteUseItemState(capture_file, site, *registers);
            break;
        case TraceKind::UseItemExecution:
            WriteUseItemExecution(capture_file, site, *registers);
            break;
        case TraceKind::GoodsActionSubmit:
            WriteGoodsActionSubmit(capture_file, *registers);
            break;
        case TraceKind::GoodsParamLookup:
            WriteGoodsParamLookup(capture_file, *registers);
            break;
        case TraceKind::BellAvailability:
            WriteBellAvailability(capture_file, site, *registers);
            break;
        case TraceKind::NetworkAreaRegion:
            capture_file << ",\"network_area_region\":{";
            capture_file << "\"object\":";
            WriteHex(capture_file, registers->r14);
            capture_file << ",\"sos_area\":" << observed_sos_area.load(std::memory_order_relaxed);
            capture_file << ",\"previous_region\":"
                         << static_cast<s64>(ReadValue<s32>(registers->r14, 0x44));
            capture_file << ",\"selected_region\":"
                         << static_cast<s64>(static_cast<s32>(registers->r15));
            capture_file << '}';
            break;
        case TraceKind::SosPlacement:
            WriteQwords(capture_file, "position_qwords", registers->rax + 0x14A0, 2);
            WriteQwords(capture_file, "orientation_qwords", registers->rax + 0x14B0, 2);
            capture_file << ",\"map_id\":";
            WriteHex(capture_file, static_cast<u32>(registers->rcx));
            break;
        case TraceKind::GlobalState:
            WritePlacementState(capture_file, registers->rax);
            break;
        case TraceKind::ReceivedPlacementConditional:
            WriteFloatVector(capture_file, "received_position", registers->rdi);
            WriteFloatVector(capture_file, "received_orientation", registers->rsi);
            WriteU32(capture_file, "matching_key", registers->rdx);
            WriteMapId(capture_file, "received_map_id", registers->rcx);
            WritePlacementState(capture_file, registers->r8);
            break;
        case TraceKind::ReceivedPlacementSet:
            WriteFloatVector(capture_file, "received_position", registers->rdi);
            WriteFloatVector(capture_file, "received_orientation", registers->rsi);
            WriteU32(capture_file, "matching_key", registers->rdx);
            WriteMapId(capture_file, "received_map_id", registers->rcx);
            WritePlacementState(capture_file, registers->rax);
            break;
        case TraceKind::PlacementSelect:
            capture_file << ",\"default_map_id\":";
            WriteHex(capture_file, static_cast<u32>(registers->rax));
            WritePlacementState(capture_file, registers->rcx);
            break;
        case TraceKind::WarpCommand:
            WriteQwords(capture_file, "parameter_qwords", registers->rsi, 12);
            break;
        case TraceKind::WarpResolved:
            WriteFloatVector(capture_file, "position", registers->rdi);
            WriteFloatVector(capture_file, "orientation", registers->rsi);
            WriteFloatVector(capture_file, "camera_orientation", registers->rdx);
            WriteMapId(capture_file, "map_id", registers->rcx);
            break;
        case TraceKind::WarpLocalApply:
            WriteMapId(capture_file, "map_id", registers->rsi);
            WriteFloatVector(capture_file, "position", registers->rdx);
            WriteFloatVector(capture_file, "orientation", registers->rcx);
            break;
        case TraceKind::StageWarp:
            WriteStageWarp(capture_file, site, *registers);
            break;
        case TraceKind::RespawnPointWarp:
            capture_file << ",\"respawn_point_warp\":{";
            capture_file << "\"warp_info_id\":" << static_cast<s32>(registers->rdi) << '}';
            WriteStageWarpState(capture_file, GetGlobalState());
            break;
        case TraceKind::StageWarpDescriptor:
            WriteStageWarpDescriptor(capture_file, *registers);
            break;
        case TraceKind::StageWarpAcknowledge:
            WriteStageWarpAcknowledge(capture_file, *registers);
            break;
        case TraceKind::StageWarpRespawnResolve:
            WriteStageWarpTask(capture_file, registers->r13, false);
            break;
        case TraceKind::StageWarpTransformResolve:
            WriteRespawnTransformResolve(capture_file, *registers);
            break;
        case TraceKind::StageWarpPlacement:
            WriteStageWarpTask(capture_file, registers->rdi, true);
            break;
        case TraceKind::SummonPoint:
            WriteQwords(capture_file, "query_qwords", registers->rsi, 8);
            break;
        case TraceKind::MapReload:
            WritePlacementState(capture_file, GetGlobalState());
            break;
        }
        capture_file << "}\n";
        capture_file.flush();

        if (hit <= 8 || hit % 300 == 0) {
            LOG_INFO(Debug,
                     "Bloodborne RE entry {} hit={} rdi={:#x} rsi={:#x} rdx={:#x} capture={}",
                     site.name, hit, registers->rdi, registers->rsi, registers->rdx,
                     Common::FS::PathToUTF8String(capture_path));
        }
    } catch (...) {
        // An observer must never unwind into guest code.
    }
}

bool VerifySites() {
    const auto image_size = MemoryPatcher::g_eboot_image_size;
    for (const auto& site : Sites) {
        const auto expected = std::span<const u8>{site.prologue.data(), site.prologue_size};
        if (site.offset > image_size || expected.size() > image_size - site.offset) {
            LOG_ERROR(Debug, "Bloodborne RE site {} lies outside the eboot image", site.name);
            return false;
        }
        const auto actual = std::span<const u8>{
            reinterpret_cast<const u8*>(image_base + site.offset), expected.size()};
        if (!std::ranges::equal(actual, expected)) {
            LOG_ERROR(Debug,
                      "Bloodborne RE signature mismatch at {} ({:#x}): expected={} actual={}",
                      site.name, site.offset, BytesToHex(expected), BytesToHex(actual));
            return false;
        }
    }
    return true;
}

} // namespace

void InstallSeamlessCoopPatches() {
    if ((negative_area_patch_installed && area_flag_patch_installed &&
         active_bell_negative_area_patch_installed && active_bell_area_flag_patch_installed &&
         sos_status_area_patch_installed) ||
        !EnvFlagEnabled("SHADPS4_BLOODBORNE_SEAMLESS_COOP")) {
        return;
    }
    if (MemoryPatcher::g_game_serial != "CUSA03173") {
        return;
    }

    const auto* param_sfo = Common::Singleton<PSF>::Instance();
    const std::string_view app_version = param_sfo->GetString("APP_VER").value_or("Unknown");
    if (app_version != "01.09") {
        LOG_ERROR(Debug,
                  "Bloodborne seamless patches require CUSA03173 01.09; loaded version is {}",
                  app_version);
        return;
    }

    const auto base = MemoryPatcher::g_eboot_address;
    const auto image_size = MemoryPatcher::g_eboot_image_size;
    if (base == 0 || BeckoningAreaComparisonOffset > image_size ||
        BeckoningAreaComparison.size() > image_size - BeckoningAreaComparisonOffset ||
        BeckoningAreaFlagResultOffset > image_size ||
        BeckoningAreaFlagResult.size() > image_size - BeckoningAreaFlagResultOffset ||
        ActiveBellAreaComparisonOffset > image_size ||
        ActiveBellAreaComparison.size() > image_size - ActiveBellAreaComparisonOffset ||
        ActiveBellAreaFlagResultOffset > image_size ||
        ActiveBellAreaFlagResult.size() > image_size - ActiveBellAreaFlagResultOffset ||
        SosStatusAreaRestrictionOffset > image_size ||
        SosStatusAreaRestriction.size() > image_size - SosStatusAreaRestrictionOffset) {
        LOG_ERROR(Debug, "Bloodborne Beckoning patches lie outside the eboot image");
        return;
    }

    auto* area_comparison = reinterpret_cast<u8*>(base + BeckoningAreaComparisonOffset);
    const auto actual_area_comparison =
        std::span<const u8>{area_comparison, BeckoningAreaComparison.size()};
    if (!std::ranges::equal(actual_area_comparison, BeckoningAreaComparison)) {
        LOG_ERROR(Debug,
                  "Bloodborne Beckoning area signature mismatch at {:#x}: expected={} actual={}",
                  BeckoningAreaComparisonOffset, BytesToHex(BeckoningAreaComparison),
                  BytesToHex(actual_area_comparison));
        return;
    }

    auto* area_flag_result = reinterpret_cast<u8*>(base + BeckoningAreaFlagResultOffset);
    const auto actual_area_flag_result =
        std::span<const u8>{area_flag_result, BeckoningAreaFlagResult.size()};
    if (!std::ranges::equal(actual_area_flag_result, BeckoningAreaFlagResult)) {
        LOG_ERROR(Debug,
                  "Bloodborne Beckoning area-flag signature mismatch at {:#x}: expected={} "
                  "actual={}",
                  BeckoningAreaFlagResultOffset, BytesToHex(BeckoningAreaFlagResult),
                  BytesToHex(actual_area_flag_result));
        return;
    }

    auto* active_bell_area_comparison =
        reinterpret_cast<u8*>(base + ActiveBellAreaComparisonOffset);
    const auto actual_active_bell_area_comparison =
        std::span<const u8>{active_bell_area_comparison, ActiveBellAreaComparison.size()};
    if (!std::ranges::equal(actual_active_bell_area_comparison, ActiveBellAreaComparison)) {
        LOG_ERROR(Debug,
                  "Bloodborne active Bell area signature mismatch at {:#x}: expected={} "
                  "actual={}",
                  ActiveBellAreaComparisonOffset, BytesToHex(ActiveBellAreaComparison),
                  BytesToHex(actual_active_bell_area_comparison));
        return;
    }

    auto* active_bell_area_flag_result =
        reinterpret_cast<u8*>(base + ActiveBellAreaFlagResultOffset);
    const auto actual_active_bell_area_flag_result =
        std::span<const u8>{active_bell_area_flag_result, ActiveBellAreaFlagResult.size()};
    if (!std::ranges::equal(actual_active_bell_area_flag_result, ActiveBellAreaFlagResult)) {
        LOG_ERROR(Debug,
                  "Bloodborne active Bell area-flag signature mismatch at {:#x}: expected={} "
                  "actual={}",
                  ActiveBellAreaFlagResultOffset, BytesToHex(ActiveBellAreaFlagResult),
                  BytesToHex(actual_active_bell_area_flag_result));
        return;
    }

    auto* sos_status_area_restriction =
        reinterpret_cast<u8*>(base + SosStatusAreaRestrictionOffset);
    const auto actual_sos_status_area_restriction =
        std::span<const u8>{sos_status_area_restriction, SosStatusAreaRestriction.size()};
    if (!std::ranges::equal(actual_sos_status_area_restriction, SosStatusAreaRestriction)) {
        LOG_ERROR(Debug,
                  "Bloodborne SOS-status area signature mismatch at {:#x}: expected={} "
                  "actual={}",
                  SosStatusAreaRestrictionOffset, BytesToHex(SosStatusAreaRestriction),
                  BytesToHex(actual_sos_status_area_restriction));
        return;
    }

    std::memcpy(area_comparison, BeckoningSignedAreaComparison.data(),
                BeckoningSignedAreaComparison.size());
    std::memcpy(area_flag_result, BeckoningAllowedAreaFlagResult.data(),
                BeckoningAllowedAreaFlagResult.size());
    std::memcpy(active_bell_area_comparison, ActiveBellSignedAreaComparison.data(),
                ActiveBellSignedAreaComparison.size());
    std::memcpy(active_bell_area_flag_result, ActiveBellAllowedAreaFlagResult.data(),
                ActiveBellAllowedAreaFlagResult.size());
    std::memcpy(sos_status_area_restriction, SosStatusAllowedArea.data(),
                SosStatusAllowedArea.size());
    negative_area_patch_installed = true;
    area_flag_patch_installed = true;
    active_bell_negative_area_patch_installed = true;
    active_bell_area_flag_patch_installed = true;
    sos_status_area_patch_installed = true;
    LOG_INFO(Debug,
             "Bloodborne seamless patches enabled Beckoning Bell use and active search in "
             "negative and area-complete SOS regions at {:#x}/{:#x}/{:#x}/{:#x}/{:#x}",
             BeckoningAreaComparisonOffset, BeckoningAreaFlagResultOffset,
             ActiveBellAreaComparisonOffset, ActiveBellAreaFlagResultOffset,
             SosStatusAreaRestrictionOffset);
}

void InstallReverseEngineeringTrace() {
    if (installed || !EnvFlagEnabled("SHADPS4_BLOODBORNE_RE_TRACE")) {
        return;
    }
    if (MemoryPatcher::g_game_serial != "CUSA03173") {
        LOG_WARNING(Debug, "Bloodborne RE trace ignored for title {}",
                    MemoryPatcher::g_game_serial);
        return;
    }

    const auto* param_sfo = Common::Singleton<PSF>::Instance();
    const std::string_view app_version = param_sfo->GetString("APP_VER").value_or("Unknown");
    if (app_version != "01.09") {
        LOG_ERROR(Debug, "Bloodborne RE trace requires CUSA03173 01.09; loaded version is {}",
                  app_version);
        return;
    }

    image_base = MemoryPatcher::g_eboot_address;
    if (image_base == 0 || !VerifySites()) {
        LOG_ERROR(Debug, "Bloodborne RE trace was not installed");
        return;
    }

    const auto capture_dir =
        Common::FS::GetUserPath(Common::FS::PathType::CapturesDir) / "bloodborne-re";
    std::error_code error;
    std::filesystem::create_directories(capture_dir, error);
    if (error) {
        LOG_ERROR(Debug, "Could not create Bloodborne RE capture directory {}: {}",
                  Common::FS::PathToUTF8String(capture_dir), error.message());
        return;
    }

    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    const int process_id = Debugger::GetCurrentPid();
    capture_path = capture_dir / ("cusa03173-109-p" + std::to_string(process_id) + '-' +
                                  std::to_string(timestamp) + ".jsonl");
    capture_file.open(capture_path, std::ios::out | std::ios::trunc);
    if (!capture_file) {
        LOG_ERROR(Debug, "Could not open Bloodborne RE capture {}",
                  Common::FS::PathToUTF8String(capture_path));
        return;
    }
    capture_file << "{\"type\":\"header\",\"title\":\"CUSA03173\","
                    "\"app_version\":\"01.09\",\"image_base\":\"0x"
                 << std::hex << image_base << std::dec << "\",\"process_id\":" << process_id
                 << ",\"site_count\":" << Sites.size() << ",\"negative_area_patch\":"
                 << (negative_area_patch_installed ? "true" : "false")
                 << ",\"area_flag_patch\":" << (area_flag_patch_installed ? "true" : "false")
                 << ",\"active_bell_negative_area_patch\":"
                 << (active_bell_negative_area_patch_installed ? "true" : "false")
                 << ",\"active_bell_area_flag_patch\":"
                 << (active_bell_area_flag_patch_installed ? "true" : "false")
                 << ",\"sos_status_area_patch\":"
                 << (sos_status_area_patch_installed ? "true" : "false") << "}\n";
    capture_file.flush();

    size_t hook_count = 0;
    for (size_t index = 0; index < Sites.size(); ++index) {
        const auto& site = Sites[index];
        const auto expected = std::span<const u8>{site.prologue.data(), site.prologue_size};
        if (!InstallGuestCodeHook(reinterpret_cast<void*>(image_base + site.offset), expected,
                                  index, TraceEntry)) {
            LOG_ERROR(Debug, "Failed to install Bloodborne RE hook {} at {:#x}", site.name,
                      site.offset);
            continue;
        }
        ++hook_count;
    }

    installed = hook_count == Sites.size();
    LOG_INFO(Debug, "Bloodborne RE trace installed {}/{} hooks; capture={}", hook_count,
             Sites.size(), Common::FS::PathToUTF8String(capture_path));
}

} // namespace Core::Bloodborne
