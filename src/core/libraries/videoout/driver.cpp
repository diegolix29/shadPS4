// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/config.h"
#include "common/debug.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/videoout/driver.h"
#include "core/libraries/videoout/videoout_error.h"
#include "imgui/renderer/imgui_core.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Libraries::VideoOut {

constexpr static bool Is32BppPixelFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::A8R8G8B8Srgb:
    case PixelFormat::A8B8G8R8Srgb:
    case PixelFormat::A2R10G10B10:
    case PixelFormat::A2R10G10B10Srgb:
    case PixelFormat::A2R10G10B10Bt2020Pq:
        return true;
    default:
        return false;
    }
}

constexpr u32 PixelFormatBpp(PixelFormat pixel_format) {
    switch (pixel_format) {
    // 8-bit formats
    case PixelFormat::R8Unorm:
    case PixelFormat::R8Snorm:
    case PixelFormat::R8Uint:
    case PixelFormat::R8Sint:
        return 1;

    // 16-bit formats
    case PixelFormat::R16Unorm:
    case PixelFormat::R16Snorm:
    case PixelFormat::R16Uint:
    case PixelFormat::R16Sint:
    case PixelFormat::R16Float:
        return 2;

    case PixelFormat::R8G8Unorm:
    case PixelFormat::R8G8Snorm:
    case PixelFormat::R8G8Uint:
    case PixelFormat::R8G8Sint:
    case PixelFormat::R16G16Unorm:
    case PixelFormat::R16G16Snorm:
    case PixelFormat::R16G16Uint:
    case PixelFormat::R16G16Sint:
    case PixelFormat::R16G16Float:
        return 4;

    // 32-bit formats
    case PixelFormat::R32Uint:
    case PixelFormat::R32Sint:
    case PixelFormat::R32Float:
        return 4;

    // 64-bit formats
    case PixelFormat::R32G32Uint:
    case PixelFormat::R32G32Sint:
    case PixelFormat::R32G32Float:
    case PixelFormat::R16G16B16A16Unorm:
    case PixelFormat::R16G16B16A16Snorm:
    case PixelFormat::R16G16B16A16Uint:
    case PixelFormat::R16G16B16A16Sint:
    case PixelFormat::R16G16B16A16Float:
        return 8;

    // 128-bit formats
    case PixelFormat::R32G32B32A32Uint:
    case PixelFormat::R32G32B32A32Sint:
    case PixelFormat::R32G32B32A32Float:
        return 16;

    // 4-byte packed formats (already in your code)
    case PixelFormat::A8R8G8B8Srgb:
    case PixelFormat::A8B8G8R8Srgb:
    case PixelFormat::A2R10G10B10:
    case PixelFormat::A2R10G10B10Srgb:
    case PixelFormat::A2R10G10B10Bt2020Pq:
        return 4;

    case PixelFormat::A16R16G16B16Float:
        return 8;

    // Depth/stencil and HDR formats
    case PixelFormat::D16Unorm:
        return 2;
    case PixelFormat::D24UnormS8Uint:
        return 4;
    case PixelFormat::D32Float:
        return 4;
    case PixelFormat::D32FloatS8Uint:
        return 8;

    case PixelFormat::R10G10B10A2Unorm:
    case PixelFormat::R10G10B10A2Uint:
        return 4;
    case PixelFormat::B10G11R11Float:
        return 4;
    case PixelFormat::E5B9G9R9Float:
        return 4;

    default:
        return 4;
    }
}

VideoOutDriver::VideoOutDriver(u32 width, u32 height) {
    main_port.resolution.full_width = width;
    main_port.resolution.full_height = height;
    main_port.resolution.pane_width = width;
    main_port.resolution.pane_height = height;
    present_thread = std::jthread([&](std::stop_token token) { PresentThread(token); });
}

VideoOutDriver::~VideoOutDriver() = default;

int VideoOutDriver::Open(const ServiceThreadParams* params) {
    if (main_port.is_open) {
        return ORBIS_VIDEO_OUT_ERROR_RESOURCE_BUSY;
    }
    main_port.is_open = true;
    liverpool->SetVoPort(&main_port);
    return 1;
}

void VideoOutDriver::Close(s32 handle) {
    std::scoped_lock lock{mutex};

    main_port.is_open = false;
    main_port.flip_rate = 0;
    main_port.prev_index = -1;
    ASSERT(main_port.flip_events.empty());
}

VideoOutPort* VideoOutDriver::GetPort(int handle) {
    if (handle != 1) [[unlikely]] {
        return nullptr;
    }
    return &main_port;
}

