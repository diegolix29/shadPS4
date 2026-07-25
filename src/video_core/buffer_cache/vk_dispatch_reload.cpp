// SPDX-License-Identifier: GPL-2.0-or-later
// Resolve vkCmdBindVertexBuffers2 via an explicitly supplied vkGetDeviceProcAddr.
//
// The original implementation called vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetDeviceProcAddr")
// to obtain gdpa without depending on VK_NO_PROTOTYPES / Vulkan-Hpp.  That call returns null in
// Vortek and FEX environments where vkGetInstanceProcAddr is a RPC trampoline that requires a
// valid VkInstance handle instead of the loader-level VK_NULL_HANDLE shortcut.
//
// The new signature receives the already-populated gdpa from the Vulkan-Hpp dispatcher
// (VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr), which is set correctly by
// VULKAN_HPP_DEFAULT_DISPATCHER.init(*instance) and subsequently by init(*device).
// LTO cannot trace through the function-pointer indirection; -fno-lto is applied to this TU
// in CMakeLists so the call is never optimised away.

#include <vulkan/vulkan_core.h>

extern "C" {

__attribute__((noinline)) PFN_vkCmdBindVertexBuffers2
BachataResolveBindVB2(VkDevice device, PFN_vkGetDeviceProcAddr gdpa, const char** out_source) {
    if (!gdpa || !device) {
        if (out_source) *out_source = "none";
        return nullptr;
    }
    auto fn = reinterpret_cast<PFN_vkCmdBindVertexBuffers2>(
        gdpa(device, "vkCmdBindVertexBuffers2"));
    if (fn) {
        if (out_source) *out_source = "core";
        return fn;
    }
    fn = reinterpret_cast<PFN_vkCmdBindVertexBuffers2>(
        gdpa(device, "vkCmdBindVertexBuffers2EXT"));
    if (fn) {
        if (out_source) *out_source = "ext";
        return fn;
    }
    if (out_source) *out_source = "none";
    return nullptr;
}

// Resolve the core 1.0 vkCmdBindVertexBuffers. GDPA returns all device-level
// entry points (core + extensions), so this works even when Vulkan-Hpp's
// dispatcher slot is null because init(*instance) was never called — which is
// the case in the Vortek RPC bridge where only init(*device) runs.
__attribute__((noinline)) PFN_vkCmdBindVertexBuffers
BachataResolveBindVB(VkDevice device, PFN_vkGetDeviceProcAddr gdpa) {
    if (!gdpa || !device) {
        return nullptr;
    }
    return reinterpret_cast<PFN_vkCmdBindVertexBuffers>(
        gdpa(device, "vkCmdBindVertexBuffers"));
}

} // extern "C"
