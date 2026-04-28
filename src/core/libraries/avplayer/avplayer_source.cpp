// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/alignment.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/file_sys/fs.h"
#include "core/libraries/avplayer/avplayer_error.h"
#include "core/libraries/avplayer/avplayer_file_streamer.h"
#include "core/libraries/avplayer/avplayer_source.h"
// FIX(GR2FORK): needed for Stop() to drain the GPU before freeing video buffers.
#include "video_core/renderer_vulkan/vk_presenter.h"

#include <magic_enum/magic_enum.hpp>

#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "common/support/avdec.h"

// FIX(GR2FORK): extern declaration at file/global scope so name resolves to
// the global-scope `presenter` defined elsewhere, not to a nonexistent
// `Libraries::AvPlayer::presenter`. Same pattern as video_out.cpp:16 and
// jpegenc.cpp:45.
extern std::unique_ptr<Vulkan::Presenter> presenter;

namespace Libraries::AvPlayer {

AvPlayerSource::AvPlayerSource(AvPlayerStateCallback& state, bool use_vdec2)
    : m_state(state), m_use_vdec2(use_vdec2) {}

AvPlayerSource::~AvPlayerSource() {
    Stop();
}

bool AvPlayerSource::Init(const AvPlayerInitData& init_data, std::string_view path) {
    m_memory_replacement = init_data.memory_replacement;
    m_max_num_video_framebuffers =
        std::min(std::max(2, init_data.num_output_video_framebuffers), 16);

    AVFormatContext* context = avformat_alloc_context();
    if (init_data.file_replacement.open != nullptr) {
        m_up_data_streamer = std::make_unique<AvPlayerFileStreamer>(init_data.file_replacement);
        if (!m_up_data_streamer->Init(path)) {
            return false;
        }
        context->pb = m_up_data_streamer->GetContext();
        if (AVPLAYER_IS_ERROR(avformat_open_input(&context, nullptr, nullptr, nullptr))) {
            return false;
        }
    } else {
        const auto mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
        const auto filepath = mnt->GetHostPath(path);
        if (AVPLAYER_IS_ERROR(
                avformat_open_input(&context, filepath.string().c_str(), nullptr, nullptr))) {
            return false;
        }
    }
    m_avformat_context = AVFormatContextPtr(context, &ReleaseAVFormatContext);
    return true;
}

bool AvPlayerSource::FindStreamInfo() {
    if (m_avformat_context == nullptr) {
        LOG_ERROR(Lib_AvPlayer, "Could not find stream info. NULL context.");
        return false;
    }
    if (m_avformat_context->nb_streams > 0) {
        return true;
    }
    return avformat_find_stream_info(m_avformat_context.get(), nullptr) == 0;
}

s32 AvPlayerSource::GetStreamCount() {
    if (m_avformat_context == nullptr) {
        LOG_ERROR(Lib_AvPlayer, "Could not get stream count. NULL context.");
        return -1;
    }
    LOG_INFO(Lib_AvPlayer, "Stream Count: {}", m_avformat_context->nb_streams);
    return m_avformat_context->nb_streams;
}

static AvPlayerStreamType CodecTypeToStreamType(AVMediaType codec_type) {
    switch (codec_type) {
    case AVMediaType::AVMEDIA_TYPE_VIDEO:
        return AvPlayerStreamType::Video;
    case AVMediaType::AVMEDIA_TYPE_AUDIO:
        return AvPlayerStreamType::Audio;
    case AVMediaType::AVMEDIA_TYPE_SUBTITLE:
        return AvPlayerStreamType::TimedText;
    default:
        LOG_ERROR(Lib_AvPlayer, "Unexpected AVMediaType {}", magic_enum::enum_name(codec_type));
        return AvPlayerStreamType::Unknown;
    }
}

bool AvPlayerSource::GetStreamInfo(u32 stream_index, AvPlayerStreamInfo& info) {
    info = {};
    if (m_avformat_context == nullptr || stream_index >= m_avformat_context->nb_streams) {
        LOG_ERROR(Lib_AvPlayer, "Could not get stream {} info.", stream_index);
        return false;
    }
    const auto p_stream = m_avformat_context->streams[stream_index];
    if (p_stream == nullptr || p_stream->codecpar == nullptr) {
        LOG_ERROR(Lib_AvPlayer, "Could not get stream {} info. NULL stream.", stream_index);
        return false;
    }
    info.type = CodecTypeToStreamType(p_stream->codecpar->codec_type);
    info.start_time = p_stream->start_time;
    info.duration = p_stream->duration;
    const auto p_lang_node = av_dict_get(p_stream->metadata, "language", nullptr, 0);
    if (p_lang_node != nullptr) {
        LOG_INFO(Lib_AvPlayer, "Stream {} language = {}", stream_index, p_lang_node->value);
    } else {
        LOG_WARNING(Lib_AvPlayer, "Stream {} language is unknown", stream_index);
    }
    switch (info.type) {
    case AvPlayerStreamType::Video: {
        LOG_INFO(Lib_AvPlayer, "Stream {} is a video stream.", stream_index);
        info.details.video.aspect_ratio =
            f32(p_stream->codecpar->width) / p_stream->codecpar->height;
        auto width = Common::AlignUp(u32(p_stream->codecpar->width), 64);
        auto height = Common::AlignUp(u32(p_stream->codecpar->height), 16);
        info.details.video.width = width;
        info.details.video.height = height;
        if (p_lang_node != nullptr) {
            std::memcpy(info.details.video.language_code, p_lang_node->value,
                        std::min(strlen(p_lang_node->value), size_t(3)));
        }
        break;
    }
    case AvPlayerStreamType::Audio: {
        LOG_INFO(Lib_AvPlayer, "Stream {} is an audio stream.", stream_index);
        info.details.audio.channel_count = p_stream->codecpar->ch_layout.nb_channels;
        info.details.audio.sample_rate = p_stream->codecpar->sample_rate;
        info.details.audio.size = 0; // sceAvPlayerGetStreamInfo() is expected to set this to 0
        if (p_lang_node != nullptr) {
            std::memcpy(info.details.audio.language_code, p_lang_node->value,
                        std::min(strlen(p_lang_node->value), size_t(3)));
        }
        break;
    }
    case AvPlayerStreamType::TimedText: {
        LOG_WARNING(Lib_AvPlayer, "Stream {} is a timedtext stream.", stream_index);
        info.details.subs.font_size = 12;
        info.details.subs.text_size = 12;
        if (p_lang_node != nullptr) {
            std::memcpy(info.details.subs.language_code, p_lang_node->value,
                        std::min(strlen(p_lang_node->value), size_t(3)));
        }
        break;
    }
    default: {
        LOG_ERROR(Lib_AvPlayer, "Stream {} type is unknown: {}.", stream_index,
                  magic_enum::enum_name(info.type));
        return false;
    }
    }
    return true;
}

bool AvPlayerSource::EnableStream(u32 stream_index) {
    if (m_avformat_context == nullptr || stream_index >= m_avformat_context->nb_streams) {
        return false;
    }
    const auto stream = m_avformat_context->streams[stream_index];
    switch (stream->codecpar->codec_type) {
    case AVMediaType::AVMEDIA_TYPE_VIDEO: {
        m_video_stream_index = stream_index;
        LOG_INFO(Lib_AvPlayer, "Video stream {} enabled", stream_index);
        break;
    }
    case AVMediaType::AVMEDIA_TYPE_AUDIO: {
        m_audio_stream_index = stream_index;
        LOG_INFO(Lib_AvPlayer, "Audio stream {} enabled", stream_index);
        break;
    }
    default:
        LOG_WARNING(Lib_AvPlayer, "Unknown stream type {} for stream {}",
                    magic_enum::enum_name(stream->codecpar->codec_type), stream_index);
        break;
    }
    return true;
}

void AvPlayerSource::SetLooping(bool is_looping) {
    m_is_looping = is_looping;
}

std::optional<bool> AvPlayerSource::HasFrames(u32 num_frames) {
    return m_video_packets.Size() > num_frames || m_is_eof;
}

bool AvPlayerSource::Start() {
    std::unique_lock lock(m_state_mutex);

    if (!m_video_stream_index && !m_audio_stream_index) {
        LOG_ERROR(Lib_AvPlayer, "Could not start playback. No streams.");
        return false;
    }
    if (m_video_stream_index) {
        const auto stream = m_avformat_context->streams[m_video_stream_index.value()];
        avformat_seek_file(m_avformat_context.get(), m_video_stream_index.value(), 0, 0,
                           stream->duration, 0);
        const auto decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (decoder == nullptr) {
            return false;
        }
        m_video_codec_context =
        AVCodecContextPtr(avcodec_alloc_context3(decoder), &ReleaseAVCodecContext);
        if (avcodec_parameters_to_context(m_video_codec_context.get(), stream->codecpar) < 0) {
            LOG_ERROR(Lib_AvPlayer, "Could not copy stream {} avcodec parameters to context.",
                      m_video_stream_index.value());
            return false;
        }

        // FIX(GR2FORK): Do NOT enable FF_THREAD_FRAME here.
        //
        // 3050 Ti silent window-closes during video transitions (pause menu,
        // title screen, tutorial popups) are caused by FFmpeg's frame-threading
        // worker pool. GR2 cycles Stop/Start rapidly as menus open and close
        // (28+ cycles captured in a single session). When Stop() frees the
        // codec context via the AVCodecContextPtr deleter, FFmpeg tries to
        // join its internal frame-threading workers. If a worker is mid-decode,
        // that teardown races with it and produces a read AV at offset 0x108
        // of a freed struct (the crash signature we saw: SEH ACCESS_VIOLATION
        // read 0x108 inside 0x7FFA... avcodec-*.dll).
        //
        // FF_THREAD_FRAME's benefit is ~5-10% decode speedup on large-resolution
        // video. GR2's menu videos are small (720p max) so the speedup is
        // imperceptible, but the teardown race is reliably fatal on slower
        // hardware. Single-threaded decode is correctness-over-speed here.
        m_video_codec_context->thread_count = 1;
        m_video_codec_context->thread_type = 0;

        if (avcodec_open2(m_video_codec_context.get(), decoder, nullptr) < 0) {
            LOG_ERROR(Lib_AvPlayer, "Could not open avcodec for video stream {}.",
                      m_video_stream_index.value());
            return false;
        }
        auto width = u32(m_video_codec_context->width);
        auto height = u32(m_video_codec_context->height);
        const auto pitch = Common::AlignUp(width, 64);
        const auto aligned_h = Common::AlignUp(height, 16);
        const auto size = pitch * aligned_h * 3 / 2;
        const auto luma_size = pitch * aligned_h;
        for (u64 index = 0; index < m_max_num_video_framebuffers; ++index) {
            auto buf = GuestBuffer(m_memory_replacement, 0x100, size, true);
            std::memset(buf.GetBuffer(), 0x00, luma_size);
            std::memset(buf.GetBuffer() + luma_size, 0x80, size - luma_size);
            m_video_buffers.Push(std::move(buf));
        }
    }
    if (m_audio_stream_index) {
        const auto stream = m_avformat_context->streams[m_audio_stream_index.value()];
        avformat_seek_file(m_avformat_context.get(), m_audio_stream_index.value(), 0, 0,
                           stream->duration, 0);
        const auto decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (decoder == nullptr) {
            return false;
        }
        m_audio_codec_context =
        AVCodecContextPtr(avcodec_alloc_context3(decoder), &ReleaseAVCodecContext);
        if (avcodec_parameters_to_context(m_audio_codec_context.get(), stream->codecpar) < 0) {
            LOG_ERROR(Lib_AvPlayer, "Could not copy stream {} avcodec parameters to context.",
                      m_audio_stream_index.value());
            return false;
        }

        // FIX(GR2FORK): same reasoning as video — no threading in audio decode
        // either. Audio codecs are cheap enough that single-thread decode is
        // well within budget. Removes one more class of teardown-race crash.
        m_audio_codec_context->thread_count = 1;
        m_audio_codec_context->thread_type = 0;

        if (avcodec_open2(m_audio_codec_context.get(), decoder, nullptr) < 0) {
            LOG_ERROR(Lib_AvPlayer, "Could not open avcodec for audio stream {}.",
                      m_audio_stream_index.value());
            return false;
        }
        const auto num_channels = m_audio_codec_context->ch_layout.nb_channels;
        const auto align = num_channels * sizeof(u16);
        const auto size = num_channels * sizeof(u16) * 1024;
        for (u64 index = 0; index < 8; ++index) {
            m_audio_buffers.Push(GuestBuffer(m_memory_replacement, 0x100, size, false));
        }
    }
    m_demuxer_thread.Run([this](std::stop_token stop) { this->DemuxerThread(stop); });
    m_video_decoder_thread.Run([this](std::stop_token stop) { this->VideoDecoderThread(stop); });
    m_audio_decoder_thread.Run([this](std::stop_token stop) { this->AudioDecoderThread(stop); });
    m_start_time = std::chrono::high_resolution_clock::now();
    return true;
}

bool AvPlayerSource::Stop() {
    std::unique_lock lock(m_state_mutex);

    // FIX(GR2FORK): US disc (CUSA03694) crashes with VK_ERROR_DEVICE_LOST at
    // vk_presenter.cpp:770 during newspaper comic cutscenes. The crash is a
    // race: comics cycle sceAvPlayerPlay/Pause/Stop rapidly, decoder threads
    // self-exit on EOF, and Stop() clears m_video_buffers below before the
    // GPU has finished sampling the last NV12 decode output. On EU disc
    // (CUSA04943) larger video frames mask this by giving the GPU time to
    // drain naturally; US disc's faster decode cycles lose the race.
    //
    // Drain all in-flight GPU work so no command buffer still references the
    // about-to-be-freed frame memory. waitIdle here is heavy but only runs
    // once per AvPlayer shutdown (cutscene transitions), so the cost is
    // negligible. Same precedent as the GR2 photo path in texture_cache.
    if (::presenter) {
        ::presenter->WaitIdle();
    }

    const bool had_threads = HasRunningThreads();

    if (had_threads) {
        if (m_up_data_streamer) {
            m_up_data_streamer->Reset();
        }

        m_video_decoder_thread.Stop();
        m_audio_decoder_thread.Stop();
        m_demuxer_thread.Stop();
    }

    // Always clean up resources, even when threads already self-terminated
    // (e.g. after EOF).  On a real PS4 stopping an already-finished player
    // is a successful no-op — returning false here previously caused
    // AvPlayerState::Stop() to skip its state transition, which could
    // prevent the game from properly cycling the handle.
    m_current_audio_frame.reset();
    m_current_video_frame.reset();

    m_video_buffers.Clear();
    m_audio_buffers.Clear();
    m_audio_packets.Clear();
    m_video_packets.Clear();
    m_audio_frames.Clear();
    m_video_frames.Clear();

    // FIX(GR2FORK): Explicitly tear down codec contexts HERE, inside Stop(),
    // while m_state_mutex is held and our decoder threads are guaranteed
    // joined. Previously these were only freed when either the source object
    // was destroyed, or when Start() reassigned the unique_ptr (which
    // triggered the custom deleter → avcodec_free_context → FFmpeg internal
    // teardown).
    //
    // The latter path was racing with the 3050 Ti's narrower timing window:
    // Start() reassigned m_video_codec_context while some peripheral FFmpeg
    // reference (packet, parser, etc.) was still in flight, producing a
    // read AV at offset 0x108 of a freed AVCodecContext (crash signature
    // observed in the log). Tearing the context down here, while we still
    // hold the mutex and before Start() has any chance to run, eliminates
    // that window.
    //
    // Also reset the SW scaler/resampler — they hold references back into
    // the codec contexts' pixel/sample formats.
    m_sws_context.reset();
    m_swr_context.reset();
    m_video_codec_context.reset();
    m_audio_codec_context.reset();

    m_last_audio_ts.reset();
    m_start_time.reset();
    m_pause_time = {};
    m_pause_duration = {};

    m_is_paused = false;
    m_is_eof = false;

    return true;
}

void AvPlayerSource::Pause() {
    m_pause_time = std::chrono::high_resolution_clock::now();
    m_is_paused = true;
}

void AvPlayerSource::Resume() {
    m_pause_duration += std::chrono::high_resolution_clock::now() - m_pause_time;
    m_is_paused = false;
}

bool AvPlayerSource::GetVideoData(AvPlayerFrameInfo& video_info) {
    AvPlayerFrameInfoEx info{};
    if (!GetVideoData(info)) {
        return false;
    }
    video_info = {};
    video_info.timestamp = u64(info.timestamp);
    video_info.p_data = reinterpret_cast<u8*>(info.p_data);
    video_info.details.video.aspect_ratio = info.details.video.aspect_ratio;
    video_info.details.video.width = info.details.video.width;
    video_info.details.video.height = info.details.video.height;
    return true;
}

bool AvPlayerSource::GetVideoData(AvPlayerFrameInfoEx& video_info) {
    if (m_current_video_frame.has_value()) {
        m_video_buffers.Push(std::move(m_current_video_frame->buffer));
        m_current_video_frame.reset();
        m_video_buffers_cv.Notify();
    }

    if (!IsActive() || m_is_paused) {
        return false;
    }

    // FIX(GR2FORK v4): peek-then-decide via TryPeek (atomic under the
    // queue's mutex) instead of unlocked Size() + Front(). Previously
    // the game thread took a raw reference to m_video_frames.front()
    // and then the DemuxerThread's loop-reset drain could empty the
    // queue under it → dangling reference, wild read on .info access.
    AvPlayerFrameInfoEx head_info{};
    if (!m_video_frames.TryPeek([&](const Frame& f) { head_info = f.info; })) {
        return false;
    }

    if (m_state.GetSyncMode() == AvPlayerAvSyncMode::Default) {
        if (m_audio_stream_index) {
            const auto audio_ts = m_last_audio_ts.value_or(0);
            if (head_info.timestamp > audio_ts) {
                // Head frame not yet due — leave it in queue, try later.
                return false;
            }
            // Skip stale frames: keep popping while the NEW head is also
            // still ≤ audio_ts. Return the last popped candidate (newest
            // frame audio has already passed).
            std::optional<Frame> candidate;
            while (true) {
                auto popped = m_video_frames.Pop();
                if (!popped.has_value()) break;
                if (candidate.has_value()) {
                    // Discard previously-held candidate — a newer one is available.
                    m_video_buffers.Push(std::move(candidate->buffer));
                    m_video_buffers_cv.Notify();
                }
                candidate = std::move(popped);
                AvPlayerFrameInfoEx next_info{};
                const bool have_next = m_video_frames.TryPeek(
                    [&](const Frame& f) { next_info = f.info; });
                if (!have_next || next_info.timestamp > audio_ts) break;
            }
            if (!candidate.has_value()) return false;
            video_info = candidate->info;
            m_current_video_frame = std::move(candidate);
            return true;
        } else {
            const auto current_time = CurrentTime();
            if (0 < current_time && current_time < head_info.timestamp) {
                return false;
            }
            // Same skip-stale-frames pattern as the audio-synced branch.
            std::optional<Frame> candidate;
            while (true) {
                auto popped = m_video_frames.Pop();
                if (!popped.has_value()) break;
                if (candidate.has_value()) {
                    m_video_buffers.Push(std::move(candidate->buffer));
                    m_video_buffers_cv.Notify();
                }
                candidate = std::move(popped);
                AvPlayerFrameInfoEx next_info{};
                const bool have_next = m_video_frames.TryPeek(
                    [&](const Frame& f) { next_info = f.info; });
                if (!have_next || current_time < next_info.timestamp) break;
            }
            if (!candidate.has_value()) return false;
            video_info = candidate->info;
            m_current_video_frame = std::move(candidate);
            return true;
        }
    }

    // Non-default sync mode: just return the oldest frame.
    auto frame = m_video_frames.Pop();
    if (!frame.has_value()) return false; // raced empty — try again next tick
    video_info = frame->info;
    m_current_video_frame = std::move(frame);
    return true;
}

bool AvPlayerSource::GetAudioData(AvPlayerFrameInfo& audio_info) {
    if (m_current_audio_frame.has_value()) {
        // return the buffer to the queue
        m_audio_buffers.Push(std::move(m_current_audio_frame->buffer));
        m_current_audio_frame.reset();
        m_audio_buffers_cv.Notify();
    }

    if (!IsActive() || m_is_paused) {
        return false;
    }

    // FIX(GR2FORK v4): atomic Pop with has_value check. The previous
    // Size() == 0 early-return followed by an unchecked Pop() was a
    // TOCTOU — if the demuxer drained m_audio_frames between the size
    // check and the Pop (which happens on loop reset), Pop() returns
    // nullopt and the subsequent frame->info.timestamp dereference was UB.
    auto frame = m_audio_frames.Pop();
    if (!frame.has_value()) {
        return false;
    }
    m_last_audio_ts = frame->info.timestamp;

    audio_info = {};
    audio_info.timestamp = frame->info.timestamp;
    audio_info.p_data = reinterpret_cast<u8*>(frame->info.p_data);
    audio_info.details.audio.sample_rate = frame->info.details.audio.sample_rate;
    audio_info.details.audio.size = frame->info.details.audio.size;
    audio_info.details.audio.channel_count = frame->info.details.audio.channel_count;
    m_current_audio_frame = std::move(frame);
    return true;
}

u64 AvPlayerSource::CurrentTime() {
    if (!IsActive() || !m_start_time.has_value()) {
        return 0;
    }
    using namespace std::chrono;
    return duration_cast<milliseconds>(high_resolution_clock::now() - m_start_time.value() -
                                       m_pause_duration)
        .count();
}

bool AvPlayerSource::IsActive() {
    return !m_is_eof || m_audio_packets.Size() != 0 || m_video_packets.Size() != 0 ||
           m_video_frames.Size() != 0 || m_audio_frames.Size() != 0;
}

void AvPlayerSource::ReleaseAVPacket(AVPacket* packet) {
    if (packet != nullptr) {
        av_packet_free(&packet);
    }
}

void AvPlayerSource::ReleaseAVFrame(AVFrame* frame) {
    if (frame != nullptr) {
        av_frame_free(&frame);
    }
}

void AvPlayerSource::ReleaseAVCodecContext(AVCodecContext* context) {
    if (context != nullptr) {
        avcodec_free_context(&context);
    }
}

void AvPlayerSource::ReleaseSWRContext(SwrContext* context) {
    if (context != nullptr) {
        swr_free(&context);
    }
}

void AvPlayerSource::ReleaseSWSContext(SwsContext* context) {
    if (context != nullptr) {
        sws_freeContext(context);
    }
}

void AvPlayerSource::ReleaseAVFormatContext(AVFormatContext* context) {
    if (context != nullptr) {
        avformat_close_input(&context);
    }
}

void AvPlayerSource::DemuxerThread(std::stop_token stop) {
    using namespace std::chrono;
    Common::SetCurrentThreadName("shadPS4:AvDemuxer");

    if (!m_audio_stream_index.has_value() && !m_video_stream_index.has_value()) {
        LOG_WARNING(Lib_AvPlayer, "Could not start DEMUXER thread. No streams enabled.");
        return;
    }
    LOG_INFO(Lib_AvPlayer, "Demuxer Thread started");

    while (!stop.stop_requested()) {
        if (m_video_packets.Size() > 30 &&
            (!m_audio_stream_index.has_value() || m_audio_packets.Size() > 8)) {
            std::this_thread::sleep_for(milliseconds(5));
            continue;
        }
        AVPacketPtr up_packet(av_packet_alloc(), &ReleaseAVPacket);
        const auto res = av_read_frame(m_avformat_context.get(), up_packet.get());
        if (res < 0) {
            if (res == AVERROR_EOF) {
                if (m_is_looping) {
                    LOG_INFO(Lib_AvPlayer, "EOF reached in demuxer. Looping the source...");
                    m_state.OnWarning(ORBIS_AVPLAYER_ERROR_WAR_LOOPING_BACK);
                    avio_seek(m_avformat_context->pb, 0, SEEK_SET);
                    if (m_video_stream_index.has_value()) {
                        const auto index = m_video_stream_index.value();
                        const auto stream = m_avformat_context->streams[index];
                        avformat_seek_file(m_avformat_context.get(), index, 0, 0, stream->duration,
                                           0);
                    }
                    if (m_audio_stream_index.has_value()) {
                        const auto index = m_audio_stream_index.value();
                        const auto stream = m_avformat_context->streams[index];
                        avformat_seek_file(m_avformat_context.get(), index, 0, 0, stream->duration,
                                           0);
                    }
                    // Reset timing references so the new loop's timestamps
                    // (starting from 0) stay in sync with the wall clock.
                    // Without this, CurrentTime() drifts ahead each loop
                    // and the frame-skip logic drops increasingly more frames.
                    m_start_time = std::chrono::high_resolution_clock::now();
                    m_pause_duration = {};
                    m_last_audio_ts.reset();

                    // FIX(GR2FORK v4): while-pop pattern; Size()-then-Pop()
                    // was a TOCTOU race against the game thread's
                    // GetVideoData path, which also pops from
                    // m_video_frames. With atomic Pop() a nullopt
                    // cleanly terminates the drain loop.
                    while (auto old = m_video_frames.Pop()) {
                        m_video_buffers.Push(std::move(old->buffer));
                    }
                    m_video_buffers_cv.Notify();
                    while (auto old = m_audio_frames.Pop()) {
                        m_audio_buffers.Push(std::move(old->buffer));
                    }
                    m_audio_buffers_cv.Notify();

                    // FIX(GR2FORK v4): codec context access must be
                    // serialized with the decoder threads' send_packet
                    // / receive_frame calls — FFmpeg requires single-
                    // thread access to an AVCodecContext. Unlocked
                    // flush_buffers here is the primary suspect for
                    // pause-menu-idle silent death: each video loop
                    // iteration (every few seconds for a short menu
                    // video) gives the race another chance, and over
                    // a ~45 minute idle that is hundreds of chances.
                    // Flush holds each mutex for only microseconds so
                    // the decoder threads are blocked imperceptibly.
                    if (m_video_codec_context) {
                        std::lock_guard lk(m_video_codec_mutex);
                        avcodec_flush_buffers(m_video_codec_context.get());
                    }
                    if (m_audio_codec_context) {
                        std::lock_guard lk(m_audio_codec_mutex);
                        avcodec_flush_buffers(m_audio_codec_context.get());
                    }

                    continue;
                } else {
                    LOG_INFO(Lib_AvPlayer, "EOF reached in demuxer. Exiting.");
                    break;
                }
            } else {
                LOG_ERROR(Lib_AvPlayer, "Could not read AV frame: error = {}", res);
                m_state.OnError();
                return;
            }
            break;
        }
        if (up_packet->stream_index == m_video_stream_index) {
            m_video_packets.Push(std::move(up_packet));
            m_video_packets_cv.Notify();
        } else if (up_packet->stream_index == m_audio_stream_index) {
            m_audio_packets.Push(std::move(up_packet));
            m_audio_packets_cv.Notify();
        }
    }

    m_is_eof = true;

    m_video_packets_cv.Notify();
    m_audio_packets_cv.Notify();
    m_video_frames_cv.Notify();
    m_audio_frames_cv.Notify();

    m_video_decoder_thread.Join();
    m_audio_decoder_thread.Join();
    m_state.OnEOF();

    LOG_INFO(Lib_AvPlayer, "Demuxer Thread exited normally");
}

AvPlayerSource::AVFramePtr AvPlayerSource::ConvertVideoFrame(const AVFrame& frame) {
    auto nv12_frame = AVFramePtr{av_frame_alloc(), &ReleaseAVFrame};
    nv12_frame->pts = frame.pts;
    nv12_frame->pkt_dts = frame.pkt_dts < 0 ? 0 : frame.pkt_dts;
    nv12_frame->format = AV_PIX_FMT_NV12;
    nv12_frame->width = frame.width;
    nv12_frame->height = frame.height;
    nv12_frame->sample_aspect_ratio = frame.sample_aspect_ratio;
    nv12_frame->crop_top = frame.crop_top;
    nv12_frame->crop_bottom = frame.crop_bottom;
    nv12_frame->crop_left = frame.crop_left;
    nv12_frame->crop_right = frame.crop_right;

    av_frame_get_buffer(nv12_frame.get(), 0);

    if (m_sws_context == nullptr) {
        m_sws_context =
            SWSContextPtr(sws_getContext(frame.width, frame.height, AVPixelFormat(frame.format),
                                         nv12_frame->width, nv12_frame->height, AV_PIX_FMT_NV12,
                                         SWS_FAST_BILINEAR, nullptr, nullptr, nullptr),
                          &ReleaseSWSContext);
    }
    const auto res = sws_scale(m_sws_context.get(), frame.data, frame.linesize, 0, frame.height,
                               nv12_frame->data, nv12_frame->linesize);
    if (res < 0) {
        LOG_ERROR(Lib_AvPlayer, "Could not convert to NV12: {}", av_err2str(res));
        return AVFramePtr{nullptr, &ReleaseAVFrame};
    }
    return nv12_frame;
}

static void CopyNV12Data(u8* dst, const AVFrame& src) {
    const u32 content_w = u32(src.width);
    const u32 content_h = u32(src.height);
    const u32 pitch = Common::AlignUp(content_w, 64);
    const u32 height = Common::AlignUp(content_h, 16);

    u8* luma_dst = dst;
    u8* chroma_dst = dst + pitch * height;

    const u32 src_luma_pitch = u32(src.linesize[0]);
    const u32 src_chroma_pitch = u32(src.linesize[1]);

    for (u32 y = 0; y < content_h; ++y) {
        std::memcpy(luma_dst + y * pitch, src.data[0] + y * src_luma_pitch, content_w);
    }

    const u32 chroma_rows = content_h / 2;
    for (u32 y = 0; y < chroma_rows; ++y) {
        std::memcpy(chroma_dst + y * pitch, src.data[1] + y * src_chroma_pitch, content_w);
    }
}

Frame AvPlayerSource::PrepareVideoFrame(GuestBuffer buffer, const AVFrame& frame) {
    ASSERT(frame.format == AV_PIX_FMT_NV12);

    const u32 content_w = u32(frame.width);
    const u32 content_h = u32(frame.height);
    const u32 pitch = Common::AlignUp(content_w, 64);
    const u32 height = Common::AlignUp(content_h, 16);

    auto p_buffer = buffer.GetBuffer();

    const u32 luma_size = pitch * height;
    const u32 chroma_size = pitch * (height / 2);
    std::memset(p_buffer, 0x00, luma_size);
    std::memset(p_buffer + luma_size, 0x80, chroma_size);

    CopyNV12Data(p_buffer, frame);

    const auto pkt_dts = u64(frame.pkt_dts) * 1000;
    const auto stream = m_avformat_context->streams[m_video_stream_index.value()];
    const auto time_base = stream->time_base;
    const auto den = time_base.den;
    const auto num = time_base.num;
    const auto timestamp = (num != 0 && den > 1) ? (pkt_dts * num) / den : pkt_dts;

    return Frame{
        .buffer = std::move(buffer),
        .info =
            {
                .p_data = p_buffer,
                .timestamp = timestamp,
                .details =
                    {
                        .video =
                            {
                                .width = pitch,
                                .height = height,
                                .aspect_ratio = (float)av_q2d(frame.sample_aspect_ratio),
                                .crop_left_offset = 0,
                                .crop_right_offset = pitch - content_w,
                                .crop_top_offset = 0,
                                .crop_bottom_offset = height - content_h,
                                .pitch = pitch,
                                .luma_bit_depth = 8,
                                .chroma_bit_depth = 8,
                            },
                    },
            },
    };
}

void AvPlayerSource::VideoDecoderThread(std::stop_token stop) {
    using namespace std::chrono;
    Common::SetCurrentThreadName("shadPS4:AvVideoDecoder");

    LOG_INFO(Lib_AvPlayer, "Video Decoder Thread started");
    while ((!m_is_eof || m_video_packets.Size() != 0) && !stop.stop_requested()) {
        if (!m_video_packets_cv.Wait(stop,
                                     [this] { return m_video_packets.Size() != 0 || m_is_eof; })) {
            continue;
        }
        const auto packet = m_video_packets.Pop();
        if (!packet.has_value()) {
            continue;
        }

        // FIX(GR2FORK v4): lock codec context for the FFmpeg call —
        // the demuxer thread may be calling avcodec_flush_buffers on
        // the same context during a loop reset, and FFmpeg requires
        // single-thread access.
        int res;
        {
            std::lock_guard lk(m_video_codec_mutex);
            res = avcodec_send_packet(m_video_codec_context.get(), packet->get());
        }
        if (res < 0 && res != AVERROR(EAGAIN)) {
            m_state.OnError();
            LOG_ERROR(Lib_AvPlayer, "Could not send packet to the video codec. Error = {}",
                      av_err2str(res));
            return;
        }
        auto up_frame = AVFramePtr(av_frame_alloc(), &ReleaseAVFrame);

        while (res >= 0) {
            if (!m_video_buffers_cv.Wait(stop, [this] { return m_video_buffers.Size() != 0; })) {
                break;
            }
            if (m_video_buffers.Size() == 0) {
                continue;
            }
            av_frame_unref(up_frame.get());
            // FIX(GR2FORK v4): lock codec context — see send_packet above.
            {
                std::lock_guard lk(m_video_codec_mutex);
                res = avcodec_receive_frame(m_video_codec_context.get(), up_frame.get());
            }
            if (res < 0) {
                if (res == AVERROR_EOF) {
                    LOG_INFO(Lib_AvPlayer, "EOF reached in video decoder");
                    return;
                } else if (res != AVERROR(EAGAIN)) {
                    LOG_ERROR(Lib_AvPlayer,
                              "Could not receive frame from the video codec. Error = {}",
                              av_err2str(res));
                    m_state.OnError();
                    return;
                }
            } else {
                auto buffer = m_video_buffers.Pop();
                if (!buffer.has_value()) {
                    // Video buffers queue was cleared. This means that player was stopped.
                    break;
                }
                if (up_frame->format != AV_PIX_FMT_NV12) {
                    const auto nv12_frame = ConvertVideoFrame(*up_frame);
                    m_video_frames.Push(PrepareVideoFrame(std::move(buffer.value()), *nv12_frame));
                } else {
                    m_video_frames.Push(PrepareVideoFrame(std::move(buffer.value()), *up_frame));
                }
                m_video_frames_cv.Notify();
            }
        }
    }

    LOG_INFO(Lib_AvPlayer, "Video Decoder Thread exited normally");
}

AvPlayerSource::AVFramePtr AvPlayerSource::ConvertAudioFrame(const AVFrame& frame) {
    auto pcm16_frame = AVFramePtr{av_frame_alloc(), &ReleaseAVFrame};
    pcm16_frame->pts = frame.pts;
    pcm16_frame->pkt_dts = frame.pkt_dts < 0 ? 0 : frame.pkt_dts;
    pcm16_frame->format = AV_SAMPLE_FMT_S16;
    pcm16_frame->ch_layout = frame.ch_layout;
    pcm16_frame->sample_rate = frame.sample_rate;

    if (m_swr_context == nullptr) {
        SwrContext* swr_context = nullptr;
        AVChannelLayout in_ch_layout = frame.ch_layout;
        AVChannelLayout out_ch_layout = frame.ch_layout;
        swr_alloc_set_opts2(&swr_context, &out_ch_layout, AV_SAMPLE_FMT_S16, frame.sample_rate,
                            &in_ch_layout, AVSampleFormat(frame.format), frame.sample_rate, 0,
                            nullptr);
        m_swr_context = SWRContextPtr(swr_context, &ReleaseSWRContext);
        swr_init(m_swr_context.get());
    }
    const auto res = swr_convert_frame(m_swr_context.get(), pcm16_frame.get(), &frame);
    if (res < 0) {
        LOG_ERROR(Lib_AvPlayer, "Could not convert to NV12: {}", av_err2str(res));
        return AVFramePtr{nullptr, &ReleaseAVFrame};
    }
    return pcm16_frame;
}

Frame AvPlayerSource::PrepareAudioFrame(GuestBuffer buffer, const AVFrame& frame) {
    ASSERT(frame.format == AV_SAMPLE_FMT_S16);
    ASSERT(frame.nb_samples <= 1024);

    auto p_buffer = buffer.GetBuffer();
    const auto size = frame.ch_layout.nb_channels * frame.nb_samples * sizeof(u16);
    std::memcpy(p_buffer, frame.data[0], size);

    const auto pkt_dts = u64(frame.pkt_dts) * 1000;
    const auto stream = m_avformat_context->streams[m_audio_stream_index.value()];
    const auto time_base = stream->time_base;
    const auto den = time_base.den;
    const auto num = time_base.num;
    const auto timestamp = (num != 0 && den > 1) ? (pkt_dts * num) / den : pkt_dts;

    return Frame{
        .buffer = std::move(buffer),
        .info =
            {
                .p_data = p_buffer,
                .timestamp = timestamp,
                .details =
                    {
                        .audio =
                            {
                                .channel_count = u16(frame.ch_layout.nb_channels),
                                .sample_rate = u32(frame.sample_rate),
                                .size = u32(size),
                            },
                    },
            },
    };
}

void AvPlayerSource::AudioDecoderThread(std::stop_token stop) {
    using namespace std::chrono;
    Common::SetCurrentThreadName("shadPS4:AvAudioDecoder");

    LOG_INFO(Lib_AvPlayer, "Audio Decoder Thread started");
    while ((!m_is_eof || m_audio_packets.Size() != 0) && !stop.stop_requested()) {
        if (!m_audio_packets_cv.Wait(stop,
                                     [this] { return m_audio_packets.Size() != 0 || m_is_eof; })) {
            continue;
        }
        const auto packet = m_audio_packets.Pop();
        if (!packet.has_value()) {
            continue;
        }
        // FIX(GR2FORK v4): lock codec context — same rationale as
        // video decoder thread above.
        int res;
        {
            std::lock_guard lk(m_audio_codec_mutex);
            res = avcodec_send_packet(m_audio_codec_context.get(), packet->get());
        }
        if (res < 0 && res != AVERROR(EAGAIN)) {
            m_state.OnError();
            LOG_ERROR(Lib_AvPlayer, "Could not send packet to the audio codec. Error = {}",
                      av_err2str(res));
            return;
        }
        auto up_frame = AVFramePtr(av_frame_alloc(), &ReleaseAVFrame);

        while (res >= 0) {
            if (!m_audio_buffers_cv.Wait(stop, [this] { return m_audio_buffers.Size() != 0; })) {
                break;
            }
            if (m_audio_buffers.Size() == 0) {
                continue;
            }

            av_frame_unref(up_frame.get());
            // FIX(GR2FORK v4): lock codec context — see send_packet above.
            {
                std::lock_guard lk(m_audio_codec_mutex);
                res = avcodec_receive_frame(m_audio_codec_context.get(), up_frame.get());
            }
            if (res < 0) {
                if (res == AVERROR_EOF) {
                    LOG_INFO(Lib_AvPlayer, "EOF reached in audio decoder");
                    return;
                } else if (res != AVERROR(EAGAIN)) {
                    m_state.OnError();
                    LOG_ERROR(Lib_AvPlayer,
                              "Could not receive frame from the audio codec. Error = {}",
                              av_err2str(res));
                    return;
                }
            } else {
                auto buffer = m_audio_buffers.Pop();
                if (!buffer.has_value()) {
                    // Audio buffers queue was cleared. This means that player was stopped.
                    break;
                }
                if (up_frame->format != AV_SAMPLE_FMT_S16) {
                    const auto pcm16_frame = ConvertAudioFrame(*up_frame);
                    m_audio_frames.Push(PrepareAudioFrame(std::move(buffer.value()), *pcm16_frame));
                } else {
                    m_audio_frames.Push(PrepareAudioFrame(std::move(buffer.value()), *up_frame));
                }
                m_audio_frames_cv.Notify();
            }
        }
    }

    LOG_INFO(Lib_AvPlayer, "Audio Decoder Thread exited normally");
}

bool AvPlayerSource::HasRunningThreads() const {
    return m_demuxer_thread.Joinable() || m_video_decoder_thread.Joinable() ||
           m_audio_decoder_thread.Joinable();
}

} // namespace Libraries::AvPlayer