int VideoOutDriver::RegisterBuffers(VideoOutPort* port, s32 startIndex, void* const* addresses,
                                    s32 bufferNum, const BufferAttribute* attribute) {
    const s32 group_index = port->FindFreeGroup();
    if (group_index >= MaxDisplayBufferGroups) {
        return ORBIS_VIDEO_OUT_ERROR_NO_EMPTY_SLOT;
    }

    if (startIndex + bufferNum > MaxDisplayBuffers || startIndex > MaxDisplayBuffers ||
        bufferNum > MaxDisplayBuffers) {
        LOG_ERROR(Lib_VideoOut,
                  "Attempted to register too many buffers startIndex = {}, bufferNum = {}",
                  startIndex, bufferNum);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    const s32 end_index = startIndex + bufferNum;
    if (bufferNum > 0 &&
        std::any_of(port->buffer_slots.begin() + startIndex, port->buffer_slots.begin() + end_index,
                    [](auto& buffer) { return buffer.group_index != -1; })) {
        return ORBIS_VIDEO_OUT_ERROR_SLOT_OCCUPIED;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "startIndex = {}, bufferNum = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             startIndex, bufferNum, GetPixelFormatString(attribute->pixel_format),
             attribute->aspect_ratio, static_cast<u32>(attribute->tiling_mode), attribute->width,
             attribute->height, attribute->pitch_in_pixel, attribute->option);

    auto& group = port->groups[group_index];
    std::memcpy(&group.attrib, attribute, sizeof(BufferAttribute));
    group.is_occupied = true;

    for (u32 i = 0; i < bufferNum; i++) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(addresses[i]);
        port->buffer_slots[startIndex + i] = VideoOutBuffer{
            .group_index = group_index,
            .address_left = address,
            .address_right = 0,
        };

        // Reset flip label also when registering buffer
        port->buffer_labels[startIndex + i] = 0;
        port->SignalVoLabel();

        presenter->RegisterVideoOutSurface(group, address);
        LOG_INFO(Lib_VideoOut, "buffers[{}] = {:#x}", i + startIndex, address);
    }

    return group_index;
}

int VideoOutDriver::UnregisterBuffers(VideoOutPort* port, s32 attributeIndex) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    auto& group = port->groups[attributeIndex];
    group.is_occupied = false;

    for (auto& buffer : port->buffer_slots) {
        if (buffer.group_index != attributeIndex) {
            continue;
        }
        buffer.group_index = -1;
    }

    return ORBIS_OK;
}

void VideoOutDriver::Flip(const Request& req) {
    // Update HDR status before presenting.
    presenter->SetHDR(req.port->is_hdr);

    // Present the frame.
    presenter->Present(req.frame);

    // Update flip status.
    auto* port = req.port;
    {
        std::unique_lock lock{port->port_mutex};
        auto& flip_status = port->flip_status;
        flip_status.count++;
        flip_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
        flip_status.tsc = Libraries::Kernel::sceKernelReadTsc();
        flip_status.flip_arg = req.flip_arg;
        flip_status.current_buffer = req.index;
        if (req.eop) {
            --flip_status.gc_queue_num;
        }
        --flip_status.flip_pending_num;
    }

    // Trigger flip events for the port.
    for (auto& event : port->flip_events) {
        if (event != nullptr) {
            event->TriggerEvent(
                static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                Kernel::SceKernelEvent::Filter::VideoOut,
                reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Flip) |
                                        (req.flip_arg << 16)));
        }
    }

    // Reset prev flip label
    if (port->prev_index != -1) {
        port->buffer_labels[port->prev_index] = 0;
        port->SignalVoLabel();
    }
    // save to prev buf index
    port->prev_index = req.index;
}

void VideoOutDriver::DrawBlankFrame() {
    const auto empty_frame = presenter->PrepareBlankFrame(false);
    presenter->Present(empty_frame);
}

void VideoOutDriver::DrawLastFrame() {
    const auto frame = presenter->PrepareLastFrame();
    if (frame != nullptr) {
        presenter->Present(frame, true);
    }
}

bool VideoOutDriver::SubmitFlip(VideoOutPort* port, s32 index, s64 flip_arg,
                                bool is_eop /*= false*/) {
    {
        std::unique_lock lock{port->port_mutex};
        if (index != -1 && port->flip_status.flip_pending_num >= port->NumRegisteredBuffers()) {
            LOG_ERROR(Lib_VideoOut, "Flip queue is full");
            return false;
        }

        if (is_eop) {
            ++port->flip_status.gc_queue_num;
        }
        ++port->flip_status.flip_pending_num; // integral GPU and CPU pending flips counter
        port->flip_status.submit_tsc = Libraries::Kernel::sceKernelReadTsc();
    }

    if (!is_eop) {
        // Non EOP flips can arrive from any thread so ask GPU thread to perform them
        liverpool->SendCommand([=, this]() { SubmitFlipInternal(port, index, flip_arg, is_eop); });
    } else {
        SubmitFlipInternal(port, index, flip_arg, is_eop);
    }

    return true;
}

