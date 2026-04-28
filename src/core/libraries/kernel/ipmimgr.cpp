// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "core/libraries/kernel/ipmimgr.h"
#include "core/libraries/libs.h"

namespace Libraries::Kernel {

static constexpr u64 IPMI_MAGIC = 0xDEADBADECAFEBEAF;

// ---------------------------------------------------------------------------
// IPMI kernel-level message router (v113 — expanded from libSceIpmi.sprx RE)
//
// On real PS4, ipmimgr_call is a kernel syscall that routes IPC messages
// between client/server endpoints. Output is always a simple s32 KID
// (kernel ID), NOT a C++ object pointer.
//
// Opcodes from libSceIpmi.sprx RE (PRX wrapper → ipmimgr_call opcode):
//   0x000-0x005: Create/Destroy client/server
//   0x201:       Connect
//   0x232:       InvokeSyncMethod
//   0x251-0x255: Event flag + async method operations
//   0x302-0x310: Server receive/send
//   0x400:       CompleteAsyncDispatch
//   0x490-0x493: CreateSession variants
//   0x520:       DestroySession
// ---------------------------------------------------------------------------
static s32 s_next_kid = 0x100;

int PS4_SYSV_ABI ipmimgr_call(int opcode, int sub_id, void* output_ptr,
                               void* input_data, int input_size, u64 magic) {
    if (magic != IPMI_MAGIC) {
        LOG_ERROR(Lib_Kernel, "[IPMI] Invalid magic {:#x}", magic);
        return -1;
    }

    auto* out = reinterpret_cast<s32*>(output_ptr);

    switch (opcode) {
    case 0x000: // CreateServer
    case 0x001: // CreateServer variant
    {
        s32 kid = s_next_kid++;
        if (out) *out = kid;
        LOG_INFO(Lib_Kernel, "[IPMI] CreateServer op={:#x} sub={:#x} -> kid={:#x}",
                 opcode, sub_id, kid);
        return 0;
    }

    case 0x002: { // CreateClient
        s32 kid = s_next_kid++;
        if (out) *out = kid;
        // CRITICAL: The libSceIpmi wrapper packs the caller's output buffer
        // pointer at input_data[8]. The kernel writes the kid there so the
        // original caller (e.g. libSceScreenShot module_start) can read it.
        if (input_data && input_size >= 16) {
            auto* in = reinterpret_cast<u64*>(input_data);
            auto* buf = reinterpret_cast<u64*>(in[1]);
            if (buf) *buf = static_cast<u64>(kid);
            LOG_INFO(Lib_Kernel, "[IPMI] CreateClient: kid={:#x} "
                     "wrote to caller buf @{:#x}",
                     kid, reinterpret_cast<u64>(buf));
        } else {
            LOG_INFO(Lib_Kernel, "[IPMI] CreateClient: kid={:#x} (no input buf)",
                     kid);
        }
        return 0;
    }

    case 0x003: // DestroyClient
        if (out) *out = 0;
        LOG_INFO(Lib_Kernel, "[IPMI] DestroyClient sub={:#x}", sub_id);
        return 0;

    case 0x004: // Destroy
    case 0x005: // DestroyServer
        if (out) *out = 0;
        return 0;

    case 0x201: { // Connect
        s32 kid = s_next_kid++;
        if (out) *out = kid;
        // Log full input for debugging — do NOT write through input pointers
        // (Connect input struct has different layout than CreateClient)
        if (input_data && input_size >= 8) {
            auto* in = reinterpret_cast<u64*>(input_data);
            LOG_INFO(Lib_Kernel, "[IPMI] Connect sub={:#x} -> kid={:#x} "
                     "in[0]={:#x} in[1]={:#x} in[2]={:#x} in[3]={:#x} in_sz={}",
                     sub_id, kid,
                     in[0], 
                     input_size >= 16 ? in[1] : 0,
                     input_size >= 24 ? in[2] : 0,
                     input_size >= 32 ? in[3] : 0,
                     input_size);
        } else {
            LOG_INFO(Lib_Kernel, "[IPMI] Connect sub={:#x} -> kid={:#x}", sub_id, kid);
        }
        return 0;
    }

    case 0x232: { // InvokeSyncMethod
        u32 method_id = 0;
        if (input_data && input_size >= 4) {
            method_id = *reinterpret_cast<u32*>(input_data);
        }
        if (out) *out = 0;
        LOG_INFO(Lib_Kernel, "[IPMI] InvokeSyncMethod sub={:#x} method={:#x} in_sz={}",
                 sub_id, method_id, input_size);
        return 0;
    }

    // v113: InvokeAsyncMethod — called by gallery for photo data delivery
    // PRX wrappers: 0x4cc0 (opcode 0x253), 0x4d80 (opcode 0x254)
    // Input struct for 0x253: {method_id(u32), data_ptr(u64), size(u64), callback(u64)} = 0x20 bytes
    // Input struct for 0x254: {method_id(u32), data_ptr(u64), callback(u64)} = 0x18 bytes
    case 0x253: { // InvokeAsyncMethod (full)
        u32 method_id = 0;
        u64 data_ptr = 0;
        u64 data_size = 0;
        u64 callback = 0;
        if (input_data && input_size >= 0x20) {
            auto* in = reinterpret_cast<u8*>(input_data);
            method_id = *reinterpret_cast<u32*>(in + 0x00);
            data_ptr   = *reinterpret_cast<u64*>(in + 0x08);
            data_size  = *reinterpret_cast<u64*>(in + 0x10);
            callback   = *reinterpret_cast<u64*>(in + 0x18);
        }
        if (out) *out = 0;
        LOG_INFO(Lib_Kernel, "[IPMI] InvokeAsyncMethod sub={:#x} method={:#x} "
                 "data={:#x} sz={:#x} cb={:#x}",
                 sub_id, method_id, data_ptr, data_size, callback);
        return 0;
    }

    case 0x254: { // InvokeAsyncMethod variant (no size field)
        u32 method_id = 0;
        u64 data_ptr = 0;
        u64 callback = 0;
        if (input_data && input_size >= 0x18) {
            auto* in = reinterpret_cast<u8*>(input_data);
            method_id = *reinterpret_cast<u32*>(in + 0x00);
            data_ptr   = *reinterpret_cast<u64*>(in + 0x08);
            callback   = *reinterpret_cast<u64*>(in + 0x10);
        }
        if (out) *out = 0;
        LOG_INFO(Lib_Kernel, "[IPMI] InvokeAsyncMethod_v2 sub={:#x} method={:#x} "
                 "data={:#x} cb={:#x}",
                 sub_id, method_id, data_ptr, callback);
        return 0;
    }

    case 0x255: { // PollAsyncResult
        u32 request_id = 0;
        if (input_data && input_size >= 4) {
            request_id = *reinterpret_cast<u32*>(input_data);
        }
        // Return "not ready" — the async operation never completes in our stub
        if (out) *out = 0;
        LOG_DEBUG(Lib_Kernel, "[IPMI] PollAsyncResult sub={:#x} req={:#x}",
                  sub_id, request_id);
        return 0;
    }

    case 0x271: // PollEventFlag
    case 0x251: // WaitEventFlag
    case 0x252: // PollEventFlag variant
        if (out) *out = 0;
        return 0;

    case 0x302: // ServerReceivePacket
    case 0x303: // ServerReceivePacket variant
    case 0x310: // SendConnectResult
        if (out) *out = 0;
        return 0;

    case 0x400: {
        // CompleteAsyncDispatch
        if (out) *out = 0;
        if (input_data && input_size >= 0x20) {
            auto* input = reinterpret_cast<u64*>(input_data);
            if (auto* p = reinterpret_cast<s32*>(input[2])) *p = 0;
            if (auto* p = reinterpret_cast<s32*>(input[3])) *p = 0;
        }
        return 0;
    }

    case 0x490:
    case 0x491:
    case 0x492:
    case 0x493: { // v113: added 0x493 (PRX wrapper at 0x5750)
        s32 kid = s_next_kid++;
        if (out) *out = kid;
        LOG_INFO(Lib_Kernel, "[IPMI] CreateSession op={:#x} sub={:#x} -> kid={:#x}",
                 opcode, sub_id, kid);
        return 0;
    }

    case 0x520: // DestroySession
        if (out) *out = 0;
        return 0;

    default:
        if (out) *out = 0;
        if (input_data && input_size >= 8) {
            auto* in = reinterpret_cast<u64*>(input_data);
            LOG_INFO(Lib_Kernel, "[IPMI] op={:#x} sub={:#x} in_sz={:#x} "
                     "in[0]={:#x} in[1]={:#x}",
                     opcode, sub_id, input_size, in[0],
                     input_size >= 16 ? in[1] : 0);
        } else {
            LOG_INFO(Lib_Kernel, "[IPMI] op={:#x} sub={:#x} in_sz={:#x}",
                     opcode, sub_id, input_size);
        }
        return 0;
    }
}

void RegisterIpmi(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("Hk7iHmGxB18", "libkernel", 1, "libkernel", ipmimgr_call);
}

} // namespace Libraries::Kernel
