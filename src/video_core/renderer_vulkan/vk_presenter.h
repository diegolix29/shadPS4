// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>
#include <queue>

#include "core/libraries/videoout/buffer.h"
#include "imgui/imgui_texture.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/vk_display_pacer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Frontend {
class WindowSDL;
}

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {

struct Frame {
    VmaAllocation allocation{};
    vk::Image image{};
    vk::ImageView image_view{};
    vk::Fence present_done{};
    vk::Semaphore ready_semaphore{};
    u64 ready_tick{};
    /// Guest frame number from the command processor; 0 for frames made by the host.
    u64 frame_id{};
    /// Host time of the guest vblank that latched the frame; 0 when not latched.
    s64 latch_ns{};
    ImTextureID imgui_texture{};
    u32 width{};
    u32 height{};
    bool is_hdr{false};
    u8 id{};
};

class Rasterizer;

class Presenter {
public:
    struct PresentTimingFeedback {
        s64 last_present_call_ns;
        s64 present_call_period_ns;
        u64 generation;
        u32 present_call_samples;
        bool is_fifo;
        /// The display holds frames on its own cadence and the present waits steer the guest
        /// vblank phase through correction_ns, which changes with correction_seq.
        bool display_locked;
        u64 correction_seq;
        s64 correction_ns;
    };

    Presenter(Frontend::WindowSDL& window, AmdGpu::Liverpool* liverpool);
    void UpdateFsrSettingsFromConfig();
    ~Presenter();

    HostPasses::PostProcessingPass::Settings& GetPPSettingsRef() {
        return pp_settings;
    }

    HostPasses::FsrPass::Settings& GetFsrSettingsRef() {
        return fsr_settings;
    }

    Frontend::WindowSDL& GetWindow() const {
        return window;
    }

    Rasterizer& GetRasterizer() const {
        return *rasterizer.get();
    }

    void SyncPipelineCache();

    bool IsHDRSupported() const {
        return swapchain.HasHDR();
    }

    void SetHDR(bool enable) {
        if (!IsHDRSupported()) {
            return;
        }
        const bool changed = swapchain.GetHDR() != enable;
        swapchain.SetHDR(enable);
        if (changed) {
            ResetFifoTimingFeedback();
        }
        pp_settings.hdr = enable ? 1 : 0;
    }

    VideoCore::Image& RegisterVideoOutSurface(
        const Libraries::VideoOut::BufferAttributeGroup& attribute, VAddr cpu_address) {
        vo_buffers_addr.emplace_back(cpu_address);
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        const auto image_id = texture_cache.FindImage(desc);
        auto& image = texture_cache.GetImage(image_id);
        image.usage.vo_surface = 1u;
        return image;
    }

    bool IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const;

    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                        VAddr cpu_address);

    Frame* PrepareBlankFrame(bool present_thread);

    void Present(Frame* frame, bool is_reusing_frame = false, u64 presentation_epoch = 0);
    Frame* PrepareLastFrame();

    /// Returns a frame to the producer pool once its rendering and any presentation of it have
    /// completed. This never waits on the caller and is safe for mailbox replacement on the
    /// vblank thread.
    void RecycleFrameAsync(Frame* frame);

    PresentTimingFeedback GetPresentTimingFeedback() const;

    /// Asks the vblank thread to keep a flip pending at this vblank. While the display takes
    /// frames on its own cadence, a flip is latched at the last vblank that still makes the next
    /// scanout, and never while the previous guest frame sits finished behind another one. The
    /// guest then waits for its flip as on a display of that rate, and no frame it rendered is
    /// thrown away. previous_frame_id is the guest frame of the last latched flip.
    [[nodiscard]] bool ShouldHoldFlip(s64 tick_ns, s64 next_tick_ns, u64 previous_frame_id);

    bool CoalescesPendingFrames() const {
        return swapchain.IsMailbox() || swapchain.IsFIFO();
    }

    /// Invalidates feedback from work belonging to an older video-out lifecycle.
    void SetPresentationEpoch(u64 epoch);

private:
    Frame* GetRenderFrame();

    void RecreateFrame(Frame* frame, u32 width, u32 height);

    void RecreateSwapchain();

    void ResetFifoTimingFeedback();

    void ResetFifoTimingFeedbackLocked();

    void ClearPresentCallHistoryLocked();

    void RecordPresentCall(u64 epoch);

    /// Present ids carry the guest frame number in their upper bits; the low bits number the
    /// re-presentations of one frame.
    static constexpr u32 PresentIdFrameShift = 16;
    [[nodiscard]] static constexpr u64 PresentIdOfFrame(u64 frame_id) {
        return frame_id << PresentIdFrameShift;
    }

    /// Picks the present id of the next present; 0 when it cannot carry one.
    [[nodiscard]] u64 NextPresentId(const Frame* frame, bool is_reusing_frame);

    /// Whether present waits pace the presentation of this swapchain.
    [[nodiscard]] bool UsesDisplayPacing() const;

    /// Command processor side end of a guest frame: NVIDIA Reflex markers and sleep, then the
    /// next frame number.
    void EndGuestFrame();

    /// Times the end of the presentation work of each guest frame on the GPU.
    void PresentReadyThread(std::stop_token token);

    /// Times the scanout of each guest frame and feeds the display pacer.
    void PresentWaitThread(std::stop_token token);

    void RecycleThread(std::stop_token token);

    void ReturnFrame(Frame* frame);

    void SetExpectedGameSize(s32 width, s32 height);

private:
    float expected_ratio{1920.0 / 1080.0f};
    u32 expected_frame_width{1920};
    u32 expected_frame_height{1080};

    Frontend::WindowSDL& window;
    Instance instance;
    HostPasses::FsrPass fsr_pass;
    HostPasses::FsrPass::Settings fsr_settings{};
    HostPasses::PostProcessingPass::Settings pp_settings{};
    HostPasses::PostProcessingPass pp_pass;
    AmdGpu::Liverpool* liverpool;
    Scheduler draw_scheduler;
    Scheduler present_scheduler;
    Swapchain swapchain;
    std::unique_ptr<Rasterizer> rasterizer;
    VideoCore::TextureCache& texture_cache;
    vk::UniqueCommandPool command_pool;
    std::vector<Frame> present_frames;
    std::queue<Frame*> free_queue;
    Frame* last_submit_frame{};
    static constexpr u32 MaxGpuFramesAhead = 3;
    /// Guest frames the GPU may still be rendering when the command processor finishes another.
    u32 gpu_frames_ahead{};
    /// Ready ticks of the latest guest frames, indexed by frame number modulo the ring size.
    std::array<u64, MaxGpuFramesAhead + 1> guest_frame_ticks{};
    u64 guest_frame_count{};
    std::mutex free_mutex;
    std::condition_variable free_cv;
    std::mutex recycle_mutex;
    std::condition_variable_any recycle_cv;
    std::queue<Frame*> recycle_queue;
    std::jthread recycle_thread;
    std::optional<ImGui::RefCountedTexture> splash_img;
    std::vector<VAddr> vo_buffers_addr;
    std::atomic<s64> last_present_call_ns{};
    std::atomic<s64> present_call_period_ns{};
    std::atomic<u32> present_call_samples{};
    std::atomic<u64> timing_generation{};
    std::mutex feedback_mutex;
    static constexpr u32 PresentCallPeriodWindow = 7;
    std::array<s64, PresentCallPeriodWindow> present_call_period_history{};
    u32 present_call_period_history_index{};
    u32 present_call_period_history_size{};
    u64 presentation_epoch{};

    /// Guest frame the command processor is building.
    u64 gcp_frame_id{1};
    vk::UniqueSemaphore reflex_semaphore;
    u64 reflex_sleep_value{};
    bool reflex_timeout_logged{};

    /// Presentation thread state.
    u64 last_present_id{};

    struct PendingPresentWait {
        u64 present_id;
        u64 present_tick;
        u64 swapchain_serial;
        s64 latch_ns;
        s64 ready_ns;
    };
    static constexpr std::size_t MaxPendingPresentWaits = 8;
    std::mutex present_wait_mutex;
    std::condition_variable_any ready_wait_cv;
    std::condition_variable_any display_wait_cv;
    std::deque<PendingPresentWait> ready_wait_queue;
    std::deque<PendingPresentWait> display_wait_queue;

    /// What the vblank thread needs from the present waits.
    struct PacingState {
        bool locked;
        s64 display_period_ns;
        s64 lead_ns;
        /// Scanout time of the latest guest frame timed by a present wait.
        s64 anchor_display_ns;
        /// Latest guest frame done on the GPU and when.
        u64 ready_present_id;
        s64 ready_ns;
        /// Latest guest frame that left the presentation queue, timed or not.
        u64 displayed_present_id;
    };
    std::mutex pacing_mutex;
    PacingState pacing{};
    DisplayPacer display_pacer;
    std::atomic<bool> display_locked{};
    std::atomic<u64> correction_seq{};
    std::atomic<s64> correction_ns{};
    std::atomic<u32> slot_holds{};
    std::atomic<u32> backlog_holds{};
    bool pacing_log{};
    std::jthread present_ready_thread;
    std::jthread present_wait_thread;
};

} // namespace Vulkan