void VideoOutDriver::SubmitFlipInternal(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop) {
    Vulkan::Frame* frame;
    if (index == -1) {
        frame = presenter->PrepareBlankFrame(false);
    } else {
        const auto& buffer = port->buffer_slots[index];
        const auto& group = port->groups[buffer.group_index];
        frame = presenter->PrepareFrame(group, buffer.address_left);
    }

    std::scoped_lock lock{mutex};
    requests.push({
        .frame = frame,
        .port = port,
        .flip_arg = flip_arg,
        .index = index,
        .eop = is_eop,
    });
}

void VideoOutDriver::PresentThread(std::stop_token token) {
    // Use 64-bit integers for nanosecond arithmetic to avoid overflow/truncation.
    int64_t fps_cap_value_ns = 0;
    constexpr int64_t kNanosPerSec = 1'000'000'000LL;

    if (Config::isFpsLimiterEnabled()) {
        const auto fps_limit = static_cast<int64_t>(Config::getFpsLimit());
        if (fps_limit > 0) {
            fps_cap_value_ns = kNanosPerSec / fps_limit; // nanoseconds per frame
        } else {
            // If fps_limit is 0 (invalid), fall back to vblank frequency below.
            fps_cap_value_ns = 0;
        }
    }

    if (fps_cap_value_ns == 0) {
        // Either limiter disabled or limiter produced 0 (or invalid fps). Use vblank frequency.
        const auto vblank_freq = static_cast<int64_t>(Config::vblankFreq());
        if (vblank_freq > 0) {
            fps_cap_value_ns = kNanosPerSec / vblank_freq; // nanoseconds per vblank
        } else {
            // As a last resort: clamp to 1ms per frame (1000000 ns) to avoid zero-duration.
            fps_cap_value_ns = 1'000'000LL;
        }
    }

    // Ensure at least 1 ns to avoid zero-duration timers.
    if (fps_cap_value_ns <= 0) {
        fps_cap_value_ns = 1;
    }

    const std::chrono::nanoseconds FpsCap{static_cast<long long>(fps_cap_value_ns)};

    Common::SetCurrentThreadName("shadPS4:PresentThread");
    Common::SetCurrentThreadRealtime(FpsCap);
    Common::AccurateTimer timer{FpsCap};

    const auto receive_request = [this] -> Request {
        std::scoped_lock lk{mutex};
        if (!requests.empty()) {
            const auto request = requests.front();
            requests.pop();
            return request;
        }
        return {};
    };

    while (!token.stop_requested()) {
        timer.Start();

        if (DebugState.IsGuestThreadsPaused()) {
            DrawLastFrame();
            timer.End();
            continue;
        }

        // Check if it's time to take a request.
        auto& vblank_status = main_port.vblank_status;

        // Use wide unsigned arithmetic to avoid wrap when adding 1.
        const uint64_t flip_rate_plus_one = static_cast<uint64_t>(main_port.flip_rate) + 1ULL;
        if (flip_rate_plus_one > 0) {
            if ((static_cast<uint64_t>(vblank_status.count) % flip_rate_plus_one) == 0ULL) {
                const auto request = receive_request();
                if (!request) {
                    if (timer.GetTotalWait().count() < 0) { // Don't draw too fast
                        if (!main_port.is_open) {
                            DrawBlankFrame();
                        } else if (ImGui::Core::MustKeepDrawing()) {
                            DrawLastFrame();
                        }
                    }
                } else {
                    Flip(request);
                    FRAME_END;
                }
            }
        } else {
            // Defensive fallback if flip_rate_plus_one somehow became zero (shouldn't happen),
            // treat as always true.
            const auto request = receive_request();
            if (!request) {
                if (timer.GetTotalWait().count() < 0) {
                    if (!main_port.is_open) {
                        DrawBlankFrame();
                    } else if (ImGui::Core::MustKeepDrawing()) {
                        DrawLastFrame();
                    }
                }
            } else {
                Flip(request);
                FRAME_END;
            }
        }

        {
            // Needs lock here as can be concurrently read by `sceVideoOutGetVblankStatus`
            std::scoped_lock lock{main_port.vo_mutex};
            vblank_status.count++;
            vblank_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
            vblank_status.tsc = Libraries::Kernel::sceKernelReadTsc();
            main_port.vblank_cv.notify_all();
        }

        // Trigger flip events for the port.
        for (auto& event : main_port.vblank_events) {
            if (event != nullptr) {
                event->TriggerEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                                    Kernel::SceKernelEvent::Filter::VideoOut, nullptr);
            }
        }

        timer.End();
    }
}

} // namespace Libraries::VideoOut