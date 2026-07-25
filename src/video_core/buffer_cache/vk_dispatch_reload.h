// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

// Resolve vkCmdBindVertexBuffers2 using an explicitly supplied vkGetDeviceProcAddr.
// The caller must pass the gdpa already stored in the Vulkan-Hpp dispatcher so that
// this function never calls vkGetInstanceProcAddr(VK_NULL_HANDLE), which returns
// null in Vortek / FEX environments that require a valid instance handle.
__attribute__((noinline)) PFN_vkCmdBindVertexBuffers2
BachataResolveBindVB2(VkDevice device, PFN_vkGetDeviceProcAddr gdpa, const char** out_source = nullptr);

// Resolve the core 1.0 vkCmdBindVertexBuffers via an explicitly supplied
// vkGetDeviceProcAddr. Vulkan-Hpp's init(*device) does not populate core 1.0
// dispatcher slots unless init(*instance) was called first; in the Vortek RPC
// bridge only init(*device) runs, leaving vkCmdBindVertexBuffers null. This
// resolver fetches it directly so the dynamic-state BindVertexBuffers path does
// not call through a null function pointer.
__attribute__((noinline)) PFN_vkCmdBindVertexBuffers
BachataResolveBindVB(VkDevice device, PFN_vkGetDeviceProcAddr gdpa);

#ifdef __cplusplus
}
#endif
